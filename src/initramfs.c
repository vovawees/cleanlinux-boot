/* SPDX-License-Identifier: MIT */
/* initramfs: a small initramfs for the systems that cannot boot without one (an encrypted
 * root, storage drivers built as modules). Everything else boots the kernel straight from
 * root=PARTUUID=, with the drivers of the root built in.
 *
 * Configuration: *.conf of cleanlinux/initramfs.d in /etc, /run and /usr/lib (profiles),
 * lines of KEY=VALUE, the lists add up:
 *   modules=nvme dm-crypt     modules (or aliases), with their dependencies and firmware
 *   files=/usr/bin/cryptsetup programs and files, ELF programs with their libraries
 *   compress=zstd|none
 * Without any modules= or files= no initramfs is wanted ("enable=yes" makes an empty one).
 *
 * The image: cleanlinux-init as /init, the libraries it needs, the modules with an index of
 * their own (depmod -b), /etc/initramfs/modules, /dev/console — a cpio (newc), sorted,
 * owned by root and dated SOURCE_DATE_EPOCH, so the same input gives the same bytes. */
#define _GNU_SOURCE
#include "boot.h"

#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <libkmod.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <zstd.h>

struct config {
	strv modules, files;
	bool enable, compress;
};

static void split_add(strv *out, const char *list)
{
	char *s = xstrdup(list);
	for (char *save = NULL, *w = strtok_r(s, " \t,", &save); w; w = strtok_r(NULL, " \t,", &save))
		if (!strv_contains(out, w))
			strv_push(out, w);
	free(s);
}

static void read_config(struct config *c)
{
	c->compress = true;
	strv files = STRV_INIT;
	conf_files("cleanlinux/initramfs.d", ".conf", &files);
	for (size_t i = 0; i < files.n; i++) {
		char *s = read_file(files.v[i], NULL);
		if (!s)
			continue;
		for (char *save = NULL, *l = strtok_r(s, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
			l = trim(l);
			if (!*l || *l == '#')
				continue;
			char *eq = strchr(l, '=');
			if (!eq) {
				warnx("%s: not KEY=VALUE: %s", files.v[i], l);
				continue;
			}
			*eq = '\0';
			char *k = trim(l), *v = trim(eq + 1);
			size_t n = strlen(v);
			if (n >= 2 && (*v == '"' || *v == '\'') && v[n - 1] == *v) {
				v[n - 1] = '\0';
				v++;
			}
			if (!strcmp(k, "modules"))
				split_add(&c->modules, v);
			else if (!strcmp(k, "files"))
				split_add(&c->files, v);
			else if (!strcmp(k, "enable"))
				c->enable = !strcmp(v, "yes") || !strcmp(v, "1");
			else if (!strcmp(k, "compress"))
				c->compress = strcmp(v, "none") != 0;
			else
				warnx("%s: unknown key %s", files.v[i], k);
		}
		free(s);
	}
	strv_free(&files);
}

bool initramfs_wanted(void)
{
	struct config c = {0};
	read_config(&c);
	bool w = c.enable || c.modules.n || c.files.n;
	strv_free(&c.modules);
	strv_free(&c.files);
	return w;
}

/* ---- the staging tree ---- */

static char *stage;

static int copy_file(const char *src, const char *dst, mode_t mode)
{
	size_t len;
	char *data = read_file(src, &len);
	if (!data)
		return -1;
	char *dir = xstrdup(dst);
	*strrchr(dir, '/') = '\0';
	mkdir_p(dir, 0755);
	free(dir);
	int rc = write_file(dst, data, len, mode);
	free(data);
	return rc;
}

/* ELF: the DT_NEEDED names and the interpreter of a 64-bit little-endian file */
static bool elf_deps(const char *path, strv *needed, char **interp)
{
	size_t len;
	unsigned char *f = (unsigned char *)read_file(path, &len);
	if (!f)
		return false;
	bool ok = false;
	if (len < sizeof(Elf64_Ehdr) || memcmp(f, ELFMAG, SELFMAG) || f[EI_CLASS] != ELFCLASS64 || f[EI_DATA] != ELFDATA2LSB)
		goto out;
	ok = true;
	const Elf64_Ehdr *eh = (const Elf64_Ehdr *)f;
	if (eh->e_phoff + (size_t)eh->e_phnum * sizeof(Elf64_Phdr) > len)
		goto out;
	const Elf64_Phdr *ph = (const Elf64_Phdr *)(f + eh->e_phoff);
	const Elf64_Dyn *dyn = NULL;
	size_t ndyn = 0;
	for (int i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type == PT_INTERP && interp && ph[i].p_offset + ph[i].p_filesz <= len)
			*interp = xstrndup((const char *)f + ph[i].p_offset, ph[i].p_filesz);
		if (ph[i].p_type == PT_DYNAMIC && ph[i].p_offset + ph[i].p_filesz <= len) {
			dyn = (const Elf64_Dyn *)(f + ph[i].p_offset);
			ndyn = ph[i].p_filesz / sizeof(Elf64_Dyn);
		}
	}
	if (!dyn)
		goto out;
	/* DT_STRTAB is an address: the file offset through the PT_LOAD that maps it */
	Elf64_Addr strtab = 0;
	for (size_t i = 0; i < ndyn && dyn[i].d_tag != DT_NULL; i++)
		if (dyn[i].d_tag == DT_STRTAB)
			strtab = dyn[i].d_un.d_ptr;
	size_t stroff = 0;
	for (int i = 0; i < eh->e_phnum; i++)
		if (ph[i].p_type == PT_LOAD && strtab >= ph[i].p_vaddr && strtab < ph[i].p_vaddr + ph[i].p_filesz)
			stroff = strtab - ph[i].p_vaddr + ph[i].p_offset;
	if (!stroff)
		goto out;
	for (size_t i = 0; i < ndyn && dyn[i].d_tag != DT_NULL; i++)
		if (dyn[i].d_tag == DT_NEEDED && stroff + dyn[i].d_un.d_val < len)
			strv_push(needed, (const char *)f + stroff + dyn[i].d_un.d_val);
out:
	free(f);
	return ok;
}

/* PATH of the root (a program, a library, a file) into the stage, and its libraries */
static int add_file(const char *path, strv *done)
{
	if (strv_contains(done, path))
		return 0;
	strv_push(done, path);
	char *src = rpath(path), *dst = xasprintf("%s%s", stage, path);
	struct stat st;
	int rc = 0;
	if (stat(src, &st) < 0 || copy_file(src, dst, st.st_mode & 07777) < 0) {
		warn_errno("initramfs: %s", path);
		rc = -1;
		goto out;
	}
	strv needed = STRV_INIT;
	char *interp = NULL;
	if (elf_deps(src, &needed, &interp)) {
		if (interp && add_file(interp, done) < 0)
			rc = -1;
		for (size_t i = 0; i < needed.n; i++) {
			char *lib = xasprintf("/usr/lib/%s", needed.v[i]);
			if (add_file(lib, done) < 0)
				rc = -1;
			free(lib);
		}
	}
	free(interp);
	strv_free(&needed);
out:
	free(src);
	free(dst);
	return rc;
}

static void add_firmware(struct kmod_module *mod, strv *done)
{
	struct kmod_list *info = NULL, *l;
	if (kmod_module_get_info(mod, &info) < 0)
		return;
	kmod_list_foreach(l, info)
	{
		if (strcmp(kmod_module_info_get_key(l), "firmware"))
			continue;
		const char *fw = kmod_module_info_get_value(l);
		static const char *const exts[] = {"", ".zst", ".xz"};
		for (size_t e = 0; e < ARRAY_LEN(exts); e++) {
			char *p = xasprintf("/usr/lib/firmware/%s%s", fw, exts[e]);
			char *rp = rpath(p);
			bool have = is_file(rp);
			free(rp);
			if (have)
				add_file(p, done);
			free(p);
			if (have)
				break;
		}
	}
	kmod_module_info_free_list(info);
}

static int add_module(struct kmod_ctx *ctx, struct kmod_module *mod, const char *moddir, strv *done, int depth)
{
	if (depth > 32)
		return 0;
	const char *path = kmod_module_get_path(mod);
	if (!path)
		return 0; /* built in */
	if (!str_prefix(path, moddir)) {
		warnx("initramfs: module %s is outside %s", path, moddir);
		return -1;
	}
	const char *rel = path + strlen(root);
	if (strv_contains(done, rel))
		return 0;
	add_file(rel, done);
	add_firmware(mod, done);
	int rc = 0;
	struct kmod_list *deps = kmod_module_get_dependencies(mod), *l;
	kmod_list_foreach(l, deps)
	{
		struct kmod_module *d = kmod_module_get_module(l);
		if (add_module(ctx, d, moddir, done, depth + 1) < 0)
			rc = -1;
		kmod_module_unref(d);
	}
	kmod_module_unref_list(deps);
	struct kmod_list *pre = NULL, *post = NULL;
	if (kmod_module_get_softdeps(mod, &pre, &post) == 0) {
		struct kmod_list *lists[] = {pre, post};
		for (size_t i = 0; i < 2; i++) {
			kmod_list_foreach(l, lists[i])
			{
				struct kmod_module *d = kmod_module_get_module(l);
				add_module(ctx, d, moddir, done, depth + 1);
				kmod_module_unref(d);
			}
			kmod_module_unref_list(lists[i]);
		}
	}
	return rc;
}

static int add_modules(const char *kver, const strv *names, strv *done)
{
	char *moddir = xasprintf("%s/usr/lib/modules/%s", root, kver);
	struct kmod_ctx *ctx = kmod_new(moddir, NULL);
	if (!ctx) {
		free(moddir);
		return -1;
	}
	int rc = 0;
	for (size_t i = 0; i < names->n; i++) {
		struct kmod_list *list = NULL, *l;
		if (kmod_module_new_from_lookup(ctx, names->v[i], &list) < 0 || !list) {
			/* built in, or a name this kernel does not have */
			char *b = xasprintf("%s/modules.builtin", moddir);
			char *s = read_file(b, NULL);
			bool builtin = false;
			if (s) {
				char *n = xstrdup(names->v[i]);
				for (char *c = n; *c; c++)
					if (*c == '-')
						*c = '_';
				char *pat = xasprintf("/%s.ko\n", n);
				for (char *c = s; *c; c++)
					if (*c == '-')
						*c = '_';
				builtin = strstr(s, pat) != NULL;
				free(pat);
				free(n);
			}
			free(s);
			free(b);
			if (!builtin) {
				warnx("initramfs: kernel %s has no module %s", kver, names->v[i]);
				rc = -1;
			}
			continue;
		}
		kmod_list_foreach(l, list)
		{
			struct kmod_module *m = kmod_module_get_module(l);
			if (add_module(ctx, m, moddir, done, 0) < 0)
				rc = -1;
			kmod_module_unref(m);
		}
		kmod_module_unref_list(list);
	}
	kmod_unref(ctx);
	/* the files depmod reads, then an index of exactly these modules */
	static const char *const keep[] = {"modules.builtin", "modules.builtin.modinfo", "modules.order"};
	for (size_t i = 0; i < ARRAY_LEN(keep); i++) {
		char *src = xasprintf("%s/%s", moddir, keep[i]);
		char *dst = xasprintf("%s/usr/lib/modules/%s/%s", stage, kver, keep[i]);
		if (is_file(src))
			copy_file(src, dst, 0644);
		free(src);
		free(dst);
	}
	char *depmod = find_program("depmod");
	if (!depmod || runl(depmod, "-a", "-b", stage, kver, (char *)NULL) != 0) {
		/* no depmod that runs here: the index of the whole kernel works as well */
		static const char *const idx[] = {"modules.dep", "modules.dep.bin", "modules.alias", "modules.alias.bin",
						  "modules.softdep", "modules.symbols.bin", "modules.builtin.bin",
						  "modules.builtin.alias.bin", "modules.devname"};
		for (size_t i = 0; i < ARRAY_LEN(idx); i++) {
			char *src = xasprintf("%s/%s", moddir, idx[i]);
			char *dst = xasprintf("%s/usr/lib/modules/%s/%s", stage, kver, idx[i]);
			if (is_file(src))
				copy_file(src, dst, 0644);
			free(src);
			free(dst);
		}
	}
	free(depmod);
	free(moddir);
	return rc;
}

/* ---- the cpio archive ---- */

struct out {
	FILE *f;
	ZSTD_CCtx *z;
	unsigned char buf[1 << 16];
	size_t n;
	unsigned long long size;
	bool error;
};

static void out_flush(struct out *o, bool end)
{
	ZSTD_inBuffer in = {o->buf, o->n, 0};
	unsigned char zb[1 << 16];
	if (!o->z) {
		if (o->n && fwrite(o->buf, 1, o->n, o->f) != o->n)
			o->error = true;
		o->n = 0;
		return;
	}
	for (;;) {
		ZSTD_outBuffer zo = {zb, sizeof(zb), 0};
		size_t left = ZSTD_compressStream2(o->z, &zo, &in, end ? ZSTD_e_end : ZSTD_e_continue);
		if (ZSTD_isError(left)) {
			warnx("zstd: %s", ZSTD_getErrorName(left));
			o->error = true;
			return;
		}
		if (zo.pos && fwrite(zb, 1, zo.pos, o->f) != zo.pos)
			o->error = true;
		if (end ? left == 0 : in.pos == in.size)
			break;
	}
	o->n = 0;
}

static void out_write(struct out *o, const void *data, size_t len)
{
	const unsigned char *p = data;
	o->size += len;
	while (len) {
		size_t k = sizeof(o->buf) - o->n < len ? sizeof(o->buf) - o->n : len;
		memcpy(o->buf + o->n, p, k);
		o->n += k;
		p += k;
		len -= k;
		if (o->n == sizeof(o->buf))
			out_flush(o, false);
	}
}

static void out_pad(struct out *o)
{
	static const char zeros[4];
	if (o->size % 4)
		out_write(o, zeros, 4 - o->size % 4);
}

static unsigned long ino_next = 1;

static void cpio_entry(struct out *o, const char *name, mode_t mode, unsigned rmaj, unsigned rmin, const void *data,
		       size_t len, unsigned long mtime)
{
	char hdr[160];
	size_t nl = strlen(name) + 1;
	snprintf(hdr, sizeof(hdr), "070701%08lX%08X%08X%08X%08X%08lX%08zX%08X%08X%08X%08X%08zX%08X", ino_next++,
		 (unsigned)mode, 0u, 0u, S_ISDIR(mode) ? 2u : 1u, mtime, len, 0u, 0u, rmaj, rmin, nl, 0u);
	if (strlen(hdr) != 110)
		die("initramfs: a cpio field overflows: %s", name);
	out_write(o, hdr, 110);
	out_write(o, name, nl);
	out_pad(o);
	if (len) {
		out_write(o, data, len);
		out_pad(o);
	}
}

static strv tree;

static int collect(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
	(void)st;
	(void)type;
	if (ftw->level > 0)
		strv_push(&tree, path + strlen(stage) + 1);
	return 0;
}

static int write_cpio(const char *out_path, bool compress)
{
	unsigned long mtime = 0;
	const char *sde = getenv("SOURCE_DATE_EPOCH");
	if (sde)
		mtime = strtoul(sde, NULL, 10);
	char *tmp = xasprintf("%s.tmp", out_path);
	struct out *o = calloc(1, sizeof(*o));
	if (!o)
		die("out of memory");
	o->f = fopen(tmp, "we");
	if (!o->f) {
		warn_errno("%s", tmp);
		free(tmp);
		free(o);
		return -1;
	}
	if (compress) {
		o->z = ZSTD_createCCtx();
		ZSTD_CCtx_setParameter(o->z, ZSTD_c_compressionLevel, 15);
		ZSTD_CCtx_setParameter(o->z, ZSTD_c_checksumFlag, 1);
	}
	nftw(stage, collect, 16, FTW_PHYS);
	strv_sort(&tree);
	/* device nodes need no privileges in a cpio: the kernel makes them */
	static const struct {
		const char *name;
		unsigned maj, min;
	} nodes[] = {{"dev/console", 5, 1}, {"dev/kmsg", 1, 11}, {"dev/null", 1, 3}};
	cpio_entry(o, "dev", S_IFDIR | 0755, 0, 0, NULL, 0, mtime);
	for (size_t i = 0; i < ARRAY_LEN(nodes); i++)
		cpio_entry(o, nodes[i].name, S_IFCHR | 0600, nodes[i].maj, nodes[i].min, NULL, 0, mtime);
	for (size_t i = 0; i < tree.n; i++) {
		if (!strcmp(tree.v[i], "dev"))
			continue;
		char *p = xasprintf("%s/%s", stage, tree.v[i]);
		struct stat st;
		if (lstat(p, &st) < 0) {
			free(p);
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			cpio_entry(o, tree.v[i], S_IFDIR | (st.st_mode & 07777), 0, 0, NULL, 0, mtime);
		} else if (S_ISLNK(st.st_mode)) {
			char target[4096];
			ssize_t n = readlink(p, target, sizeof(target));
			if (n > 0)
				cpio_entry(o, tree.v[i], S_IFLNK | 0777, 0, 0, target, (size_t)n, mtime);
		} else if (S_ISREG(st.st_mode)) {
			size_t len;
			char *data = read_file(p, &len);
			if (data)
				cpio_entry(o, tree.v[i], S_IFREG | (st.st_mode & 07777), 0, 0, data, len, mtime);
			free(data);
		}
		free(p);
	}
	cpio_entry(o, "TRAILER!!!", 0, 0, 0, NULL, 0, 0);
	out_flush(o, true);
	strv_free(&tree);
	if (o->z)
		ZSTD_freeCCtx(o->z);
	/* every step runs, even after an error: no short-circuit */
	bool err = o->error;
	err |= fflush(o->f) != 0;
	err |= fsync(fileno(o->f)) != 0;
	err |= fclose(o->f) != 0;
	free(o);
	if (err || rename(tmp, out_path) < 0) {
		warn_errno("cannot write %s", out_path);
		unlink(tmp);
		free(tmp);
		return -1;
	}
	chmod(out_path, 0600); /* an initramfs may hold keys */
	free(tmp);
	return 0;
}

static int rm_stage(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
	(void)st;
	(void)ftw;
	return type == FTW_DP ? rmdir(path) : unlink(path);
}

int initramfs_build(const char *kver, const char *out_path)
{
	struct config c = {0};
	read_config(&c);
	char tmpl[] = "/tmp/cleanlinux-initramfs.XXXXXX";
	stage = mkdtemp(tmpl);
	if (!stage)
		die("mkdtemp: %s", strerror(errno));
	int rc = 0;
	static const char *const dirs[] = {"/dev", "/proc", "/sys", "/run", "/new_root", "/etc/initramfs", "/usr/bin", "/usr/lib", "/tmp"};
	for (size_t i = 0; i < ARRAY_LEN(dirs); i++) {
		char *d = xasprintf("%s%s", stage, dirs[i]);
		mkdir_p(d, 0755);
		free(d);
	}
	static const char *const links[][2] = {{"usr/bin", "bin"}, {"usr/bin", "sbin"}, {"usr/lib", "lib"}, {"usr/lib", "lib64"}};
	for (size_t i = 0; i < ARRAY_LEN(links); i++) {
		char *l = xasprintf("%s/%s", stage, links[i][1]);
		if (symlink(links[i][0], l) < 0)
			rc = -1;
		free(l);
	}
	strv done = STRV_INIT;
	char *init = rpath(LIBEXECDIR "/init"), *dst = xasprintf("%s/init", stage);
	if (copy_file(init, dst, 0755) < 0) {
		warn_errno("%s", init);
		rc = -1;
	}
	free(init);
	free(dst);
	strv needed = STRV_INIT;
	char *interp = NULL, *init_src = rpath(LIBEXECDIR "/init");
	if (elf_deps(init_src, &needed, &interp)) {
		if (interp)
			add_file(interp, &done);
		for (size_t i = 0; i < needed.n; i++) {
			char *lib = xasprintf("/usr/lib/%s", needed.v[i]);
			if (add_file(lib, &done) < 0)
				rc = -1;
			free(lib);
		}
	}
	free(interp);
	free(init_src);
	strv_free(&needed);
	for (size_t i = 0; i < c.files.n; i++)
		if (add_file(c.files.v[i], &done) < 0)
			rc = -1;
	if (c.modules.n && add_modules(kver, &c.modules, &done) < 0)
		rc = -1;
	char *list = xstrdup("# modules loaded by cleanlinux-init before it looks for the root\n");
	for (size_t i = 0; i < c.modules.n; i++) {
		char *n = xasprintf("%s%s\n", list, c.modules.v[i]);
		free(list);
		list = n;
	}
	char *lp = xasprintf("%s/etc/initramfs/modules", stage);
	write_file(lp, list, strlen(list), 0644);
	free(lp);
	free(list);
	if (rc == 0)
		rc = write_cpio(out_path, c.compress);
	nftw(stage, rm_stage, 16, FTW_DEPTH | FTW_PHYS);
	strv_free(&done);
	strv_free(&c.modules);
	strv_free(&c.files);
	return rc;
}

/* initramfs [KVER] [-o FILE] */
int cmd_initramfs(int argc, char **argv)
{
	const char *kver = NULL, *out = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-o") && i + 1 < argc)
			out = argv[++i];
		else if (argv[i][0] != '-' && !kver)
			kver = argv[i];
		else
			die("usage: initramfs [KVER] [-o FILE]");
	}
	struct utsname u;
	if (!kver && uname(&u) == 0)
		kver = u.release;
	char *outp = out ? xstrdup(out) : xasprintf("%s/usr/lib/modules/%s/initramfs.img", root, kver);
	int rc = initramfs_build(kver, outp);
	if (rc == 0)
		msg("%s", outp);
	free(outp);
	return rc ? 1 : 0;
}
