/* SPDX-License-Identifier: MIT */
/* kernel: the installed kernels into the EFI system partition, with systemd-boot entries.
 *
 *   ESP/EFI/systemd/systemd-boot<arch>.efi, ESP/EFI/BOOT/BOOT<ARCH>.EFI   the loader
 *   ESP/cleanlinux/<kver>/linux (+ initramfs.img)                       each kernel
 *   ESP/loader/entries/cleanlinux-<kver>.conf                            its entry
 *   ESP/loader/loader.conf                                               (only when missing)
 *
 * Kernels are the /usr/lib/modules/<kver>/vmlinuz of the root; an initramfs is made (and kept in
 * /var/lib/cleanlinux/initramfs) only when cleanlinux/initramfs.d asks for one. The command line is
 * /etc/kernel/cmdline, then the *.conf fragments of kernel/cmdline.d (/etc, /run, /usr/lib)
 * and the profile's /usr/lib/modules/<kver>/cmdline. Entries and kernels that are no longer
 * installed are removed; files are only rewritten when they change. */
#define _GNU_SOURCE
#include "boot.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#define MSDOS_SUPER_MAGIC 0x4d44
#define ENTRY_PREFIX "cleanlinux-"

#if defined(__x86_64__)
#define EFI_ARCH "x64"
#define EFI_ARCH_UP "X64"
#elif defined(__aarch64__)
#define EFI_ARCH "aa64"
#define EFI_ARCH_UP "AA64"
#elif defined(__riscv) && __riscv_xlen == 64
#define EFI_ARCH "riscv64"
#define EFI_ARCH_UP "RISCV64"
#else
#define EFI_ARCH ""
#define EFI_ARCH_UP ""
#endif

static const char *esp;
static bool dry_run;

static bool is_vfat(const char *dir)
{
	struct statfs sf;
	return statfs(dir, &sf) == 0 && sf.f_type == MSDOS_SUPER_MAGIC;
}

/* the mounted ESP: /efi, /boot or /boot/efi of the root, a vfat mount point */
static char *find_esp(void)
{
	static const char *const dirs[] = {"/efi", "/boot", "/boot/efi"};
	for (size_t i = 0; i < ARRAY_LEN(dirs); i++) {
		char *p = rpath(dirs[i]);
		if (is_mountpoint(p) && is_vfat(p))
			return p;
		free(p);
	}
	return NULL;
}

static bool same_content(const char *a, const char *b)
{
	struct stat sa, sb;
	if (stat(a, &sa) < 0 || stat(b, &sb) < 0 || sa.st_size != sb.st_size)
		return false;
	size_t la, lb;
	char *ca = read_file(a, &la), *cb = read_file(b, &lb);
	bool same = ca && cb && la == lb && !memcmp(ca, cb, la);
	free(ca);
	free(cb);
	return same;
}

/* write DATA to PATH on the ESP when it differs: a temporary file, fsync, rename (FAT has no
 * modes, so nothing is chmod-ed) */
static int esp_write(const char *path, const void *data, size_t len)
{
	size_t old_len;
	char *old = read_file(path, &old_len);
	bool same = old && old_len == len && !memcmp(old, data, len);
	free(old);
	if (same)
		return 0;
	msg("%s %s", dry_run ? "would write" : "writing", path);
	if (dry_run)
		return 0;
	char *dir = xstrdup(path);
	*strrchr(dir, '/') = '\0';
	mkdir_p(dir, 0755);
	free(dir);
	char *tmp = xasprintf("%s.new", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	int rc = -1;
	if (fd >= 0) {
		const char *p = data;
		size_t left = len;
		while (left) {
			ssize_t w = write(fd, p, left);
			if (w < 0 && errno == EINTR)
				continue;
			if (w <= 0)
				break;
			p += w;
			left -= (size_t)w;
		}
		if (!left && fsync(fd) == 0)
			rc = 0;
		if (close(fd) < 0)
			rc = -1;
	}
	if (rc == 0 && rename(tmp, path) < 0)
		rc = -1;
	if (rc < 0) {
		warn_errno("cannot write %s", path);
		unlink(tmp);
	}
	free(tmp);
	return rc;
}

static int esp_copy(const char *src, const char *dst)
{
	if (same_content(src, dst))
		return 0;
	size_t len;
	char *data = read_file(src, &len);
	if (!data) {
		warn_errno("%s", src);
		return -1;
	}
	int rc = esp_write(dst, data, len);
	free(data);
	return rc;
}

static void remove_tree(const char *path)
{
	DIR *d = opendir(path);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			char *p = xasprintf("%s/%s", path, e->d_name);
			if (e->d_type == DT_DIR)
				remove_tree(p);
			else if (unlink(p) < 0)
				warn_errno("cannot remove %s", p);
			free(p);
		}
		closedir(d);
	}
	if (rmdir(path) < 0 && errno != ENOENT)
		warn_errno("cannot remove %s", path);
}

/* the installed kernels: <kver> of /usr/lib/modules/<kver>/vmlinuz, sorted */
static void installed_kernels(strv *out)
{
	char *base = rpath("/usr/lib/modules");
	DIR *d = opendir(base);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.')
				continue;
			char *img = xasprintf("%s/%s/vmlinuz", base, e->d_name);
			if (is_file(img))
				strv_push(out, e->d_name);
			free(img);
		}
		closedir(d);
	}
	free(base);
	strv_sort(out);
}

/* words of a file, comments (#) and line breaks dropped, appended to *line */
static void append_words(char **line, const char *path)
{
	char *s = read_file(path, NULL);
	if (!s)
		return;
	for (char *save = NULL, *l = strtok_r(s, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		l = trim(l);
		if (!*l || *l == '#')
			continue;
		char *n = **line ? xasprintf("%s %s", *line, l) : xstrdup(l);
		free(*line);
		*line = n;
	}
	free(s);
}

/* the command line, without the words a boot loader adds itself */
static char *proc_cmdline(void)
{
	char *out = xstrdup("");
	char *s = xstrdup(cmdline());
	for (char *save = NULL, *w = strtok_r(s, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
		if (str_prefix(w, "BOOT_IMAGE=") || str_prefix(w, "initrd="))
			continue;
		char *n = *out ? xasprintf("%s %s", out, w) : xstrdup(w);
		free(out);
		out = n;
	}
	free(s);
	return out;
}

static char *kernel_cmdline(const char *kver)
{
	char *line = xstrdup("");
	char *p = rpath("/etc/kernel/cmdline");
	if (is_file(p)) {
		append_words(&line, p);
	} else if (!*root) {
		/* no /etc/kernel/cmdline: keep what this boot was started with */
		free(line);
		line = proc_cmdline();
	}
	free(p);
	strv frags = STRV_INIT;
	conf_files("kernel/cmdline.d", ".conf", &frags);
	for (size_t i = 0; i < frags.n; i++)
		append_words(&line, frags.v[i]);
	strv_free(&frags);
	p = xasprintf("%s/usr/lib/modules/%s/cmdline", root, kver);
	append_words(&line, p);
	free(p);
	return line;
}

static char *os_name(void)
{
	char *p = rpath("/usr/lib/os-release");
	char *s = read_file(p, NULL), *name = NULL;
	free(p);
	for (char *save = NULL, *l = s ? strtok_r(s, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save)) {
		if (!str_prefix(l, "NAME="))
			continue;
		char *v = l + 5;
		size_t n = strlen(v);
		if (n >= 2 && (*v == '"' || *v == '\'') && v[n - 1] == *v) {
			v[n - 1] = '\0';
			v++;
		}
		name = xstrdup(v);
	}
	free(s);
	return name ? name : xstrdup("CleanLinux");
}

static int sync_kernel(const char *kver, const char *title)
{
	int rc = 0;
	char *src = xasprintf("%s/usr/lib/modules/%s/vmlinuz", root, kver);
	char *dst = xasprintf("%s/cleanlinux/%s/linux", esp, kver);
	if (esp_copy(src, dst) < 0)
		rc = 1;
	free(src);
	free(dst);
	/* an initramfs only for the systems that need one (cleanlinux/initramfs.d) */
	char *irs = xasprintf("%s" STATE_DIR "/initramfs/%s.img", root, kver);
	char *ird = xasprintf("%s/cleanlinux/%s/initramfs.img", esp, kver);
	if (initramfs_wanted()) {
		if (!dry_run) {
			char *dir = xasprintf("%s" STATE_DIR "/initramfs", root);
			mkdir_p(dir, 0700);
			free(dir);
			if (initramfs_build(kver, irs) < 0)
				rc = 1;
		}
	} else if (exists(irs) && !dry_run) {
		unlink(irs);
	}
	bool have_initrd = is_file(irs);
	if (have_initrd) {
		if (esp_copy(irs, ird) < 0)
			rc = 1;
	} else if (exists(ird) && !dry_run) {
		unlink(ird);
	}
	free(irs);
	free(ird);
	char *opts = kernel_cmdline(kver);
	char *entry = xasprintf("title %s\nversion %s\nsort-key cleanlinux\nlinux /cleanlinux/%s/linux\n%s%s%soptions %s\n",
				title, kver, kver, have_initrd ? "initrd /cleanlinux/" : "", have_initrd ? kver : "",
				have_initrd ? "/initramfs.img\n" : "", opts);
	char *ep = xasprintf("%s/loader/entries/" ENTRY_PREFIX "%s.conf", esp, kver);
	if (esp_write(ep, entry, strlen(entry)) < 0)
		rc = 1;
	if (!*opts)
		warnx("kernel %s: empty command line (no /etc/kernel/cmdline), the kernel will not find its root", kver);
	free(ep);
	free(entry);
	free(opts);
	return rc;
}

/* entries and kernel directories of kernels that are gone */
static void remove_stale(const strv *kernels)
{
	char *dir = xasprintf("%s/loader/entries", esp);
	DIR *d = opendir(dir);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			if (!str_prefix(e->d_name, ENTRY_PREFIX) || !str_suffix(e->d_name, ".conf"))
				continue;
			char *kver = xstrndup(e->d_name + strlen(ENTRY_PREFIX), strlen(e->d_name) - strlen(ENTRY_PREFIX) - 5);
			if (!strv_contains(kernels, kver)) {
				char *p = xasprintf("%s/%s", dir, e->d_name);
				msg("%s %s", dry_run ? "would remove" : "removing", p);
				if (!dry_run && unlink(p) < 0)
					warn_errno("cannot remove %s", p);
				free(p);
			}
			free(kver);
		}
		closedir(d);
	}
	free(dir);
	dir = xasprintf("%s" STATE_DIR "/initramfs", root);
	d = opendir(dir);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			if (!str_suffix(e->d_name, ".img"))
				continue;
			char *kver = xstrndup(e->d_name, strlen(e->d_name) - 4);
			if (!strv_contains(kernels, kver) && !dry_run) {
				char *p = xasprintf("%s/%s", dir, e->d_name);
				unlink(p);
				free(p);
			}
			free(kver);
		}
		closedir(d);
	}
	free(dir);
	dir = xasprintf("%s/cleanlinux", esp);
	d = opendir(dir);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.' || strv_contains(kernels, e->d_name))
				continue;
			char *p = xasprintf("%s/%s", dir, e->d_name);
			msg("%s %s", dry_run ? "would remove" : "removing", p);
			if (!dry_run)
				remove_tree(p);
			free(p);
		}
		closedir(d);
	}
	free(dir);
}

/* systemd-boot from the systemd-boot package, as the default loader of removable media
 * (EFI/BOOT/BOOT<ARCH>.EFI, which every firmware starts without a boot variable) and in its
 * own directory */
static int sync_loader(void)
{
	if (!*EFI_ARCH)
		return 0;
	char *src = xasprintf("%s/usr/lib/systemd/boot/efi/systemd-boot" EFI_ARCH ".efi", root);
	if (!is_file(src)) {
		free(src);
		return 0; /* not installed: another loader boots this system */
	}
	int rc = 0;
	char *a = xasprintf("%s/EFI/systemd/systemd-boot" EFI_ARCH ".efi", esp);
	char *b = xasprintf("%s/EFI/BOOT/BOOT" EFI_ARCH_UP ".EFI", esp);
	if (esp_copy(src, a) < 0 || esp_copy(src, b) < 0)
		rc = 1;
	free(a);
	free(b);
	free(src);
	char *conf = xasprintf("%s/loader/loader.conf", esp);
	if (!exists(conf)) {
		static const char text[] = "default " ENTRY_PREFIX "*\ntimeout 3\nconsole-mode keep\n";
		if (esp_write(conf, text, sizeof(text) - 1) < 0)
			rc = 1;
	}
	free(conf);
	return rc;
}

/* kernel sync|list [--esp DIR] [--dry-run] */
int cmd_kernel(int argc, char **argv)
{
	const char *sub = argc > 1 ? argv[1] : "sync";
	char *esp_owned = NULL;
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--esp") && i + 1 < argc)
			esp = argv[++i];
		else if (!strcmp(argv[i], "--dry-run"))
			dry_run = true;
		else
			die("usage: kernel sync|list [--esp DIR] [--dry-run]");
	}
	strv kernels = STRV_INIT;
	installed_kernels(&kernels);
	if (!strcmp(sub, "list")) {
		for (size_t i = 0; i < kernels.n; i++) {
			char *opts = kernel_cmdline(kernels.v[i]);
			printf("%s\t%s\n", kernels.v[i], opts);
			free(opts);
		}
		strv_free(&kernels);
		return 0;
	}
	if (strcmp(sub, "sync"))
		die("usage: kernel sync|list [--esp DIR] [--dry-run]");
	if (!esp) {
		const char *env = getenv("CLEANLINUX_ESP");
		if (env && *env)
			esp = env;
		else
			esp = esp_owned = find_esp();
	}
	if (!esp) {
		msg("no EFI system partition mounted at /efi, /boot or /boot/efi: nothing to do");
		strv_free(&kernels);
		return 0;
	}
	int rc = 0;
	char *title = os_name();
	for (size_t i = 0; i < kernels.n; i++)
		if (sync_kernel(kernels.v[i], title))
			rc = 1;
	free(title);
	remove_stale(&kernels);
	if (sync_loader())
		rc = 1;
	if (!dry_run)
		sync();
	strv_free(&kernels);
	free(esp_owned);
	return rc;
}
