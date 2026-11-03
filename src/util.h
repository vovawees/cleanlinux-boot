/* SPDX-License-Identifier: MIT */
/* cleanlinux-boot: shared helpers */
#ifndef CLB_UTIL_H
#define CLB_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

extern const char *prog;  /* command name for messages */
extern const char *root;  /* "" or an alternative root (tests, installers) */
extern bool to_kmsg;      /* messages also go to /dev/kmsg (early boot, initramfs) */

void msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void warnx(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void warn_errno(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
_Noreturn void die(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* a growable vector of owned strings */
typedef struct {
	char **v;
	size_t n, cap;
} strv;
#define STRV_INIT {NULL, 0, 0}
void strv_push(strv *s, const char *str);    /* copies */
void strv_take(strv *s, char *str);          /* takes ownership */
bool strv_contains(const strv *s, const char *str);
void strv_sort(strv *s);
void strv_free(strv *s);
char **strv_argv(strv *s); /* NULL-terminated view */

/* path inside the root: "/etc/x" -> "<root>/etc/x" (caller frees) */
char *rpath(const char *path);

char *read_file(const char *path, size_t *len); /* NUL-terminated; NULL + errno on error */
char *read_line(const char *path);              /* first line without the newline, trimmed */
int write_file(const char *path, const void *data, size_t len, mode_t mode); /* atomic replace */
int write_str(const char *path, const char *s); /* plain write (sysfs, procfs) */
int mkdir_p(const char *path, mode_t mode);
bool is_dir(const char *path);
bool is_file(const char *path);
bool exists(const char *path);
bool is_mountpoint(const char *path);
char *trim(char *s);
bool str_prefix(const char *s, const char *prefix);
bool str_suffix(const char *s, const char *suffix);

/* the kernel command line (from <root>/proc/cmdline or $CLEANLINUX_CMDLINE) */
const char *cmdline(void);
/* value of the last KEY=value on the command line; "" for a bare KEY; NULL when absent */
char *cmdline_value(const char *key);

/* configuration fragments: *SUFFIX files of /etc/DIR, /run/DIR, /usr/local/lib/DIR and
 * /usr/lib/DIR; a file name in an earlier directory masks the same name in later ones (a
 * symlink to /dev/null disables it); the result is sorted by file name */
void conf_files(const char *dir, const char *suffix, strv *out);

/* run a program and wait: its exit status, 128+signal, or -1 when it could not run */
int run(char *const argv[]);
int runl(const char *path, ...); /* NULL-terminated arguments after the program */
/* the program by name in /usr/bin (inside the root), or NULL */
char *find_program(const char *name);

#endif
