/* SPDX-License-Identifier: MIT */
/* The hardware's power and memory settings, from text files in /etc rather than a GUI:
 *
 *   zram       swap in compressed RAM sized by the RAM (zram.d); none next to a disk swap,
 *              where zswap compresses in front of it
 *   backlight  the brightness of every backlight, saved at shutdown and restored at boot
 *   power      power.d: platform_profile, the governor and energy preference of the CPU,
 *              battery charge limits, power limits through firmware attributes and RAPL
 *
 * zram.d and power.d are KEY=VALUE files: *.conf of /etc/cleanlinux/DIR, /run, /usr/local/lib
 * and /usr/lib (the hardware profile's package), sorted by name, the last value of a key wins. */
#define _GNU_SOURCE
#include "boot.h"

#include <dirent.h>
#include <errno.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/swap.h>
#include <unistd.h>

typedef struct {
	strv keys, vals;
} settings;

static void settings_set(settings *s, const char *key, const char *val)
{
	for (size_t i = 0; i < s->keys.n; i++) {
		if (!strcmp(s->keys.v[i], key)) {
			free(s->vals.v[i]);
			s->vals.v[i] = xstrdup(val);
			return;
		}
	}
	strv_push(&s->keys, key);
	strv_push(&s->vals, val);
}

static const char *settings_get(const settings *s, const char *key)
{
	for (size_t i = 0; i < s->keys.n; i++)
		if (!strcmp(s->keys.v[i], key))
			return s->vals.v[i];
	return NULL;
}

static void settings_load(settings *s, const char *dir)
{
	strv files = STRV_INIT;
	conf_files(dir, ".conf", &files);
	for (size_t f = 0; f < files.n; f++) {
		char *data = read_file(files.v[f], NULL);
		if (!data) {
			warn_errno("%s", files.v[f]);
			continue;
		}
		for (char *save, *l = strtok_r(data, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
			l = trim(l);
			if (!*l || *l == '#' || *l == ';')
				continue;
			char *eq = strchr(l, '=');
			if (!eq) {
				warnx("%s: not KEY=VALUE: %s", files.v[f], l);
				continue;
			}
			*eq = '\0';
			settings_set(s, trim(l), trim(eq + 1));
		}
		free(data);
	}
	strv_free(&files);
}

static void settings_free(settings *s)
{
	strv_free(&s->keys);
	strv_free(&s->vals);
}

/* ---- zram ---- */

#define ZRAM_MAX_MIB 8192ULL

static unsigned long long mem_total(void)
{
	char *p = rpath("/proc/meminfo");
	char *s = read_file(p, NULL);
	free(p);
	unsigned long long kib = 0;
	if (s) {
		char *m = strstr(s, "MemTotal:");
		if (m)
			kib = strtoull(m + 9, NULL, 10);
		free(s);
	}
	return kib * 1024;
}

/* a swap other than zram is on: the swaps of fstab are enabled before this runs */
static bool disk_swap(void)
{
	char *p = rpath("/proc/swaps");
	char *s = read_file(p, NULL);
	free(p);
	bool found = false;
	if (s) {
		char *save = NULL, *l = strtok_r(s, "\n", &save); /* the header */
		while (l && (l = strtok_r(NULL, "\n", &save)))
			if (*l && !str_prefix(l, "/dev/zram"))
				found = true;
		free(s);
	}
	return found;
}

/* bytes of zram swap: size= is a percentage of the RAM or MiB (0 or off: none), at most max=
 * MiB (0: no limit); cleanlinux.zram= on the kernel command line wins */
static unsigned long long zram_bytes(const settings *s, unsigned long long ram)
{
	char *cmd = cmdline_value("cleanlinux.zram");
	const char *size = cmd && *cmd ? cmd : settings_get(s, "size");
	if (!size || !*size)
		size = "100%";
	unsigned long long max = ZRAM_MAX_MIB << 20, bytes = 0;
	const char *m = settings_get(s, "max");
	if (m && *m) {
		char *end;
		unsigned long long v = strtoull(m, &end, 10);
		if (*end)
			warnx("bad max '%s'", m);
		else
			max = v << 20;
	}
	if (strcmp(size, "off") && strcmp(size, "no")) {
		char *end;
		unsigned long long n = strtoull(size, &end, 10);
		if (!strcmp(end, "%"))
			bytes = ram / 100 * n;
		else if (!*end)
			bytes = n << 20;
		else
			warnx("bad size '%s'", size);
		if (max && bytes > max)
			bytes = max;
	}
	free(cmd);
	return bytes & ~4095ULL;
}

static int zram_start(bool print)
{
	settings s = {STRV_INIT, STRV_INIT};
	settings_load(&s, "cleanlinux/zram.d");
	unsigned long long ram = mem_total(), bytes = zram_bytes(&s, ram);
	const char *alg = settings_get(&s, "algorithm");
	const char *pr = settings_get(&s, "priority");
	alg = alg && *alg ? alg : "zstd";
	int prio = pr && *pr ? atoi(pr) : 100;
	bool disk = disk_swap();
	int rc = 0;
	if (print) {
		printf("ram %llu MiB\nzram %llu MiB\nalgorithm %s\npriority %d\n", ram >> 20, bytes >> 20, alg, prio);
		printf("disk-swap %s\n", disk ? "yes (no zram: zswap in front of it)" : "no");
		goto out;
	}
	if (!bytes || disk) {
		msg("%s", disk ? "a disk swap is on, zswap compresses in front of it" : "off");
		goto out;
	}
	char *sys = rpath("/sys/block/zram0");
	if (!exists(sys)) {
		struct kmod_ctx *ctx = kmod_open(NULL);
		strv names = STRV_INIT;
		strv_push(&names, "zram");
		if (ctx)
			modules_load_list(ctx, &names, false);
		strv_free(&names);
	}
	char *size_file = xasprintf("%s/disksize", sys), *alg_file = xasprintf("%s/comp_algorithm", sys);
	char *dev = rpath("/dev/zram0"), *cur = NULL;
	if (!exists(sys)) {
		warnx("no zram device (a kernel without CONFIG_ZRAM?)");
		rc = 1;
	} else if ((cur = read_line(size_file)) && strcmp(cur, "0")) {
		msg("zram0 is already set up");
	} else {
		if (write_str(alg_file, alg) < 0)
			warn_errno("compression %s", alg);
		char num[32];
		snprintf(num, sizeof num, "%llu", bytes);
		char *mkswap = find_program("mkswap");
		if (write_str(size_file, num) < 0) {
			warn_errno("size");
			rc = 1;
		} else if (!mkswap || runl(mkswap, "-L", "zram0", dev, (char *)NULL) != 0) {
			warnx("mkswap failed");
			rc = 1;
		} else if (swapon(dev, SWAP_FLAG_PREFER | ((prio << SWAP_FLAG_PRIO_SHIFT) & SWAP_FLAG_PRIO_MASK)) < 0) {
			warn_errno("swapon");
			rc = 1;
		} else {
			/* zswap in front of zram compresses everything twice */
			char *zs = rpath("/sys/module/zswap/parameters/enabled");
			if (exists(zs))
				write_str(zs, "N");
			free(zs);
			msg("%llu MiB of swap (%s, priority %d)", bytes >> 20, alg, prio);
		}
		free(mkswap);
	}
	free(cur);
	free(dev);
	free(alg_file);
	free(size_file);
	free(sys);
out:
	settings_free(&s);
	return rc;
}

static int zram_stop(void)
{
	char *dev = rpath("/dev/zram0"), *reset = rpath("/sys/block/zram0/reset");
	int rc = 0;
	if (exists(reset)) {
		if (swapoff(dev) < 0 && errno != EINVAL)
			warn_errno("swapoff");
		if (write_str(reset, "1") < 0) {
			warn_errno("reset");
			rc = 1;
		}
	}
	free(reset);
	free(dev);
	return rc;
}

int cmd_zram(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "start"))
		return zram_start(false);
	if (argc == 2 && !strcmp(argv[1], "--print"))
		return zram_start(true);
	if (argc == 2 && !strcmp(argv[1], "stop"))
		return zram_stop();
	die("usage: zram start|stop|--print");
}

/* ---- backlight ---- */

#define BACKLIGHT_DIR STATE_DIR "/backlight"

static int backlight(bool save)
{
	char *cls = rpath("/sys/class/backlight"), *state = rpath(BACKLIGHT_DIR);
	DIR *d = opendir(cls);
	int rc = 0;
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.')
				continue;
			char *cur = xasprintf("%s/%s/brightness", cls, e->d_name);
			char *max = xasprintf("%s/%s/max_brightness", cls, e->d_name);
			char *file = xasprintf("%s/%s", state, e->d_name);
			char *v = read_line(save ? cur : file), *m = save ? NULL : read_line(max);
			if (save && v && *v) {
				char *line = xasprintf("%s\n", v);
				if (mkdir_p(state, 0755) < 0 || write_file(file, line, strlen(line), 0644) < 0) {
					warn_errno("%s", file);
					rc = 1;
				}
				free(line);
			} else if (!save && v && m) {
				/* never darker than 5%: a black screen at boot looks like a dead device */
				long top = strtol(m, NULL, 10), val = strtol(v, NULL, 10), low = top / 20;
				if (low < 1 && top > 0)
					low = 1;
				val = val < low ? low : val > top ? top : val;
				char num[32];
				snprintf(num, sizeof num, "%ld", val);
				if (write_str(cur, num) < 0) {
					warn_errno("%s", cur);
					rc = 1;
				}
			}
			free(m);
			free(v);
			free(file);
			free(max);
			free(cur);
		}
		closedir(d);
	}
	free(state);
	free(cls);
	return rc;
}

int cmd_backlight(int argc, char **argv)
{
	if (argc == 2 && (!strcmp(argv[1], "load") || !strcmp(argv[1], "save")))
		return backlight(!strcmp(argv[1], "save"));
	die("usage: backlight load|save");
}

/* ---- power ---- */

/* write VALUE to every file matching PATTERN inside the root; how many took it */
static int write_glob(const char *pattern, const char *value, const char *key, bool *failed)
{
	char *p = rpath(pattern);
	glob_t g;
	int n = 0;
	if (glob(p, 0, NULL, &g) == 0) {
		for (size_t i = 0; i < g.gl_pathc; i++) {
			if (write_str(g.gl_pathv[i], value) < 0) {
				warn_errno("%s=%s: %s", key, value, g.gl_pathv[i]);
				*failed = true;
			} else {
				n++;
			}
		}
		globfree(&g);
	}
	free(p);
	return n;
}

static const struct {
	const char *key, *pattern;
} POWER_FILES[] = {
	{"platform_profile", "/sys/firmware/acpi/platform_profile"},
	/* the governor first: the energy preference of the active pstate drivers depends on it */
	{"governor", "/sys/devices/system/cpu/cpufreq/policy*/scaling_governor"},
	{"epp", "/sys/devices/system/cpu/cpufreq/policy*/energy_performance_preference"},
	/* the end before the start: drivers refuse a start above the end */
	{"charge_limit", "/sys/class/power_supply/*/charge_control_end_threshold"},
	{"charge_start", "/sys/class/power_supply/*/charge_control_start_threshold"},
};

static int power(bool print)
{
	settings s = {STRV_INIT, STRV_INIT};
	settings_load(&s, "cleanlinux/power.d");
	bool failed = false;
	for (size_t i = 0; i < ARRAY_LEN(POWER_FILES); i++) {
		const char *v = settings_get(&s, POWER_FILES[i].key);
		if (!v || !*v)
			continue;
		if (print) {
			printf("%s=%s\n", POWER_FILES[i].key, v);
			continue;
		}
		if (!write_glob(POWER_FILES[i].pattern, v, POWER_FILES[i].key, &failed) && !failed)
			msg("%s: not supported by this hardware", POWER_FILES[i].key);
	}
	for (size_t i = 0; i < s.keys.n; i++) {
		const char *k = s.keys.v[i], *v = s.vals.v[i];
		bool known = false;
		for (size_t j = 0; j < ARRAY_LEN(POWER_FILES); j++)
			known |= !strcmp(k, POWER_FILES[j].key);
		if (known || !*v)
			continue;
		char *pattern = NULL, *value = NULL;
		if (str_prefix(k, "firmware.") && k[9] && !strchr(k + 9, '/')) {
			/* firmware attributes: TDP (ppt_pl1_spl, ...) and the like of asus-armoury,
			 * lenovo-wmi and other platform drivers */
			pattern = xasprintf("/sys/class/firmware-attributes/*/attributes/%s/current_value", k + 9);
			value = xstrdup(v);
		} else if (!strcmp(k, "rapl_long") || !strcmp(k, "rapl_short")) {
			/* Intel RAPL package limits in watts: constraint 0 is long term (PL1), 1 short (PL2) */
			char *end;
			double w = strtod(v, &end);
			if (*end || w <= 0) {
				warnx("%s: watts expected, not '%s'", k, v);
				failed = true;
				continue;
			}
			pattern = xasprintf("/sys/class/powercap/intel-rapl:[0-9]/constraint_%d_power_limit_uw",
					    !strcmp(k, "rapl_short"));
			value = xasprintf("%.0f", w * 1e6);
		} else {
			warnx("unknown setting %s", k);
			failed = true;
			continue;
		}
		if (print)
			printf("%s=%s\n", k, v);
		else if (!write_glob(pattern, value, k, &failed) && !failed)
			msg("%s: not supported by this hardware", k);
		free(value);
		free(pattern);
	}
	settings_free(&s);
	return failed;
}

int cmd_power(int argc, char **argv)
{
	if (argc == 1 || (argc == 2 && !strcmp(argv[1], "apply")))
		return power(false);
	if (argc == 2 && !strcmp(argv[1], "--print"))
		return power(true);
	die("usage: power [apply|--print]");
}
