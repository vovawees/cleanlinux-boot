/* SPDX-License-Identifier: MIT */
/* kernel: the installed kernels into the boot partition (the EFI system partition, a vfat file
 * system), for the boot loaders of the device (cleanlinux/boot.d, loader=):
 *
 *   ESP/cleanlinux/<kver>/linux (+ initramfs.img, dtbs/)                 each kernel
 *   systemd-boot  ESP/EFI/systemd/systemd-boot<arch>.efi, ESP/EFI/BOOT/BOOT<ARCH>.EFI   (UEFI, default)
 *                 ESP/loader/entries/cleanlinux-<kver>.conf, ESP/loader/loader.conf (only when missing)
 *   extlinux      ESP/extlinux/extlinux.conf (U-Boot), the newest kernel first
 *   rpi           ESP/config.txt + ESP/cleanlinux/<kver>/cmdline.txt (the Raspberry Pi firmware)
 *   android       /var/lib/cleanlinux/android/<kver>.img: Android boot images (Qualcomm ABL...),
 *                 the default one flashed into android.partition on the running system
 *
 * Kernels are the /usr/lib/modules/<kver>/vmlinuz of the root (U-Boot, the Raspberry Pi firmware
 * and Android boot images take /usr/lib/modules/<kver>/Image when there is one: on arm64 vmlinuz is
 * an EFI program); device trees come from /usr/lib/modules/<kver>/dtbs. An initramfs is made (and
 * kept in /var/lib/cleanlinux/initramfs) only when cleanlinux/initramfs.d asks for one. The command
 * line is /etc/kernel/cmdline, then the *.conf fragments of kernel/cmdline.d (/etc, /run, /usr/lib)
 * and the profile's /usr/lib/modules/<kver>/cmdline. Entries, kernels and files that are no longer
 * needed are removed; files are only rewritten when they change. */
#define _GNU_SOURCE
#include "boot.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <unistd.h>

#define MSDOS_SUPER_MAGIC 0x4d44
#define ENTRY_PREFIX "cleanlinux-"
/* the first line of the files this writes for U-Boot and the Raspberry Pi firmware */
#define MARK "# CleanLinux: written by cleanlinux-boot kernel sync"

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
static bool dry_run, flash;

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

/* the files of the kernel being synced: the others in its directory are removed afterwards */
static strv *keep;

static void note(const char *path)
{
	if (keep && !strv_contains(keep, path))
		strv_push(keep, path);
}

/* write DATA to PATH on the ESP when it differs: a temporary file, fsync, rename (FAT has no
 * modes, so nothing is chmod-ed) */
static int esp_write(const char *path, const void *data, size_t len)
{
	note(path);
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
	note(dst);
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

static int newest_first(const void *a, const void *b)
{
	return strverscmp(*(char *const *)b, *(char *const *)a);
}

/* the installed kernels: <kver> of /usr/lib/modules/<kver>/vmlinuz, the newest first */
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
	if (out->n > 1)
		qsort(out->v, out->n, sizeof(*out->v), newest_first);
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

/* ---- the boot loaders of the device: the .conf files of cleanlinux/boot.d ---- */

enum { L_SDBOOT = 1, L_EXTLINUX = 2, L_RPI = 4, L_ANDROID = 8 };
static const struct {
	const char *name;
	unsigned bit;
} loader_names[] = {{"systemd-boot", L_SDBOOT}, {"extlinux", L_EXTLINUX}, {"rpi", L_RPI}, {"android", L_ANDROID}};

typedef struct {
	unsigned loaders;
	char *devicetree, *def, *image, *compress, *partition, *board, *os_version, *os_patch_level;
	strv overlays;
	unsigned timeout;
	unsigned header;
	uint32_t pagesize, base, kernel_offset, ramdisk_offset, tags_offset, dtb_offset;
} bootconf;

static bool parse_u32(const char *s, uint32_t *out)
{
	char *end;
	errno = 0;
	unsigned long long v = strtoull(s, &end, 0);
	if (errno || end == s || *end || *s == '-' || v > UINT32_MAX)
		return false;
	*out = (uint32_t)v;
	return true;
}

static void set_str(char **dst, const char *v)
{
	free(*dst);
	*dst = xstrdup(v);
}

static char *dt_clean(const char *file, const char *key, const char *v);

static void bootconf_set(bootconf *bc, const char *file, const char *k, const char *v)
{
	static const struct {
		const char *key;
		size_t off;
	} nums[] = {
		{"android.pagesize", offsetof(bootconf, pagesize)},
		{"android.base", offsetof(bootconf, base)},
		{"android.kernel_offset", offsetof(bootconf, kernel_offset)},
		{"android.ramdisk_offset", offsetof(bootconf, ramdisk_offset)},
		{"android.tags_offset", offsetof(bootconf, tags_offset)},
		{"android.dtb_offset", offsetof(bootconf, dtb_offset)},
	};
	uint32_t n;
	if (!strcmp(k, "loader")) {
		bc->loaders = 0;
		char *list = xstrdup(v);
		for (char *save = NULL, *w = strtok_r(list, " \t,", &save); w; w = strtok_r(NULL, " \t,", &save)) {
			size_t i = 0;
			while (i < ARRAY_LEN(loader_names) && strcmp(loader_names[i].name, w))
				i++;
			if (i < ARRAY_LEN(loader_names))
				bc->loaders |= loader_names[i].bit;
			else if (strcmp(w, "none"))
				warnx("%s: unknown loader '%s' (systemd-boot, extlinux, rpi, android, none)", file, w);
		}
		free(list);
	} else if (!strcmp(k, "devicetree")) {
		char *dt = *v ? dt_clean(file, k, v) : xstrdup("");
		if (dt) {
			free(bc->devicetree);
			bc->devicetree = dt;
		}
	} else if (!strcmp(k, "overlays")) {
		strv_free(&bc->overlays);
		char *list = xstrdup(v);
		for (char *save = NULL, *w = strtok_r(list, " \t,", &save); w; w = strtok_r(NULL, " \t,", &save)) {
			char *o = dt_clean(file, k, w);
			if (o && o[strlen(o) - 1] == '/') {
				warnx("%s: overlays=%s: files, not directories", file, w);
				free(o);
			} else if (o) {
				strv_take(&bc->overlays, o);
			}
		}
		free(list);
	} else if (!strcmp(k, "default")) {
		set_str(&bc->def, v);
	} else if (!strcmp(k, "image")) {
		if (strchr(v, '/') || !strcmp(v, "..") || !strcmp(v, "."))
			warnx("%s: image=%s: a file name in /usr/lib/modules/<kver>", file, v);
		else
			set_str(&bc->image, v);
	} else if (!strcmp(k, "timeout")) {
		if (parse_u32(v, &n) && n <= 600)
			bc->timeout = n;
		else
			warnx("%s: timeout=%s: seconds, 0 to 600", file, v);
	} else if (!strcmp(k, "android.header")) {
		if (parse_u32(v, &n) && n <= 2)
			bc->header = n;
		else
			warnx("%s: android.header=%s: 0, 1 or 2", file, v);
	} else if (!strcmp(k, "android.compress")) {
		if (strcmp(v, "gzip") && strcmp(v, "none"))
			warnx("%s: android.compress=%s: gzip or none", file, v);
		else
			set_str(&bc->compress, v);
	} else if (!strcmp(k, "android.partition")) {
		set_str(&bc->partition, v);
	} else if (!strcmp(k, "android.board")) {
		set_str(&bc->board, v);
	} else if (!strcmp(k, "android.os_version")) {
		set_str(&bc->os_version, v);
	} else if (!strcmp(k, "android.os_patch_level")) {
		set_str(&bc->os_patch_level, v);
	} else {
		for (size_t i = 0; i < ARRAY_LEN(nums); i++)
			if (!strcmp(k, nums[i].key)) {
				if (parse_u32(v, &n))
					*(uint32_t *)((char *)bc + nums[i].off) = n;
				else
					warnx("%s: %s=%s: a number (0x... for hex)", file, k, v);
				return;
			}
		warnx("%s: unknown setting %s", file, k);
	}
}

/* the defaults (systemd-boot, the addresses of mkbootimg), then the files in name order: a later
 * setting wins */
static void bootconf_load(bootconf *bc)
{
	*bc = (bootconf){.loaders = L_SDBOOT, .timeout = 3, .header = 2, .pagesize = 4096,
			 .base = 0x10000000, .kernel_offset = 0x00008000, .ramdisk_offset = 0x01000000,
			 .tags_offset = 0x00000100, .dtb_offset = 0x01f00000};
	bc->overlays = (strv)STRV_INIT;
	bc->compress = xstrdup("gzip");
	strv files = STRV_INIT;
	conf_files("cleanlinux/boot.d", ".conf", &files);
	for (size_t f = 0; f < files.n; f++) {
		char *data = read_file(files.v[f], NULL);
		if (!data) {
			warn_errno("%s", files.v[f]);
			continue;
		}
		for (char *save = NULL, *l = strtok_r(data, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
			l = trim(l);
			if (!*l || *l == '#')
				continue;
			char *eq = strchr(l, '=');
			if (!eq) {
				warnx("%s: not KEY=VALUE: %s", files.v[f], l);
				continue;
			}
			*eq = '\0';
			char *k = trim(l), *v = trim(eq + 1);
			size_t n = strlen(v);
			if (n >= 2 && (*v == '"' || *v == '\'') && v[n - 1] == *v) {
				v[n - 1] = '\0';
				v++;
			}
			bootconf_set(bc, files.v[f], k, v);
		}
		free(data);
	}
	strv_free(&files);
}

static void bootconf_free(bootconf *bc)
{
	free(bc->devicetree);
	free(bc->def);
	free(bc->image);
	free(bc->compress);
	free(bc->partition);
	free(bc->board);
	free(bc->os_version);
	free(bc->os_patch_level);
	strv_free(&bc->overlays);
}

static const char *or_none(const char *s)
{
	return s && *s ? s : "-";
}

/* append to a malloc-ed string */
__attribute__((format(printf, 2, 3))) static void addf(char **s, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char *more;
	if (vasprintf(&more, fmt, ap) < 0)
		die("out of memory");
	va_end(ap);
	char *n = xasprintf("%s%s", *s ? *s : "", more);
	free(more);
	free(*s);
	*s = n;
}

/* a path below dtbs/: no "..", no empty or "." components (a trailing / stays: a directory) */
static char *dt_clean(const char *file, const char *key, const char *v)
{
	if (*v == '/')
		return xstrdup(v);
	char *out = NULL, *copy = xstrdup(v);
	bool dir = *v && v[strlen(v) - 1] == '/', bad = false;
	for (char *save = NULL, *c = strtok_r(copy, "/", &save); c; c = strtok_r(NULL, "/", &save)) {
		if (!strcmp(c, "."))
			continue;
		if (!strcmp(c, "..")) {
			bad = true;
			break;
		}
		addf(&out, "%s%s", out ? "/" : "", c);
	}
	free(copy);
	if (bad || !out) {
		warnx("%s: %s=%s: a path below /usr/lib/modules/<kver>/dtbs, or an absolute one", file, key, v);
		free(out);
		return NULL;
	}
	if (dir)
		addf(&out, "/");
	return out;
}

static void bootconf_print(const bootconf *bc, const char *def)
{
	char *names = xstrdup("");
	for (size_t i = 0; i < ARRAY_LEN(loader_names); i++)
		if (bc->loaders & loader_names[i].bit) {
			char *n = xasprintf("%s%s%s", names, *names ? " " : "", loader_names[i].name);
			free(names);
			names = n;
		}
	printf("loader=%s\n", *names ? names : "none");
	free(names);
	printf("devicetree=%s\n", or_none(bc->devicetree));
	printf("overlays=");
	for (size_t i = 0; i < bc->overlays.n; i++)
		printf("%s%s", i ? " " : "", bc->overlays.v[i]);
	printf("%s\ndefault=%s\nimage=%s\ntimeout=%u\n", bc->overlays.n ? "" : "-", or_none(def), or_none(bc->image),
	       bc->timeout);
	if (bc->loaders & L_ANDROID)
		printf("android.header=%u\nandroid.pagesize=%u\nandroid.base=0x%08x\nandroid.kernel_offset=0x%08x\n"
		       "android.ramdisk_offset=0x%08x\nandroid.tags_offset=0x%08x\nandroid.dtb_offset=0x%08x\n"
		       "android.os_version=%s\nandroid.os_patch_level=%s\nandroid.board=%s\nandroid.compress=%s\n"
		       "android.partition=%s\n",
		       bc->header, bc->pagesize, bc->base, bc->kernel_offset, bc->ramdisk_offset, bc->tags_offset,
		       bc->dtb_offset, or_none(bc->os_version), or_none(bc->os_patch_level), or_none(bc->board),
		       bc->compress, or_none(bc->partition));
}

/* ---- each kernel ---- */

/* what the loaders are given for one kernel; ESP paths start with a / */
typedef struct {
	const char *kver;
	char *cmdline;
	char *image;    /* the kernel of the non-UEFI loaders: ESP path */
	char *initrd;   /* ESP path or NULL */
	char *fdt;      /* the device tree: ESP path, or NULL */
	char *fdtdir;   /* devicetree=vendor/: the directory U-Boot picks $fdtfile from */
	strv overlays;  /* ESP paths */
	char *rpi_fdt;  /* Raspberry Pi: file names in the os_prefix directory */
	strv rpi_overlays;
} kinfo;

static void kinfo_free(kinfo *k)
{
	free(k->cmdline);
	free(k->image);
	free(k->initrd);
	free(k->fdt);
	free(k->fdtdir);
	free(k->rpi_fdt);
	strv_free(&k->overlays);
	strv_free(&k->rpi_overlays);
}

/* the kernel image for U-Boot, the Raspberry Pi firmware and Android boot images, inside the root */
static char *raw_image(const bootconf *bc, const char *kver)
{
	if (bc->image)
		return xasprintf("%s/usr/lib/modules/%s/%s", root, kver, bc->image);
	char *p = xasprintf("%s/usr/lib/modules/%s/Image", root, kver);
	if (is_file(p))
		return p;
	free(p);
	return xasprintf("%s/usr/lib/modules/%s/vmlinuz", root, kver);
}

static const char *base_name(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

static bool is_dir_path(const char *p)
{
	return *p && p[strlen(p) - 1] == '/';
}

/* a device tree (file or directory) of the kernel: devicetree=vendor/board.dtb (or vendor/) of
 * /usr/lib/modules/<kver>/dtbs, or an absolute path inside the root; without a trailing / */
static char *dt_source(const char *kver, const char *dt)
{
	char *p = *dt == '/' ? rpath(dt) : xasprintf("%s/usr/lib/modules/%s/dtbs/%s", root, kver, dt);
	size_t n = strlen(p);
	while (n > 1 && p[n - 1] == '/')
		p[--n] = '\0';
	return p;
}

/* its path below ESP/cleanlinux/<kver>/dtbs, without a trailing / */
static char *dt_rel(const char *dt)
{
	char *p = xstrdup(dt);
	size_t n = strlen(p);
	while (n > 1 && p[n - 1] == '/')
		p[--n] = '\0';
	if (*dt == '/') {
		char *b = xstrdup(base_name(p));
		free(p);
		p = b;
	}
	return p;
}

/* the files of directory SRC into DST, recursively */
static int esp_copy_tree(const char *src, const char *dst)
{
	DIR *d = opendir(src);
	if (!d) {
		warn_errno("%s", src);
		return -1;
	}
	int rc = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char *s = xasprintf("%s/%s", src, e->d_name), *t = xasprintf("%s/%s", dst, e->d_name);
		if (is_dir(s)) {
			if (esp_copy_tree(s, t) < 0)
				rc = -1;
		} else if (is_file(s) && esp_copy(s, t) < 0) {
			rc = -1;
		}
		free(s);
		free(t);
	}
	closedir(d);
	return rc;
}

/* files of DIR that are not in KEEP, and the directories left empty */
static void prune(const char *dir, const strv *kept)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char *p = xasprintf("%s/%s", dir, e->d_name);
		if (is_dir(p)) {
			prune(p, kept);
			if (!dry_run)
				rmdir(p); /* only when empty */
		} else if (!strv_contains(kept, p)) {
			msg("%s %s", dry_run ? "would remove" : "removing", p);
			if (!dry_run && unlink(p) < 0)
				warn_errno("cannot remove %s", p);
		}
		free(p);
	}
	closedir(d);
}

/* the initramfs of KVER in /var/lib/cleanlinux/initramfs when cleanlinux/initramfs.d asks for one:
 * its path, or NULL */
static char *prepare_initramfs(const char *kver, int *rc)
{
	char *irs = xasprintf("%s" STATE_DIR "/initramfs/%s.img", root, kver);
	if (initramfs_wanted()) {
		if (!dry_run) {
			char *dir = xasprintf("%s" STATE_DIR "/initramfs", root);
			mkdir_p(dir, 0700);
			free(dir);
			if (initramfs_build(kver, irs) < 0)
				*rc = 1;
		}
	} else if (exists(irs) && !dry_run) {
		unlink(irs);
	}
	if (is_file(irs))
		return irs;
	free(irs);
	return NULL;
}

static int sync_kernel(const bootconf *bc, kinfo *k, const char *initrd, const char *title)
{
	int rc = 0;
	const char *kver = k->kver;
	strv kept = STRV_INIT;
	keep = &kept;
	char *kdir = xasprintf("%s/cleanlinux/%s", esp, kver);
	/* the kernel: vmlinuz as linux for systemd-boot, and the Image of the other loaders when the
	 * kernel has one */
	bool raw_loaders = bc->loaders & (L_EXTLINUX | L_RPI);
	char *raw = raw_image(bc, kver), *src = xasprintf("%s/usr/lib/modules/%s/vmlinuz", root, kver);
	bool raw_is_vmlinuz = !strcmp(raw, src);
	if ((bc->loaders & L_SDBOOT) || (raw_loaders && raw_is_vmlinuz)) {
		char *dst = xasprintf("%s/linux", kdir);
		if (esp_copy(src, dst) < 0)
			rc = 1;
		free(dst);
	}
	if (raw_loaders) {
		const char *name = raw_is_vmlinuz ? "linux" : base_name(raw);
		if (!raw_is_vmlinuz) {
			char *dst = xasprintf("%s/%s", kdir, name);
			if (esp_copy(raw, dst) < 0)
				rc = 1;
			free(dst);
		}
		k->image = xasprintf("/cleanlinux/%s/%s", kver, name);
	}
	free(raw);
	free(src);
	if (initrd) {
		char *dst = xasprintf("%s/initramfs.img", kdir);
		if (esp_copy(initrd, dst) < 0)
			rc = 1;
		free(dst);
		k->initrd = xasprintf("/cleanlinux/%s/initramfs.img", kver);
	}
	/* device trees: in dtbs/ for systemd-boot and U-Boot, next to the kernel for the firmware of
	 * the Raspberry Pi (os_prefix covers the device tree and overlays/) */
	const char *dt = bc->devicetree && *bc->devicetree ? bc->devicetree : NULL;
	bool dt_dir = dt && is_dir_path(dt);
	if (dt && (bc->loaders & (L_SDBOOT | L_EXTLINUX))) {
		char *s = dt_source(kver, dt), *rel = dt_rel(dt), *t = xasprintf("%s/dtbs/%s", kdir, rel);
		if (dt_dir ? esp_copy_tree(s, t) < 0 : esp_copy(s, t) < 0)
			rc = 1;
		if (dt_dir)
			k->fdtdir = xasprintf("/cleanlinux/%s/dtbs", kver);
		else
			k->fdt = xasprintf("/cleanlinux/%s/dtbs/%s", kver, rel);
		free(s);
		free(rel);
		free(t);
	}
	for (size_t i = 0; i < bc->overlays.n && (bc->loaders & (L_SDBOOT | L_EXTLINUX)); i++) {
		char *s = dt_source(kver, bc->overlays.v[i]), *rel = dt_rel(bc->overlays.v[i]);
		char *t = xasprintf("%s/dtbs/%s", kdir, rel);
		if (esp_copy(s, t) < 0)
			rc = 1;
		strv_take(&k->overlays, xasprintf("/cleanlinux/%s/dtbs/%s", kver, rel));
		free(s);
		free(rel);
		free(t);
	}
	if (bc->loaders & L_RPI) {
		char *line = xasprintf("%s\n", k->cmdline), *p = xasprintf("%s/cmdline.txt", kdir);
		if (esp_write(p, line, strlen(line)) < 0)
			rc = 1;
		free(p);
		free(line);
		if (dt_dir) {
			/* the firmware picks the device tree of the board itself */
			char *s = dt_source(kver, dt);
			DIR *d = opendir(s);
			struct dirent *e;
			while (d && (e = readdir(d))) {
				if (!str_suffix(e->d_name, ".dtb"))
					continue;
				char *f = xasprintf("%s/%s", s, e->d_name), *t = xasprintf("%s/%s", kdir, e->d_name);
				if (esp_copy(f, t) < 0)
					rc = 1;
				free(f);
				free(t);
			}
			if (d) {
				closedir(d);
			} else {
				warn_errno("%s", s);
				rc = 1;
			}
			free(s);
		} else if (dt) {
			char *s = dt_source(kver, dt), *t = xasprintf("%s/%s", kdir, base_name(s));
			if (esp_copy(s, t) < 0)
				rc = 1;
			k->rpi_fdt = xstrdup(base_name(s));
			free(s);
			free(t);
		}
		for (size_t i = 0; i < bc->overlays.n; i++) {
			char *s = dt_source(kver, bc->overlays.v[i]), *t = xasprintf("%s/overlays/%s", kdir, base_name(s));
			if (esp_copy(s, t) < 0)
				rc = 1;
			char *name = xstrdup(base_name(s));
			if (str_suffix(name, ".dtbo"))
				name[strlen(name) - 5] = '\0';
			strv_take(&k->rpi_overlays, name);
			free(s);
			free(t);
		}
	}
	/* the systemd-boot entry */
	if (bc->loaders & L_SDBOOT) {
		char *entry = NULL;
		addf(&entry, "title %s\nversion %s\nsort-key cleanlinux\nlinux /cleanlinux/%s/linux\n", title, kver, kver);
		if (k->initrd)
			addf(&entry, "initrd %s\n", k->initrd);
		if (k->fdt)
			addf(&entry, "devicetree %s\n", k->fdt);
		for (size_t i = 0; i < k->overlays.n; i++)
			addf(&entry, "%s%s%s", i ? " " : "devicetree-overlay ", k->overlays.v[i], i + 1 == k->overlays.n ? "\n" : "");
		addf(&entry, "options %s\n", k->cmdline);
		char *ep = xasprintf("%s/loader/entries/" ENTRY_PREFIX "%s.conf", esp, kver);
		keep = NULL;
		if (esp_write(ep, entry, strlen(entry)) < 0)
			rc = 1;
		free(ep);
		free(entry);
	}
	keep = NULL;
	prune(kdir, &kept);
	strv_free(&kept);
	free(kdir);
	return rc;
}

/* entries and kernel directories of kernels that are gone; all the entries when systemd-boot is
 * not a loader of the device */
static void remove_stale(const strv *kernels, bool entries)
{
	char *dir = xasprintf("%s/loader/entries", esp);
	DIR *d = opendir(dir);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			if (!str_prefix(e->d_name, ENTRY_PREFIX) || !str_suffix(e->d_name, ".conf"))
				continue;
			char *kver = xstrndup(e->d_name + strlen(ENTRY_PREFIX), strlen(e->d_name) - strlen(ENTRY_PREFIX) - 5);
			if (!entries || !strv_contains(kernels, kver)) {
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

/* a file written by this (MARK on its first line) that its loader no longer needs */
static void remove_ours(const char *path)
{
	char *s = read_file(path, NULL);
	if (s && str_prefix(s, MARK)) {
		msg("%s %s", dry_run ? "would remove" : "removing", path);
		if (!dry_run && unlink(path) < 0)
			warn_errno("cannot remove %s", path);
	}
	free(s);
}

/* a file this did not write is left alone */
static int write_ours(const char *path, const char *text)
{
	char *s = read_file(path, NULL);
	bool foreign = s && !str_prefix(s, MARK);
	free(s);
	if (foreign) {
		warnx("%s was not written by cleanlinux-boot: left alone (move it away to have it written)", path);
		return 1;
	}
	return esp_write(path, text, strlen(text)) < 0;
}

/* U-Boot: ESP/extlinux/extlinux.conf, the default kernel first, then the others newest first
 * (the keywords in lower case: U-Boot reads them so) */
static int sync_extlinux(const bootconf *bc, const kinfo *ks, size_t n, size_t def, const char *title)
{
	char *path = xasprintf("%s/extlinux/extlinux.conf", esp);
	if (!(bc->loaders & L_EXTLINUX)) {
		remove_ours(path);
		free(path);
		return 0;
	}
	char *text = NULL;
	addf(&text, MARK ", edits are lost: the settings are in /etc/cleanlinux/boot.d\n");
	addf(&text, "menu title %s\ntimeout %u\n", title, bc->timeout * 10);
	if (n)
		addf(&text, "default " ENTRY_PREFIX "%s\n", ks[def].kver);
	for (size_t j = 0; j < n; j++) {
		const kinfo *k = &ks[j == 0 ? def : j <= def ? j - 1 : j];
		addf(&text, "\nlabel " ENTRY_PREFIX "%s\n\tmenu label %s %s\n\tlinux %s\n", k->kver, title, k->kver, k->image);
		if (k->initrd)
			addf(&text, "\tinitrd %s\n", k->initrd);
		if (k->fdt)
			addf(&text, "\tfdt %s\n", k->fdt);
		if (k->fdtdir)
			addf(&text, "\tfdtdir %s\n", k->fdtdir);
		for (size_t i = 0; i < k->overlays.n; i++)
			addf(&text, "%s%s%s", i ? " " : "\tfdtoverlays ", k->overlays.v[i], i + 1 == k->overlays.n ? "\n" : "");
		addf(&text, "\tappend %s\n", k->cmdline);
	}
	int rc = write_ours(path, text);
	free(text);
	free(path);
	return rc;
}

/* the Raspberry Pi firmware: ESP/config.txt boots the default kernel from its directory
 * (os_prefix: the kernel, initramfs, cmdline.txt, device tree and overlays/ are read there) */
static int sync_rpi(const bootconf *bc, const kinfo *k)
{
	char *path = xasprintf("%s/config.txt", esp);
	if (!(bc->loaders & L_RPI) || !k) {
		remove_ours(path);
		free(path);
		return 0;
	}
	char *text = NULL;
	addf(&text, MARK ", edits are lost: the settings are in\n"
		    "# /etc/cleanlinux/boot.d, those of the firmware in usercfg.txt\n");
	addf(&text, "[all]\narm_64bit=1\nupstream_kernel=1\nos_prefix=cleanlinux/%s/\nkernel=%s\n", k->kver,
	     base_name(k->image));
	if (k->initrd)
		addf(&text, "initramfs initramfs.img followkernel\n");
	if (k->rpi_fdt)
		addf(&text, "device_tree=%s\n", k->rpi_fdt);
	for (size_t i = 0; i < k->rpi_overlays.n; i++)
		addf(&text, "dtoverlay=%s\n", k->rpi_overlays.v[i]);
	addf(&text, "include usercfg.txt\n");
	int rc = write_ours(path, text);
	free(text);
	free(path);
	return rc;
}

/* ---- Android boot images ---- */

/* PATH gzip-ed (gzip -n -9 on the PATH of the host: the root can be another architecture) */
static char *gzip_file(const char *path, size_t *len)
{
	char *tmp = xasprintf("%s" STATE_DIR "/android/.kernel-XXXXXX", root);
	int fd = mkostemp(tmp, O_CLOEXEC);
	if (fd < 0) {
		warn_errno("%s", tmp);
		free(tmp);
		return NULL;
	}
	pid_t pid = fork();
	if (pid == 0) {
		dup2(fd, 1);
		execlp("gzip", "gzip", "-n", "-9", "-c", path, (char *)NULL);
		_exit(127);
	}
	close(fd);
	int st = -1;
	while (pid > 0 && waitpid(pid, &st, 0) < 0 && errno == EINTR)
		;
	char *data = NULL;
	if (pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0)
		data = read_file(tmp, len);
	else
		warnx("android: gzip %s failed (is gzip installed? android.compress=none takes the kernel as it is)", path);
	unlink(tmp);
	free(tmp);
	return data;
}

static int android_image(const bootconf *bc, const char *kver, const char *cmdline, const char *initrd,
			 const char *out)
{
	char *raw = raw_image(bc, kver), *err = NULL;
	size_t klen = 0, rlen = 0, dlen = 0;
	char *kernel = read_file(raw, &klen);
	if (!kernel) {
		warn_errno("android: %s", raw);
		free(raw);
		return 1;
	}
	if (!strcmp(bc->compress, "gzip") && !(klen >= 2 && (unsigned char)kernel[0] == 0x1f && (unsigned char)kernel[1] == 0x8b)) {
		free(kernel);
		kernel = gzip_file(raw, &klen);
		if (!kernel) {
			free(raw);
			return 1;
		}
	}
	free(raw);
	char *dtb = NULL;
	const char *dt = bc->devicetree;
	if (dt && *dt) {
		if (is_dir_path(dt)) {
			warnx("android: devicetree=%s: a boot image takes one device tree file", dt);
		} else {
			char *s = dt_source(kver, dt);
			dtb = read_file(s, &dlen);
			if (!dtb)
				warn_errno("android: %s", s);
			free(s);
		}
	}
	/* header 0 and 1: the device tree appended to the kernel (Image.gz-dtb) */
	if (dtb && bc->header < 2) {
		kernel = xrealloc(kernel, klen + dlen + 1);
		memcpy(kernel + klen, dtb, dlen);
		klen += dlen;
	}
	char *ramdisk = initrd ? read_file(initrd, &rlen) : NULL;
	bootimg bi = {.header_version = bc->header, .page_size = bc->pagesize, .base = bc->base,
		      .kernel_offset = bc->kernel_offset, .ramdisk_offset = bc->ramdisk_offset,
		      .tags_offset = bc->tags_offset, .dtb_offset = bc->dtb_offset, .board = bc->board,
		      .cmdline = cmdline, .kernel = kernel, .kernel_len = klen, .ramdisk = ramdisk,
		      .ramdisk_len = ramdisk ? rlen : 0, .dtb = bc->header >= 2 ? dtb : NULL,
		      .dtb_len = bc->header >= 2 && dtb ? dlen : 0};
	int rc = 1;
	size_t len = 0;
	void *img = NULL;
	if (bootimg_os_version(bc->os_version, bc->os_patch_level, &bi.os_version, &err) == 0)
		img = bootimg_build(&bi, &len, &err);
	if (!img) {
		warnx("android: kernel %s: %s", kver, err);
		free(err);
	} else {
		size_t old_len;
		char *old = read_file(out, &old_len);
		bool same = old && old_len == len && !memcmp(old, img, len);
		free(old);
		rc = 0;
		if (!same) {
			msg("%s %s", dry_run ? "would write" : "writing", out);
			if (!dry_run && write_file(out, img, len, 0600) < 0) {
				warn_errno("cannot write %s", out);
				rc = 1;
			}
		}
	}
	free(img);
	free(kernel);
	free(ramdisk);
	free(dtb);
	return rc;
}

/* the images of the installed kernels in /var/lib/cleanlinux/android; the default one flashed
 * into android.partition on the running system (with --root only on --flash); images of kernels
 * that are gone removed, the backup (*.orig) of the partition kept */
static int sync_android(const bootconf *bc, const kinfo *ks, char *const *initrds, size_t n, size_t def)
{
	int rc = 0;
	char *dir = xasprintf("%s" STATE_DIR "/android", root);
	if (bc->loaders & L_ANDROID) {
		if (!dry_run)
			mkdir_p(dir, 0700);
		for (size_t i = 0; i < n; i++) {
			char *out = xasprintf("%s/%s.img", dir, ks[i].kver);
			rc |= android_image(bc, ks[i].kver, ks[i].cmdline, initrds[i], out);
			free(out);
		}
		if (bc->partition && *bc->partition && n && (!*root || flash)) {
			char *img_path = xasprintf("%s/%s.img", dir, ks[def].kver);
			size_t len;
			char *img = read_file(img_path, &len);
			if (img) {
				char *backup = xasprintf("%s/%s.orig", dir, base_name(bc->partition));
				if (bootimg_flash(img, len, bc->partition, backup, dry_run) < 0)
					rc = 1;
				free(backup);
			} else if (!dry_run) {
				warn_errno("android: %s", img_path);
				rc = 1;
			}
			free(img);
			free(img_path);
		} else if (bc->partition && *bc->partition && n) {
			msg("android: %s/%s.img is not flashed into %s with --root (--flash does)", dir, ks[def].kver,
			    bc->partition);
		}
	}
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (!str_suffix(e->d_name, ".img"))
			continue;
		bool wanted = false;
		for (size_t i = 0; i < n && (bc->loaders & L_ANDROID); i++) {
			char *name = xasprintf("%s.img", ks[i].kver);
			wanted |= !strcmp(name, e->d_name);
			free(name);
		}
		if (!wanted) {
			char *p = xasprintf("%s/%s", dir, e->d_name);
			msg("%s %s", dry_run ? "would remove" : "removing", p);
			if (!dry_run)
				unlink(p);
			free(p);
		}
	}
	if (d)
		closedir(d);
	free(dir);
	return rc;
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

#define USAGE "usage: kernel sync|list|config [--esp DIR] [--dry-run] [--flash]"

/* kernel sync|list|config [--esp DIR] [--dry-run] [--flash] */
int cmd_kernel(int argc, char **argv)
{
	const char *sub = argc > 1 ? argv[1] : "sync";
	char *esp_owned = NULL;
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--esp") && i + 1 < argc)
			esp = argv[++i];
		else if (!strcmp(argv[i], "--dry-run"))
			dry_run = true;
		else if (!strcmp(argv[i], "--flash"))
			flash = true;
		else
			die(USAGE);
	}
	strv kernels = STRV_INIT;
	installed_kernels(&kernels);
	bootconf bc;
	bootconf_load(&bc);
	/* the default kernel: default= when it is installed, else the newest */
	size_t def = 0;
	if (bc.def && *bc.def && strcmp(bc.def, "newest")) {
		while (def < kernels.n && strcmp(kernels.v[def], bc.def))
			def++;
		if (def == kernels.n) {
			if (!strcmp(sub, "sync") && kernels.n)
				warnx("default=%s is not installed: the newest kernel is the default", bc.def);
			def = 0;
		}
	}
	int rc = 0;
	if (!strcmp(sub, "list")) {
		for (size_t i = 0; i < kernels.n; i++) {
			char *opts = kernel_cmdline(kernels.v[i]);
			printf("%s\t%s\n", kernels.v[i], opts);
			free(opts);
		}
		goto out;
	}
	if (!strcmp(sub, "config")) {
		bootconf_print(&bc, kernels.n ? kernels.v[def] : NULL);
		goto out;
	}
	if (strcmp(sub, "sync"))
		die(USAGE);
	if (!esp) {
		const char *env = getenv("CLEANLINUX_ESP");
		if (env && *env)
			esp = env;
		else
			esp = esp_owned = find_esp();
	}
	const unsigned esp_loaders = L_SDBOOT | L_EXTLINUX | L_RPI;
	if (!(bc.loaders & esp_loaders))
		esp = NULL; /* the boot partition is not touched */
	else if (!esp && !(bc.loaders & L_ANDROID))
		msg("no boot partition (vfat) mounted at /efi, /boot or /boot/efi: nothing to do");
	if (!esp && !(bc.loaders & L_ANDROID))
		goto out;
	kinfo *ks = calloc(kernels.n ? kernels.n : 1, sizeof(*ks));
	char **initrds = calloc(kernels.n ? kernels.n : 1, sizeof(*initrds));
	if (!ks || !initrds)
		die("out of memory");
	for (size_t i = 0; i < kernels.n; i++) {
		ks[i].kver = kernels.v[i];
		ks[i].cmdline = kernel_cmdline(kernels.v[i]);
		if (!*ks[i].cmdline)
			warnx("kernel %s: empty command line (no /etc/kernel/cmdline), the kernel will not find its root",
			      kernels.v[i]);
		initrds[i] = prepare_initramfs(kernels.v[i], &rc);
	}
	if (esp) {
		char *title = os_name();
		for (size_t i = 0; i < kernels.n; i++)
			if (sync_kernel(&bc, &ks[i], initrds[i], title))
				rc = 1;
		remove_stale(&kernels, bc.loaders & L_SDBOOT);
		if (sync_extlinux(&bc, ks, kernels.n, def, title))
			rc = 1;
		if (sync_rpi(&bc, kernels.n ? &ks[def] : NULL))
			rc = 1;
		if ((bc.loaders & L_SDBOOT) && sync_loader())
			rc = 1;
		free(title);
	}
	if (sync_android(&bc, ks, initrds, kernels.n, def))
		rc = 1;
	if (!dry_run)
		sync();
	for (size_t i = 0; i < kernels.n; i++) {
		kinfo_free(&ks[i]);
		free(initrds[i]);
	}
	free(ks);
	free(initrds);
out:
	bootconf_free(&bc);
	strv_free(&kernels);
	free(esp_owned);
	return rc;
}
