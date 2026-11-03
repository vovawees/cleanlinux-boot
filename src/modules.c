/* SPDX-License-Identifier: MIT */
/* Kernel modules through libkmod: the lists of modules-load.d and modules-load= on the
 * command line, and coldplug (the module of every device's modalias) for the initramfs and
 * for systems without udev. modprobe.d (blacklist, options, softdeps) applies as usual. */
#define _GNU_SOURCE
#include "boot.h"

#include <errno.h>
#include <ftw.h>
#include <libkmod.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

struct kmod_ctx *kmod_open(const char *kver)
{
	char *dir = NULL;
	if (kver || *root) {
		struct utsname u;
		if (!kver && uname(&u) == 0)
			kver = u.release;
		dir = xasprintf("%s/usr/lib/modules/%s", root, kver ? kver : "");
	}
	struct kmod_ctx *ctx = kmod_new(dir, NULL);
	free(dir);
	if (!ctx)
		return NULL;
	kmod_load_resources(ctx);
	return ctx;
}

/* load NAME (a module name or an alias); 1 when something was inserted, 0 when nothing had to
 * be (built in, loaded, blacklisted, unknown alias), -1 on errors */
static int load_one(struct kmod_ctx *ctx, const char *name, bool quiet)
{
	struct kmod_list *list = NULL, *l;
	int r = kmod_module_new_from_lookup(ctx, name, &list);
	if (r < 0) {
		if (!quiet)
			warnx("module %s: lookup failed: %s", name, strerror(-r));
		return -1;
	}
	if (!list) {
		if (!quiet)
			warnx("module %s not found", name);
		return quiet ? 0 : -1;
	}
	int result = 0;
	kmod_list_foreach(l, list)
	{
		struct kmod_module *mod = kmod_module_get_module(l);
		int state = kmod_module_get_initstate(mod);
		if (state == KMOD_MODULE_LIVE || state == KMOD_MODULE_BUILTIN || state == KMOD_MODULE_COMING) {
			kmod_module_unref(mod);
			continue;
		}
		r = kmod_module_probe_insert_module(mod, KMOD_PROBE_APPLY_BLACKLIST, NULL, NULL, NULL, NULL);
		if (r == 0) {
			if (result == 0)
				result = 1;
		} else if (r < 0 && r != -EEXIST && r != -ENODEV) {
			/* ENODEV: the driver found no device of its kind, normal for aliases */
			if (!quiet)
				warnx("module %s: %s", kmod_module_get_name(mod), strerror(-r));
			result = -1;
		}
		kmod_module_unref(mod);
	}
	kmod_module_unref_list(list);
	return result;
}

int modules_load_list(struct kmod_ctx *ctx, const strv *names, bool quiet)
{
	int failed = 0;
	for (size_t i = 0; i < names->n; i++)
		if (load_one(ctx, names->v[i], quiet) < 0)
			failed++;
	return failed;
}

static void split_into(strv *out, const char *list, const char *seps)
{
	char *copy = xstrdup(list), *save;
	for (char *t = strtok_r(copy, seps, &save); t; t = strtok_r(NULL, seps, &save))
		if (!strv_contains(out, t))
			strv_push(out, t);
	free(copy);
}

int cmd_modules(int argc, char **argv)
{
	strv names = STRV_INIT, files = STRV_INIT;
	for (int i = 1; i < argc; i++)
		strv_push(&names, argv[i]);
	if (argc <= 1) {
		static const char *const keys[] = {"modules-load", "rd.modules-load", "modules_load"};
		for (size_t k = 0; k < ARRAY_LEN(keys); k++) {
			char *v = cmdline_value(keys[k]);
			if (v)
				split_into(&names, v, ",");
			free(v);
		}
		conf_files("modules-load.d", ".conf", &files);
		for (size_t f = 0; f < files.n; f++) {
			char *data = read_file(files.v[f], NULL);
			if (!data) {
				warn_errno("%s", files.v[f]);
				continue;
			}
			for (char *save, *l = strtok_r(data, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
				l = trim(l);
				if (*l && *l != '#' && *l != ';' && !strv_contains(&names, l))
					strv_push(&names, l);
			}
			free(data);
		}
	}
	int rc = 0;
	if (names.n) {
		struct kmod_ctx *ctx = kmod_open(NULL);
		if (!ctx)
			die("libkmod: cannot open the module index");
		rc = modules_load_list(ctx, &names, false) ? 1 : 0;
		kmod_unref(ctx);
	}
	strv_free(&names);
	strv_free(&files);
	return rc;
}

/* ---- coldplug ---- */

static strv seen_aliases;
static struct kmod_ctx *walk_ctx;
static int walk_loaded;

static int on_entry(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
	(void)st;
	if (type != FTW_F || strcmp(path + ftw->base, "modalias"))
		return 0;
	char *alias = read_line(path);
	if (!alias || !*alias || strv_contains(&seen_aliases, alias)) {
		free(alias);
		return 0;
	}
	strv_take(&seen_aliases, alias);
	if (load_one(walk_ctx, alias, true) > 0)
		walk_loaded++;
	return 0;
}

/* every device's driver; drivers create devices (buses, hubs), so repeat until nothing new */
int modules_coldplug(struct kmod_ctx *ctx)
{
	int total = 0;
	walk_ctx = ctx;
	char *devices = rpath("/sys/devices");
	for (int pass = 0; pass < 5; pass++) {
		walk_loaded = 0;
		nftw(devices, on_entry, 32, FTW_PHYS);
		total += walk_loaded;
		if (!walk_loaded)
			break;
	}
	free(devices);
	return total;
}

int cmd_coldplug(int argc, char **argv)
{
	(void)argc, (void)argv;
	struct kmod_ctx *ctx = kmod_open(NULL);
	if (!ctx)
		die("libkmod: cannot open the module index");
	int n = modules_coldplug(ctx);
	kmod_unref(ctx);
	msg("coldplug: %d module(s) loaded", n);
	return 0;
}
