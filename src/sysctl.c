/* SPDX-License-Identifier: MIT */
/* sysctl.d: like systemd-sysctl and procps' sysctl --system. Files from /etc, /run,
 * /usr/local/lib and /usr/lib sysctl.d (a name in /etc masks the same name in /usr/lib), then
 * /etc/sysctl.conf; for a key set more than once the last value wins. "-key = value" ignores
 * errors, keys may use '.' or '/' and glob patterns (net.ipv4.conf.*.rp_filter). */
#define _GNU_SOURCE
#include "boot.h"

#include <errno.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct setting {
	char *key, *value;
	bool ignore;
};

static struct setting *settings;
static size_t nsettings;

static void add(const char *key, const char *value, bool ignore)
{
	for (size_t i = 0; i < nsettings; i++)
		if (!strcmp(settings[i].key, key)) {
			free(settings[i].value);
			settings[i].value = xstrdup(value);
			settings[i].ignore = ignore;
			return;
		}
	settings = xrealloc(settings, (nsettings + 1) * sizeof(*settings));
	settings[nsettings++] = (struct setting){xstrdup(key), xstrdup(value), ignore};
}

/* "net.ipv4.ip_forward" -> "net/ipv4/ip_forward"; with '/' already, dots stay (interface names) */
static char *key_path(const char *key)
{
	char *k = xstrdup(key);
	if (!strchr(k, '/'))
		for (char *c = k; *c; c++)
			if (*c == '.')
				*c = '/';
	char *p = xasprintf("%s/proc/sys/%s", root, k);
	free(k);
	return p;
}

static void parse(const char *path)
{
	char *data = read_file(path, NULL);
	if (!data) {
		warn_errno("%s", path);
		return;
	}
	int lineno = 0;
	for (char *save, *line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		lineno++;
		char *l = trim(line);
		if (!*l || *l == '#' || *l == ';')
			continue;
		bool ignore = false;
		if (*l == '-') {
			ignore = true;
			l++;
		}
		char *eq = strchr(l, '=');
		if (!eq) {
			warnx("%s:%d: no '=' in \"%s\"", path, lineno, l);
			continue;
		}
		*eq = '\0';
		char *key = trim(l), *value = trim(eq + 1);
		if (!*key) {
			warnx("%s:%d: empty key", path, lineno);
			continue;
		}
		add(key, value, ignore);
	}
	free(data);
}

static int apply(const struct setting *s)
{
	char *path = key_path(s->key);
	int failed = 0;
	if (strpbrk(path, "*?[")) {
		glob_t g;
		if (glob(path, 0, NULL, &g) == 0) {
			for (size_t i = 0; i < g.gl_pathc; i++)
				if (write_str(g.gl_pathv[i], s->value) < 0 && !s->ignore) {
					warn_errno("%s", g.gl_pathv[i]);
					failed++;
				}
			globfree(&g);
		}
	} else if (write_str(path, s->value) < 0) {
		if (!s->ignore && errno != ENOENT) {
			warn_errno("sysctl %s = %s", s->key, s->value);
			failed++;
		} else if (!s->ignore) {
			warnx("sysctl %s: no such key", s->key);
		}
	}
	free(path);
	return failed;
}

int cmd_sysctl(int argc, char **argv)
{
	strv files = STRV_INIT;
	if (argc > 1) {
		for (int i = 1; i < argc; i++)
			strv_push(&files, argv[i]);
	} else {
		conf_files("sysctl.d", ".conf", &files);
		char *legacy = rpath("/etc/sysctl.conf");
		if (is_file(legacy))
			strv_push(&files, legacy);
		free(legacy);
	}
	for (size_t i = 0; i < files.n; i++)
		parse(files.v[i]);
	int failed = 0;
	for (size_t i = 0; i < nsettings; i++)
		failed += apply(&settings[i]);
	strv_free(&files);
	return failed ? 1 : 0;
}
