/* SPDX-License-Identifier: MIT */
/* Small boot steps: the host name, the random seed, the system clock, the machine id. */
#define _GNU_SOURCE
#include "boot.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_HOSTNAME "cleanlinux"
#define SEED_FILE STATE_DIR "/random-seed"
#define SEED_SIZE 512
#define CLOCK_FILE STATE_DIR "/clock"

/* ---- hostname ---- */

/* the configured host name: the first non-comment line of /etc/hostname */
static char *configured_hostname(void)
{
	char *p = rpath("/etc/hostname");
	char *s = read_file(p, NULL);
	free(p);
	if (!s)
		return NULL;
	char *name = NULL;
	for (char *l = strtok(s, "\n"); l; l = strtok(NULL, "\n")) {
		l = trim(l);
		if (*l && *l != '#') {
			name = xstrdup(l);
			break;
		}
	}
	free(s);
	return name;
}

static bool valid_hostname(const char *s)
{
	size_t n = strlen(s);
	if (n == 0 || n > 64 || *s == '-' || *s == '.')
		return false;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
		      *s == '-' || *s == '.'))
			return false;
	return true;
}

int cmd_hostname(int argc, char **argv)
{
	bool dry = argc > 1 && !strcmp(argv[1], "--print");
	char *name = configured_hostname();
	if (name && !valid_hostname(name)) {
		warnx("/etc/hostname: invalid host name \"%s\", using %s", name, DEFAULT_HOSTNAME);
		free(name);
		name = NULL;
	}
	if (!name)
		name = xstrdup(DEFAULT_HOSTNAME);
	if (dry) {
		puts(name);
		free(name);
		return 0;
	}
	int rc = 0;
	if (sethostname(name, strlen(name)) < 0) {
		warn_errno("sethostname %s", name);
		rc = 1;
	}
	free(name);
	return rc;
}

/* ---- random seed ---- */

static int seed_save(void)
{
	unsigned char buf[SEED_SIZE];
	size_t n = 0;
	while (n < sizeof(buf)) {
		/* GRND_INSECURE: never blocks, and at shutdown the pool is initialised anyway */
		ssize_t r = getrandom(buf + n, sizeof(buf) - n, GRND_INSECURE);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EINVAL) /* kernels before 5.6 */
				r = getrandom(buf + n, sizeof(buf) - n, GRND_NONBLOCK);
			if (r < 0) {
				warn_errno("getrandom");
				return 1;
			}
		}
		n += (size_t)r;
	}
	char *dir = rpath(STATE_DIR), *p = rpath(SEED_FILE);
	mkdir_p(dir, 0755);
	int rc = write_file(p, buf, sizeof(buf), 0600);
	if (rc < 0)
		warn_errno("%s", p);
	explicit_bzero(buf, sizeof(buf));
	free(dir);
	free(p);
	return rc < 0;
}

static int seed_load(void)
{
	char *p = rpath(SEED_FILE);
	size_t len = 0;
	char *seed = read_file(p, &len);
	if (!seed) {
		if (errno != ENOENT)
			warn_errno("%s", p);
		free(p);
		return 0; /* the first boot: nothing to load yet */
	}
	/* written to /dev/urandom it is mixed into the pool without being credited as entropy:
	 * a seed restored from a disk image must not be trusted, only added */
	char *dev = rpath("/dev/urandom");
	int fd = open(dev, O_WRONLY | O_CLOEXEC | O_NOCTTY);
	int rc = 0;
	if (fd < 0 || write(fd, seed, len) != (ssize_t)len) {
		warn_errno("%s", dev);
		rc = 1;
	}
	if (fd >= 0)
		close(fd);
	explicit_bzero(seed, len);
	free(seed);
	free(dev);
	free(p);
	/* a fresh seed right away: the next boot never reuses this one, even after a crash */
	if (seed_save())
		rc = 1;
	return rc;
}

int cmd_seed(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "load"))
		return seed_load();
	if (argc == 2 && !strcmp(argv[1], "save"))
		return seed_save();
	die("usage: seed load|save");
}

/* ---- clock ---- */

/* the hardware clock keeps local time (Windows dual boot): third line of adjtime is LOCAL */
static bool rtc_local(void)
{
	static const char *const paths[] = {"/etc/adjtime", "/var/lib/hwclock/adjtime"};
	for (size_t i = 0; i < ARRAY_LEN(paths); i++) {
		char *p = rpath(paths[i]);
		char *s = read_file(p, NULL);
		free(p);
		if (!s)
			continue;
		char *save = NULL, *l = strtok_r(s, "\n", &save);
		for (int n = 1; l && n < 3; n++)
			l = strtok_r(NULL, "\n", &save);
		bool local = l && !strcmp(trim(l), "LOCAL");
		free(s);
		return local;
	}
	return false;
}

static int hwclock(const char *op)
{
	char *rtc = rpath("/dev/rtc0");
	bool have_rtc = exists(rtc);
	free(rtc);
	char *hw = find_program("hwclock");
	if (!have_rtc || !hw) {
		free(hw);
		return 0;
	}
	int rc = runl(hw, op, rtc_local() ? "--localtime" : "--utc", (char *)NULL);
	free(hw);
	return rc;
}

/* never earlier than the build of this program or the last clock saved on this machine:
 * boards without a battery-backed RTC (and dead batteries) start in 1970, which breaks
 * TLS certificates, make, and fsck's "last mount time is in the future" */
static int clock_floor(void)
{
	time_t floor = (time_t)BUILD_EPOCH;
	struct stat st;
	char *p = rpath(CLOCK_FILE);
	if (stat(p, &st) == 0 && st.st_mtime > floor)
		floor = st.st_mtime;
	free(p);
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	if (now.tv_sec >= floor)
		return 0;
	struct timespec ts = {.tv_sec = floor, .tv_nsec = 0};
	if (clock_settime(CLOCK_REALTIME, &ts) < 0) {
		warn_errno("clock_settime");
		return 1;
	}
	char buf[64];
	struct tm tm;
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", gmtime_r(&floor, &tm));
	msg("the clock was behind, moved forward to %s", buf);
	return 0;
}

static int clock_load(void)
{
	int rc = 0;
	/* the kernel set the clock from the RTC as UTC (CONFIG_RTC_HCTOSYS); local time needs
	 * hwclock to read it again, with the time zone */
	if (rtc_local() && hwclock("--hctosys") != 0)
		rc = 1;
	if (clock_floor())
		rc = 1;
	return rc;
}

static int clock_save(void)
{
	int rc = 0;
	char *dir = rpath(STATE_DIR), *p = rpath(CLOCK_FILE);
	mkdir_p(dir, 0755);
	if (write_file(p, "", 0, 0644) < 0) { /* the time stamp is the file's mtime */
		warn_errno("%s", p);
		rc = 1;
	}
	free(dir);
	free(p);
	if (hwclock("--systohc") != 0)
		rc = 1;
	return rc;
}

int cmd_clock(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "load"))
		return clock_load();
	if (argc == 2 && !strcmp(argv[1], "save"))
		return clock_save();
	die("usage: clock load|save");
}

/* ---- machine id ---- */

static bool valid_machine_id(const char *s)
{
	if (strlen(s) != 32)
		return false;
	for (; *s; s++)
		if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f')))
			return false;
	return true;
}

/* /etc/machine-id: 128 random bits as 32 hex digits (a version 4 UUID without dashes), made
 * once on the first boot; images ship without it so that every installation gets its own */
int cmd_machine_id(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	char *p = rpath("/etc/machine-id");
	char *id = read_line(p);
	if (id && valid_machine_id(id)) {
		free(id);
		free(p);
		return 0;
	}
	free(id);
	unsigned char b[16];
	if (getrandom(b, sizeof(b), 0) != (ssize_t)sizeof(b))
		die("getrandom: %s", strerror(errno));
	b[6] = (b[6] & 0x0f) | 0x40;
	b[8] = (b[8] & 0x3f) | 0x80;
	char hex[34];
	for (size_t i = 0; i < sizeof(b); i++)
		snprintf(hex + 2 * i, 3, "%02x", b[i]);
	hex[32] = '\n';
	hex[33] = '\0';
	int rc = 0;
	if (write_file(p, hex, 33, 0444) < 0) {
		warn_errno("%s", p);
		rc = 1;
	}
	free(p);
	return rc;
}
