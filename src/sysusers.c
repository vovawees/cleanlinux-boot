/* SPDX-License-Identifier: MIT */
/* sysusers: the system users and groups that packages declare in sysusers.d (the format of
 * systemd-sysusers, the common subset), added to /etc/passwd, shadow, group and gshadow when
 * missing. burn runs it after a transaction that installed such a file (a file trigger) and
 * the boot runs it before tmpfiles, which may give files to those users.
 *
 *   u NAME ID "GECOS" HOME SHELL   a user and its group; ID: -, UID, UID:GID or UID:GROUP
 *   g NAME ID                      a group; ID: - or GID
 *   m USER GROUP                   USER becomes a member of GROUP
 *   r - LOW-HIGH                   (accepted, the range comes from /etc/login.defs)
 *
 * New IDs are the highest free ones of SYS_UID_MIN..SYS_UID_MAX and SYS_GID_MIN..SYS_GID_MAX
 * (login.defs, default 101..999), a user's group takes the user's number when it is free.
 * Accounts are locked ("!*"), the shell defaults to nologin and the home to /. */
#define _GNU_SOURCE
#include "boot.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* one account database file, kept in memory as its text and rewritten when changed */
struct db {
	const char *path; /* /etc/... */
	mode_t mode;
	char *text;
	size_t len, cap;
	bool changed;
};

static struct db passwd = {.path = "/etc/passwd", .mode = 0644}, shadow = {.path = "/etc/shadow", .mode = 0600},
		 group = {.path = "/etc/group", .mode = 0644}, gshadow = {.path = "/etc/gshadow", .mode = 0600};
static long uid_min = 101, uid_max = 999, gid_min = 101, gid_max = 999;
static int failures;

static void db_load(struct db *d)
{
	char *p = rpath(d->path);
	size_t len = 0;
	d->text = read_file(p, &len);
	if (!d->text) {
		d->text = xstrdup("");
		len = 0;
	}
	d->len = len;
	d->cap = len + 1;
	/* a last line without a newline gets one, so that appending makes a new line */
	if (len && d->text[len - 1] != '\n') {
		d->text = xrealloc(d->text, d->cap = len + 2);
		d->text[d->len++] = '\n';
		d->text[d->len] = '\0';
		d->changed = true;
	}
	free(p);
}

static void db_append(struct db *d, const char *line)
{
	size_t n = strlen(line);
	if (d->len + n + 2 > d->cap)
		d->text = xrealloc(d->text, d->cap = (d->len + n + 2) * 2);
	memcpy(d->text + d->len, line, n);
	d->len += n;
	d->text[d->len++] = '\n';
	d->text[d->len] = '\0';
	d->changed = true;
}

static int db_save(struct db *d)
{
	int rc = 0;
	if (d->changed) {
		char *p = rpath(d->path);
		if (write_file(p, d->text, d->len, d->mode) < 0) {
			warn_errno("writing %s", p);
			rc = -1;
		}
		free(p);
	}
	free(d->text);
	d->text = NULL;
	return rc;
}

/* the line of NAME (up to the first ':'), or NULL; *len gets its length without the newline */
static char *db_find(const struct db *d, const char *name, size_t *len)
{
	size_t nl = strlen(name);
	for (char *l = d->text; *l;) {
		char *end = strchr(l, '\n');
		size_t ll = end ? (size_t)(end - l) : strlen(l);
		if (ll > nl && !strncmp(l, name, nl) && l[nl] == ':') {
			if (len)
				*len = ll;
			return l;
		}
		if (!end)
			break;
		l = end + 1;
	}
	return NULL;
}

/* the number in the third field of NAME's line, or -1 */
static long db_id(const struct db *d, const char *name)
{
	char *l = db_find(d, name, NULL);
	if (!l)
		return -1;
	char *f = strchr(l, ':');
	f = f ? strchr(f + 1, ':') : NULL;
	return f ? strtol(f + 1, NULL, 10) : -1;
}

/* is the number ID in the third field of any line? */
static bool db_id_used(const struct db *d, long id)
{
	for (const char *l = d->text; *l;) {
		const char *f = strchr(l, ':');
		const char *end = strchr(l, '\n');
		if (f && (!end || f < end))
			f = strchr(f + 1, ':');
		if (f && (!end || f < end)) {
			char *e;
			long v = strtol(f + 1, &e, 10);
			if (e != f + 1 && *e == ':' && v == id)
				return true;
		}
		if (!end)
			break;
		l = end + 1;
	}
	return false;
}

static void read_login_defs(void)
{
	char *p = rpath("/etc/login.defs");
	char *s = read_file(p, NULL);
	free(p);
	if (!s)
		return;
	for (char *save = NULL, *l = strtok_r(s, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		char key[32];
		long v;
		if (sscanf(l, " %31s %ld", key, &v) != 2 || key[0] == '#')
			continue;
		if (!strcmp(key, "SYS_UID_MIN"))
			uid_min = v;
		else if (!strcmp(key, "SYS_UID_MAX"))
			uid_max = v;
		else if (!strcmp(key, "SYS_GID_MIN"))
			gid_min = v;
		else if (!strcmp(key, "SYS_GID_MAX"))
			gid_max = v;
	}
	free(s);
}

static bool valid_name(const char *s)
{
	if (!*s || strlen(s) > 31 || *s == '-' || (*s >= '0' && *s <= '9'))
		return false;
	for (const char *c = s; *c; c++)
		if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
		      *c == '_' || *c == '-' || (*c == '$' && !c[1])))
			return false;
	return true;
}

static long free_gid(long want)
{
	if (want >= 0 && !db_id_used(&group, want))
		return want;
	for (long id = gid_max; id >= gid_min; id--)
		if (!db_id_used(&group, id))
			return id;
	return -1;
}

static long free_uid(void)
{
	/* a number free as a user and as a group, so that the user's group can share it */
	for (long id = uid_max; id >= uid_min; id--)
		if (!db_id_used(&passwd, id) && !db_id_used(&group, id))
			return id;
	for (long id = uid_max; id >= uid_min; id--)
		if (!db_id_used(&passwd, id))
			return id;
	return -1;
}

static long add_group(const char *name, long want)
{
	long gid = db_id(&group, name);
	if (gid >= 0)
		return gid;
	gid = free_gid(want);
	if (gid < 0) {
		warnx("no free group ID for %s", name);
		failures++;
		return -1;
	}
	char *line = xasprintf("%s:x:%ld:", name, gid);
	db_append(&group, line);
	free(line);
	if (!db_find(&gshadow, name, NULL)) {
		line = xasprintf("%s:!::", name);
		db_append(&gshadow, line);
		free(line);
	}
	msg("created group %s (%ld)", name, gid);
	return gid;
}

static void add_user(const char *name, const char *id, const char *gecos, const char *home, const char *shell)
{
	if (db_find(&passwd, name, NULL))
		return;
	long uid = -1, gid = -1;
	const char *gname = name;
	char *colon = id ? strchr(id, ':') : NULL;
	char *num_end;
	if (id && strcmp(id, "-") && *id != '/') {
		uid = strtol(id, &num_end, 10);
		if (num_end == id || (*num_end && *num_end != ':')) {
			warnx("user %s: bad ID '%s'", name, id);
			failures++;
			return;
		}
		if (db_id_used(&passwd, uid)) {
			warnx("user %s: UID %ld is taken, choosing another", name, uid);
			uid = -1;
		}
	}
	if (colon) {
		char *e;
		long g = strtol(colon + 1, &e, 10);
		if (e != colon + 1 && !*e)
			gid = add_group(name, g);
		else
			gname = colon + 1;
	}
	if (uid < 0)
		uid = free_uid();
	if (uid < 0) {
		warnx("no free user ID for %s", name);
		failures++;
		return;
	}
	if (gid < 0) {
		gid = db_id(&group, gname);
		if (gid < 0 && strcmp(gname, name)) {
			warnx("user %s: unknown group %s", name, gname);
			failures++;
			return;
		}
		if (gid < 0)
			gid = add_group(name, uid);
		if (gid < 0)
			return;
	}
	char *line = xasprintf("%s:x:%ld:%ld:%s:%s:%s", name, uid, gid, gecos ? gecos : "", home && *home ? home : "/",
			       shell && *shell ? shell : "/usr/bin/nologin");
	db_append(&passwd, line);
	free(line);
	if (!db_find(&shadow, name, NULL)) {
		line = xasprintf("%s:!*:%ld::::::", name, (long)(time(NULL) / 86400));
		db_append(&shadow, line);
		free(line);
	}
	msg("created user %s (%ld)", name, uid);
}

/* add USER to the member list (4th field) of GROUP in DB */
static void add_member_to(struct db *d, const char *grp, const char *user)
{
	size_t len;
	char *l = db_find(d, grp, &len);
	if (!l)
		return;
	char *members = l;
	for (int i = 0; i < 3 && members; i++) {
		members = memchr(members, ':', len - (size_t)(members - l));
		if (members)
			members++;
	}
	if (!members)
		return;
	size_t ml = len - (size_t)(members - l);
	/* already a member? */
	size_t ul = strlen(user);
	for (const char *m = members; m < members + ml;) {
		const char *c = memchr(m, ',', (size_t)(members + ml - m));
		size_t n = c ? (size_t)(c - m) : (size_t)(members + ml - m);
		if (n == ul && !strncmp(m, user, ul))
			return;
		if (!c)
			break;
		m = c + 1;
	}
	size_t off = (size_t)(l - d->text) + len; /* the end of the line */
	char *ins = xasprintf("%s%s", ml ? "," : "", user);
	size_t il = strlen(ins);
	if (d->len + il + 1 > d->cap)
		d->text = xrealloc(d->text, d->cap = (d->len + il + 1) * 2);
	memmove(d->text + off + il, d->text + off, d->len - off + 1);
	memcpy(d->text + off, ins, il);
	d->len += il;
	d->changed = true;
	free(ins);
}

static void add_member(const char *user, const char *grp)
{
	if (!db_find(&group, grp, NULL)) {
		warnx("member %s: unknown group %s", user, grp);
		failures++;
		return;
	}
	size_t before = group.len;
	add_member_to(&group, grp, user);
	add_member_to(&gshadow, grp, user);
	if (group.len != before)
		msg("added %s to group %s", user, grp);
}

/* the next field: whitespace-separated, "double quoted" with spaces; "-" stays "-" */
static char *field(char **s)
{
	char *p = *s;
	while (*p == ' ' || *p == '\t')
		p++;
	if (!*p)
		return NULL;
	char *start = p;
	if (*p == '"') {
		start = ++p;
		while (*p && *p != '"')
			p++;
	} else {
		while (*p && *p != ' ' && *p != '\t')
			p++;
	}
	if (*p)
		*p++ = '\0';
	*s = p;
	return start;
}

static const char *opt(const char *f) { return f && strcmp(f, "-") ? f : NULL; }

static void parse_line(const char *file, int lineno, char *line)
{
	char *s = line;
	char *type = field(&s), *name = field(&s), *id = field(&s), *gecos = field(&s), *home = field(&s),
	     *shell = field(&s);
	if (!type || *type == '#')
		return;
	if (!name) {
		warnx("%s:%d: no name", file, lineno);
		failures++;
		return;
	}
	if (!strcmp(type, "r"))
		return;
	if (!valid_name(name)) {
		warnx("%s:%d: invalid name '%s'", file, lineno, name);
		failures++;
		return;
	}
	if (!strcmp(type, "u")) {
		add_user(name, opt(id), opt(gecos), opt(home), opt(shell));
	} else if (!strcmp(type, "g")) {
		long want = -1;
		if (opt(id)) {
			char *e;
			want = strtol(id, &e, 10);
			if (*e)
				want = -1;
		}
		add_group(name, want);
	} else if (!strcmp(type, "m")) {
		if (!opt(id) || !valid_name(id)) {
			warnx("%s:%d: m needs a group", file, lineno);
			failures++;
			return;
		}
		add_member(name, id);
	} else {
		warnx("%s:%d: unsupported type '%s'", file, lineno, type);
	}
}

int cmd_sysusers(int argc, char **argv)
{
	strv files = STRV_INIT;
	if (argc > 1) {
		for (int i = 1; i < argc; i++)
			strv_push(&files, argv[i]);
	} else {
		conf_files("sysusers.d", ".conf", &files);
	}
	if (!files.n) {
		strv_free(&files);
		return 0;
	}
	/* the lock of the shadow suite (lckpwdf): useradd and passwd wait for it too */
	char *lockp = rpath("/etc/.pwd.lock");
	int lock = open(lockp, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	free(lockp);
	if (lock >= 0 && flock(lock, LOCK_EX) < 0)
		warn_errno("locking the account files");
	read_login_defs();
	db_load(&passwd), db_load(&shadow), db_load(&group), db_load(&gshadow);
	for (size_t i = 0; i < files.n; i++) {
		char *s = read_file(files.v[i], NULL);
		if (!s) {
			warn_errno("reading %s", files.v[i]);
			failures++;
			continue;
		}
		int lineno = 0;
		for (char *l = s, *next; l; l = next) {
			next = strchr(l, '\n');
			if (next)
				*next++ = '\0';
			lineno++;
			parse_line(files.v[i], lineno, l);
		}
		free(s);
	}
	/* groups before users: a user line names its group */
	if (db_save(&group) < 0 || db_save(&gshadow) < 0 || db_save(&passwd) < 0 || db_save(&shadow) < 0)
		failures++;
	if (lock >= 0)
		close(lock);
	strv_free(&files);
	return failures ? 1 : 0;
}
