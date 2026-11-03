/* SPDX-License-Identifier: MIT */
/* Devices and terminals: the udev coldplug, getty on virtual terminals and serial consoles. */
#define _GNU_SOURCE
#include "boot.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- udev ---- */

/* newest modification time of the hwdb sources */
static time_t hwdb_sources_mtime(void)
{
	strv files = STRV_INIT;
	conf_files("udev/hwdb.d", ".hwdb", &files);
	time_t newest = 0;
	struct stat st;
	for (size_t i = 0; i < files.n; i++)
		if (stat(files.v[i], &st) == 0 && st.st_mtime > newest)
			newest = st.st_mtime;
	strv_free(&files);
	return newest;
}

/* the compiled hwdb (/etc/udev/hwdb.bin, else /usr/lib/udev/hwdb.bin) is missing or older
 * than its sources: an image built without running the target's udevadm, or hwdb files
 * added by hand */
static bool hwdb_stale(void)
{
	time_t src = hwdb_sources_mtime();
	if (!src)
		return false;
	static const char *const bins[] = {"/etc/udev/hwdb.bin", "/usr/lib/udev/hwdb.bin"};
	for (size_t i = 0; i < ARRAY_LEN(bins); i++) {
		char *p = rpath(bins[i]);
		struct stat st;
		int r = stat(p, &st);
		free(p);
		if (r == 0)
			return st.st_mtime < src;
	}
	return true;
}

/* udev-trigger: compile the hwdb when needed, then replay the "add" events of every device
 * the kernel found before udevd ran (subsystems first, so that buses exist for devices) */
int cmd_udev(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	char *udevadm = find_program("udevadm");
	if (!udevadm) {
		warnx("udevadm is not installed");
		return 1;
	}
	/* udevd listens once its control socket is there; events sent before that are lost */
	char *ctl = rpath("/run/udev/control");
	for (int i = 0; i < 100 && !exists(ctl); i++)
		usleep(100000);
	if (!exists(ctl))
		warnx("udevd is not running (no /run/udev/control)");
	free(ctl);
	int rc = 0;
	if (hwdb_stale() && access(root[0] ? root : "/etc", W_OK) == 0) {
		msg("updating the hardware database");
		if (runl(udevadm, "hwdb", "--update", (char *)NULL) != 0)
			warnx("udevadm hwdb --update failed");
	}
	if (runl(udevadm, "trigger", "--type=subsystems", "--action=add", (char *)NULL) != 0)
		rc = 1;
	if (runl(udevadm, "trigger", "--type=devices", "--action=add", (char *)NULL) != 0)
		rc = 1;
	free(udevadm);
	return rc;
}

/* ---- getty ---- */

static bool is_vt(const char *tty)
{
	if (strncmp(tty, "tty", 3) || !tty[3])
		return false;
	for (const char *s = tty + 3; *s; s++)
		if (!isdigit((unsigned char)*s))
			return false;
	return true;
}

/* the speed of a serial console from console=ttyS0,115200n8 */
static char *console_speed(const char *tty)
{
	const char *s = cmdline();
	char *speed = NULL;
	size_t tl = strlen(tty);
	for (const char *p = s; (p = strstr(p, "console=")); p += 8) {
		if (p != s && p[-1] != ' ')
			continue;
		const char *v = p + 8;
		if (strncmp(v, tty, tl) || v[tl] != ',')
			continue;
		v += tl + 1;
		size_t n = strspn(v, "0123456789");
		if (n) {
			free(speed);
			speed = xstrndup(v, n);
		}
	}
	return speed;
}

/* getty TTY: agetty on a virtual terminal (TERM=linux) or on a serial line (local line
 * without modem control, the speed of console= or a list agetty tries in turn) */
int cmd_getty(int argc, char **argv)
{
	if (argc != 2)
		die("usage: getty TTY");
	const char *tty = argv[1];
	if (str_prefix(tty, "/dev/"))
		tty += 5;
	if (!*tty || strchr(tty, '/') || !strcmp(tty, "console"))
		die("getty: bad terminal name: %s", argv[1]);
	char *agetty = find_program("agetty");
	if (!agetty)
		die("agetty is not installed");
	if (is_vt(tty)) {
		/* the first terminal keeps the boot messages on screen */
		if (!strcmp(tty, "tty1"))
			execl(agetty, "agetty", "--noclear", tty, "linux", (char *)NULL);
		else
			execl(agetty, "agetty", tty, "linux", (char *)NULL);
	} else {
		char *speed = console_speed(tty);
		execl(agetty, "agetty", "-L", speed ? speed : "115200,57600,38400,9600", tty, "vt220", (char *)NULL);
	}
	die("exec %s: %s", agetty, strerror(errno));
}

/* consoles: a serial getty on every serial console of the kernel (console=ttyS0, an
 * earlycon from the device tree or SPCR); virtual terminals have their gettys already */
int cmd_consoles(int argc, char **argv)
{
	bool dry = argc > 1 && !strcmp(argv[1], "--print");
	char *p = rpath("/sys/class/tty/console/active");
	char *active = read_line(p);
	free(p);
	if (!active)
		return 0;
	char *dinitctl = find_program("dinitctl");
	const char *cfd = getenv("DINIT_CS_FD");
	int rc = 0;
	for (char *t = strtok(active, " "); t; t = strtok(NULL, " ")) {
		if (is_vt(t) || !strcmp(t, "console"))
			continue;
		char *dev = xasprintf("%s/dev/%s", root, t);
		bool ok = exists(dev);
		free(dev);
		if (!ok)
			continue;
		char *svc = xasprintf("serial-getty@%s", t);
		if (dry) {
			puts(svc);
		} else if (dinitctl) {
			int r = cfd ? runl(dinitctl, "--use-passed-cfd", "start", "--no-wait", svc, (char *)NULL)
				    : runl(dinitctl, "start", "--no-wait", svc, (char *)NULL);
			if (r != 0) {
				warnx("could not start %s", svc);
				rc = 1;
			}
		}
		free(svc);
	}
	free(dinitctl);
	free(active);
	return rc;
}
