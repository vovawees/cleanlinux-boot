/* SPDX-License-Identifier: MIT */
/* tmpfiles: the common subset of tmpfiles.d(5), so that packages written for systemd
 * get their directories in /run and /var without it.
 *
 *   d D   directory (D: emptied with --remove)    f f+  file (+: truncated), argument = content
 *   w w+  write (+: append) to existing files      L L+  symlink (+: replaces what is there)
 *   z     mode and owner of existing paths         r R   remove (R: recursively) with --remove
 *
 * Modifiers: ! only with --boot, - errors are ignored. Paths of w, z, r and R are globs.
 * Specifiers: %% %t %S %C %L %T %V %m %H. Other types (C, Z, a, x, ...) are skipped. */
#define _GNU_SOURCE
#include "boot.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool opt_create = true, opt_remove, opt_boot;
static int failures;

static void fail(bool ignore, const char *fmt, const char *path)
{
	if (ignore)
		return;
	warn_errno(fmt, path);
	failures++;
}

/* a user or group name or number from <root>/etc/passwd or group; -1 when unknown */
static long lookup_id(const char *file, const char *name)
{
	char *end;
	long n = strtol(name, &end, 10);
	if (*name && !*end)
		return n;
	char *p = rpath(file);
	char *s = read_file(p, NULL);
	free(p);
	if (!s)
		return !strcmp(name, "root") ? 0 : -1;
	long id = -1;
	size_t nl = strlen(name);
	for (char *save = NULL, *l = strtok_r(s, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		if (strncmp(l, name, nl) || l[nl] != ':')
			continue;
		char *f = strchr(l + nl + 1, ':'); /* past the password field */
		if (f)
			id = strtol(f + 1, NULL, 10);
		break;
	}
	free(s);
	return id;
}

static char *expand(const char *in)
{
	static const struct {
		char c;
		const char *v;
	} dirs[] = {{'t', "/run"}, {'S', "/var/lib"}, {'C', "/var/cache"}, {'L', "/var/log"}, {'T', "/tmp"}, {'V', "/var/tmp"}};
	size_t cap = strlen(in) * 2 + 64, n = 0;
	char *out = xmalloc(cap);
	for (const char *s = in; *s; s++) {
		const char *add = NULL;
		char *owned = NULL, one[2] = {*s, 0};
		if (*s == '%' && s[1]) {
			s++;
			if (*s == '%')
				add = "%";
			for (size_t i = 0; !add && i < ARRAY_LEN(dirs); i++)
				if (dirs[i].c == *s)
					add = dirs[i].v;
			if (!add && *s == 'm') {
				char *p = rpath("/etc/machine-id");
				add = owned = read_line(p);
				free(p);
			}
			if (!add && *s == 'H') {
				char h[256] = "";
				gethostname(h, sizeof(h) - 1);
				add = owned = xstrdup(h);
			}
			if (!add) {
				one[0] = '%'; /* unknown: kept as it is */
				s--;
				add = one;
			}
		} else {
			add = one;
		}
		size_t al = strlen(add);
		if (n + al + 1 > cap)
			out = xrealloc(out, cap = (n + al + 1) * 2);
		memcpy(out + n, add, al);
		n += al;
		free(owned);
	}
	out[n] = '\0';
	return out;
}

struct entry {
	char type;
	bool plus, boot_only, ignore;
	char *path;  /* expanded, without the root */
	mode_t mode; /* (mode_t)-1: not given */
	long uid, gid;
	char *arg;   /* expanded, or NULL */
};

static void set_attrs(const struct entry *e, const char *p, bool created, mode_t def_mode)
{
	mode_t mode = e->mode != (mode_t)-1 ? e->mode : (created ? def_mode : (mode_t)-1);
	long uid = e->uid >= 0 ? e->uid : (created ? 0 : -1);
	long gid = e->gid >= 0 ? e->gid : (created ? 0 : -1);
	if ((uid >= 0 || gid >= 0) && lchown(p, (uid_t)uid, (gid_t)gid) < 0)
		fail(e->ignore, "chown %s", p);
	if (mode != (mode_t)-1 && chmod(p, mode) < 0)
		fail(e->ignore, "chmod %s", p);
}

static int rm_entry(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
	(void)st;
	if (ftw->level == 0)
		return 0;
	if ((type == FTW_DP ? rmdir(path) : unlink(path)) < 0 && errno != ENOENT)
		warn_errno("remove %s", path);
	return 0;
}

/* the contents of a directory, without crossing into other filesystems */
static void empty_dir(const char *p) { nftw(p, rm_entry, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT); }

static void do_dir(const struct entry *e, const char *p)
{
	bool created = false;
	if (opt_create) {
		struct stat st;
		if (lstat(p, &st) < 0) {
			char *parent = xstrdup(p);
			char *slash = strrchr(parent, '/');
			if (slash && slash != parent) {
				*slash = '\0';
				mkdir_p(parent, 0755);
			}
			free(parent);
			if (mkdir(p, 0755) < 0) {
				fail(e->ignore, "mkdir %s", p);
				return;
			}
			created = true;
		} else if (!S_ISDIR(st.st_mode)) {
			errno = ENOTDIR;
			fail(e->ignore, "%s", p);
			return;
		}
		set_attrs(e, p, created, 0755);
	}
	if (opt_remove && e->type == 'D' && !created && is_dir(p))
		empty_dir(p);
}

static void do_file(const struct entry *e, const char *p)
{
	if (!opt_create)
		return;
	bool exists_now = access(p, F_OK) == 0;
	if (exists_now && !e->plus) {
		set_attrs(e, p, false, 0644);
		return;
	}
	int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
	if (fd < 0) {
		fail(e->ignore, "%s", p);
		return;
	}
	if (e->arg && write(fd, e->arg, strlen(e->arg)) < 0)
		fail(e->ignore, "write %s", p);
	close(fd);
	set_attrs(e, p, true, 0644);
}

static void do_link(const struct entry *e, const char *p)
{
	if (!opt_create || !e->arg)
		return;
	char target[4096];
	ssize_t l = readlink(p, target, sizeof(target) - 1);
	if (l >= 0) {
		target[l] = '\0';
		if (!strcmp(target, e->arg))
			return;
	}
	if (l >= 0 || access(p, F_OK) == 0) {
		if (!e->plus)
			return;
		if (unlink(p) < 0 && rmdir(p) < 0) {
			fail(e->ignore, "replace %s", p);
			return;
		}
	}
	if (symlink(e->arg, p) < 0)
		fail(e->ignore, "symlink %s", p);
}

static void do_glob(const struct entry *e, const char *pattern)
{
	glob_t g;
	if (glob(pattern, GLOB_NOSORT, NULL, &g) != 0)
		return; /* nothing matches: nothing to do */
	for (size_t i = 0; i < g.gl_pathc; i++) {
		const char *p = g.gl_pathv[i];
		switch (e->type) {
		case 'w':
			if (opt_create && e->arg) {
				int fd = open(p, O_WRONLY | O_CLOEXEC | O_NOFOLLOW | (e->plus ? O_APPEND : 0));
				if (fd < 0 || write(fd, e->arg, strlen(e->arg)) < 0)
					fail(e->ignore, "write %s", p);
				if (fd >= 0)
					close(fd);
			}
			break;
		case 'z':
			if (opt_create)
				set_attrs(e, p, false, 0);
			break;
		case 'r':
			if (opt_remove && remove(p) < 0 && errno != ENOENT && errno != ENOTEMPTY)
				fail(e->ignore, "remove %s", p);
			break;
		case 'R':
			if (opt_remove) {
				empty_dir(p);
				if (remove(p) < 0 && errno != ENOENT)
					fail(e->ignore, "remove %s", p);
			}
			break;
		}
	}
	globfree(&g);
}

static void apply(const struct entry *e)
{
	if (e->boot_only && !opt_boot)
		return;
	char *p = rpath(e->path);
	switch (e->type) {
	case 'd':
	case 'D':
		do_dir(e, p);
		break;
	case 'f':
		do_file(e, p);
		break;
	case 'L':
		do_link(e, p);
		break;
	case 'w':
	case 'z':
	case 'r':
	case 'R':
		do_glob(e, p);
		break;
	}
	free(p);
}

/* the next whitespace-separated field; NULL at the end of the line */
static char *field(char **s)
{
	char *p = *s;
	while (*p == ' ' || *p == '\t')
		p++;
	if (!*p)
		return NULL;
	char *start = p;
	while (*p && *p != ' ' && *p != '\t')
		p++;
	if (*p)
		*p++ = '\0';
	*s = p;
	return start;
}

static void parse_line(const char *file, int lineno, char *line)
{
	char *s = line;
	char *type = field(&s), *path = field(&s);
	if (!type || *type == '#')
		return;
	if (!path) {
		warnx("%s:%d: no path", file, lineno);
		return;
	}
	char *mode = field(&s), *user = field(&s), *group = field(&s), *age = field(&s);
	(void)age;
	while (*s == ' ' || *s == '\t')
		s++;
	struct entry e = {.type = type[0], .mode = (mode_t)-1, .uid = -1, .gid = -1};
	if (!strchr("dDfwLzrR", e.type))
		return; /* not supported here (C, Z, a, x, c, b, p, ...) */
	for (const char *m = type + 1; *m; m++) {
		if (*m == '+')
			e.plus = true;
		else if (*m == '!')
			e.boot_only = true;
		else if (*m == '-')
			e.ignore = true;
	}
	if (mode && strcmp(mode, "-")) {
		const char *m = mode;
		if (*m == '~' || *m == ':')
			m++;
		e.mode = (mode_t)strtoul(m, NULL, 8);
	}
	if (user && strcmp(user, "-") && (e.uid = lookup_id("/etc/passwd", user)) < 0)
		warnx("%s:%d: unknown user %s", file, lineno, user);
	if (group && strcmp(group, "-") && (e.gid = lookup_id("/etc/group", group)) < 0)
		warnx("%s:%d: unknown group %s", file, lineno, group);
	e.path = expand(path);
	if (*s && strcmp(s, "-"))
		e.arg = expand(s);
	if (e.path[0] != '/')
		warnx("%s:%d: path is not absolute: %s", file, lineno, e.path);
	else
		apply(&e);
	free(e.path);
	free(e.arg);
}

/* tmpfiles [--boot] [--remove] [--no-create] [FILE...] */
int cmd_tmpfiles(int argc, char **argv)
{
	strv files = STRV_INIT;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--boot"))
			opt_boot = true;
		else if (!strcmp(argv[i], "--remove"))
			opt_remove = true;
		else if (!strcmp(argv[i], "--no-create"))
			opt_create = false;
		else if (argv[i][0] == '-')
			die("usage: tmpfiles [--boot] [--remove] [--no-create] [FILE...]");
		else
			strv_push(&files, argv[i]);
	}
	if (!files.n)
		conf_files("tmpfiles.d", ".conf", &files);
	for (size_t i = 0; i < files.n; i++) {
		char *text = read_file(files.v[i], NULL);
		if (!text) {
			warn_errno("%s", files.v[i]);
			failures++;
			continue;
		}
		int lineno = 0;
		for (char *l = text, *next; l && *l; l = next) {
			next = strchr(l, '\n');
			if (next)
				*next++ = '\0';
			parse_line(files.v[i], ++lineno, l);
		}
		free(text);
	}
	strv_free(&files);
	return failures ? 1 : 0;
}
