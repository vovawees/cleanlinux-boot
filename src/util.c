/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

const char *prog = "cleanlinux-boot";
const char *root = "";
bool to_kmsg;

static void vlog(const char *level, int err, const char *fmt, va_list ap)
{
	char buf[1024];
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	if (n < 0)
		return;
	if (err)
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ": %s", strerror(err));
	fprintf(stderr, "%s: %s%s\n", prog, level, buf);
	if (to_kmsg) {
		int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC | O_NOCTTY);
		if (fd >= 0) {
			/* <3> err, <4> warning, <6> info */
			dprintf(fd, "<%c>%s: %s%s\n", *level ? (level[0] == 'e' ? '3' : '4') : '6', prog, level, buf);
			close(fd);
		}
	}
}

void msg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vlog("", 0, fmt, ap);
	va_end(ap);
}

void warnx(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vlog("warning: ", 0, fmt, ap);
	va_end(ap);
}

void warn_errno(const char *fmt, ...)
{
	int e = errno;
	va_list ap;
	va_start(ap, fmt);
	vlog("warning: ", e, fmt, ap);
	va_end(ap);
}

_Noreturn void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vlog("error: ", 0, fmt, ap);
	va_end(ap);
	exit(1);
}

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p)
		die("out of memory");
	return p;
}

void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p)
		die("out of memory");
	return p;
}

char *xstrdup(const char *s)
{
	char *p = strdup(s);
	if (!p)
		die("out of memory");
	return p;
}

char *xstrndup(const char *s, size_t n)
{
	char *p = strndup(s, n);
	if (!p)
		die("out of memory");
	return p;
}

char *xasprintf(const char *fmt, ...)
{
	va_list ap;
	char *p;
	va_start(ap, fmt);
	int r = vasprintf(&p, fmt, ap);
	va_end(ap);
	if (r < 0)
		die("out of memory");
	return p;
}

void strv_take(strv *s, char *str)
{
	if (s->n + 2 > s->cap) {
		s->cap = s->cap ? s->cap * 2 : 8;
		s->v = xrealloc(s->v, s->cap * sizeof(char *));
	}
	s->v[s->n++] = str;
	s->v[s->n] = NULL;
}

void strv_push(strv *s, const char *str) { strv_take(s, xstrdup(str)); }

bool strv_contains(const strv *s, const char *str)
{
	for (size_t i = 0; i < s->n; i++)
		if (!strcmp(s->v[i], str))
			return true;
	return false;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

void strv_sort(strv *s)
{
	if (s->n > 1)
		qsort(s->v, s->n, sizeof(char *), cmp_str);
}

void strv_free(strv *s)
{
	for (size_t i = 0; i < s->n; i++)
		free(s->v[i]);
	free(s->v);
	s->v = NULL;
	s->n = s->cap = 0;
}

char **strv_argv(strv *s)
{
	if (!s->v)
		strv_take(s, NULL), s->n = 0;
	return s->v;
}

char *rpath(const char *path) { return xasprintf("%s%s", root, path); }

char *read_file(const char *path, size_t *len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	size_t cap = 4096, n = 0;
	char *buf = xmalloc(cap);
	for (;;) {
		if (n + 1 >= cap)
			buf = xrealloc(buf, cap *= 2);
		ssize_t r = read(fd, buf + n, cap - n - 1);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			int e = errno;
			free(buf);
			close(fd);
			errno = e;
			return NULL;
		}
		if (r == 0)
			break;
		n += (size_t)r;
	}
	close(fd);
	buf[n] = '\0';
	if (len)
		*len = n;
	return buf;
}

char *read_line(const char *path)
{
	char *s = read_file(path, NULL);
	if (!s)
		return NULL;
	s[strcspn(s, "\n")] = '\0';
	char *t = trim(s);
	if (t != s)
		memmove(s, t, strlen(t) + 1);
	return s;
}

int write_file(const char *path, const void *data, size_t len, mode_t mode)
{
	char *tmp = xasprintf("%s.clb-XXXXXX", path);
	int fd = mkostemp(tmp, O_CLOEXEC);
	if (fd < 0) {
		free(tmp);
		return -1;
	}
	const char *p = data;
	size_t left = len;
	while (left) {
		ssize_t w = write(fd, p, left);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			goto fail;
		}
		p += w;
		left -= (size_t)w;
	}
	if (fchmod(fd, mode) < 0 || fsync(fd) < 0)
		goto fail;
	if (close(fd) < 0) {
		fd = -1;
		goto fail;
	}
	if (rename(tmp, path) < 0) {
		fd = -1;
		goto fail;
	}
	free(tmp);
	return 0;
fail:;
	int e = errno;
	if (fd >= 0)
		close(fd);
	unlink(tmp);
	free(tmp);
	errno = e;
	return -1;
}

int write_str(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOCTTY);
	if (fd < 0)
		return -1;
	size_t len = strlen(s);
	ssize_t w = write(fd, s, len);
	int e = errno;
	close(fd);
	if (w < 0 || (size_t)w != len) {
		errno = w < 0 ? e : EIO;
		return -1;
	}
	return 0;
}

int mkdir_p(const char *path, mode_t mode)
{
	char *p = xstrdup(path);
	for (char *s = p + 1; *s; s++) {
		if (*s != '/')
			continue;
		*s = '\0';
		if (mkdir(p, 0755) < 0 && errno != EEXIST) {
			free(p);
			return -1;
		}
		*s = '/';
	}
	int r = mkdir(p, mode);
	free(p);
	if (r < 0 && errno == EEXIST && is_dir(path))
		return 0;
	return r;
}

bool is_dir(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_file(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool exists(const char *path) { return access(path, F_OK) == 0; }

/* a mount point when the directory and its parent are on different devices, or when it is
 * the root of the namespace (same inode as its parent) */
bool is_mountpoint(const char *path)
{
	struct stat a, b;
	if (lstat(path, &a) < 0 || !S_ISDIR(a.st_mode))
		return false;
	char *parent = xasprintf("%s/..", path);
	int r = stat(parent, &b);
	free(parent);
	if (r < 0)
		return false;
	return a.st_dev != b.st_dev || a.st_ino == b.st_ino;
}

char *trim(char *s)
{
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
		s++;
	size_t n = strlen(s);
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r'))
		s[--n] = '\0';
	return s;
}

bool str_prefix(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

bool str_suffix(const char *s, const char *suffix)
{
	size_t a = strlen(s), b = strlen(suffix);
	return a >= b && !strcmp(s + a - b, suffix);
}

const char *cmdline(void)
{
	static char *cached;
	if (cached)
		return cached;
	const char *env = getenv("CLEANLINUX_CMDLINE");
	if (env) {
		cached = xstrdup(env);
	} else {
		char *p = rpath("/proc/cmdline");
		cached = read_file(p, NULL);
		free(p);
		if (!cached)
			cached = xstrdup("");
	}
	for (char *c = cached; *c; c++)
		if (*c == '\n')
			*c = ' ';
	return cached;
}

/* next word of the command line, honouring "double quotes" (as the kernel does) */
static const char *next_word(const char *s, char **word)
{
	while (*s == ' ' || *s == '\t')
		s++;
	if (!*s)
		return NULL;
	size_t cap = strlen(s) + 1, n = 0;
	char *w = xmalloc(cap);
	bool quoted = false;
	for (; *s && (quoted || (*s != ' ' && *s != '\t')); s++) {
		if (*s == '"') {
			quoted = !quoted;
			continue;
		}
		w[n++] = *s;
	}
	w[n] = '\0';
	*word = w;
	return s;
}

char *cmdline_value(const char *key)
{
	const char *s = cmdline();
	char *word, *found = NULL;
	size_t kl = strlen(key);
	while ((s = next_word(s, &word))) {
		if (!strncmp(word, key, kl) && (word[kl] == '=' || word[kl] == '\0')) {
			free(found);
			found = xstrdup(word[kl] == '=' ? word + kl + 1 : "");
		}
		free(word);
	}
	return found;
}

void conf_files(const char *dir, const char *suffix, strv *out)
{
	static const char *const bases[] = {"/etc", "/run", "/usr/local/lib", "/usr/lib"};
	strv names = STRV_INIT, paths = STRV_INIT;
	for (size_t b = 0; b < ARRAY_LEN(bases); b++) {
		char *d = xasprintf("%s%s/%s", root, bases[b], dir);
		DIR *dh = opendir(d);
		if (dh) {
			struct dirent *e;
			while ((e = readdir(dh))) {
				if (e->d_name[0] == '.' || !str_suffix(e->d_name, suffix) || strv_contains(&names, e->d_name))
					continue;
				strv_push(&names, e->d_name);
				strv_take(&paths, xasprintf("%s/%s", d, e->d_name));
			}
			closedir(dh);
		}
		free(d);
	}
	/* sort by file name, then drop the masked ones (symlinks to /dev/null, empty files are fine) */
	for (size_t i = 0; i < paths.n; i++)
		for (size_t j = i + 1; j < paths.n; j++)
			if (strcmp(names.v[j], names.v[i]) < 0) {
				char *t = names.v[i];
				names.v[i] = names.v[j];
				names.v[j] = t;
				t = paths.v[i];
				paths.v[i] = paths.v[j];
				paths.v[j] = t;
			}
	for (size_t i = 0; i < paths.n; i++) {
		char target[64];
		ssize_t l = readlink(paths.v[i], target, sizeof(target) - 1);
		if (l > 0) {
			target[l] = '\0';
			if (!strcmp(target, "/dev/null"))
				continue;
		}
		strv_push(out, paths.v[i]);
	}
	strv_free(&names);
	strv_free(&paths);
}

int run(char *const argv[])
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execv(argv[0], argv);
		_exit(127);
	}
	int st;
	while (waitpid(pid, &st, 0) < 0)
		if (errno != EINTR)
			return -1;
	if (WIFEXITED(st))
		return WEXITSTATUS(st) == 127 ? -1 : WEXITSTATUS(st);
	return 128 + WTERMSIG(st);
}

int runl(const char *path, ...)
{
	strv a = STRV_INIT;
	strv_push(&a, path);
	va_list ap;
	va_start(ap, path);
	const char *s;
	while ((s = va_arg(ap, const char *)))
		strv_push(&a, s);
	va_end(ap);
	int r = run(strv_argv(&a));
	strv_free(&a);
	return r;
}

char *find_program(const char *name)
{
	char *p = xasprintf("%s/usr/bin/%s", root, name);
	if (access(p, X_OK) == 0)
		return p;
	free(p);
	return NULL;
}
