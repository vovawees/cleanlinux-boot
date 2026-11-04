/* SPDX-License-Identifier: MIT */
/* bootimg: Android boot images for the boot loaders of phones and handhelds (Qualcomm ABL and the
 * like), the layout of AOSP mkbootimg with header versions 0, 1 and 2:
 *
 *   header (1632, 1648 or 1660 bytes)  kernel  ramdisk  [v2: dtb]     each padded to the page size
 *
 * The id of the header is the SHA-1 of every section followed by its size (32-bit little endian),
 * as mkbootimg makes it; the command line takes 512 bytes and continues in the 1024 extra ones.
 * Flashing writes the image to the boot partition only when that holds an Android boot image (or
 * nothing yet), it is big enough, and the image differs; the first flash keeps a copy of what the
 * partition held. */
#define _GNU_SOURCE
#include "boot.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define BOOT_MAGIC "ANDROID!"
#define BOOT_NAME_SIZE 16
#define BOOT_ARGS_SIZE 512
#define BOOT_EXTRA_ARGS_SIZE 1024

/* ---- SHA-1 (FIPS 180-4), for the id of the header ---- */

typedef struct {
	uint32_t h[5];
	uint64_t len;
	uint8_t buf[64];
	size_t n;
} sha1_ctx;

static uint32_t rol(uint32_t x, int n)
{
	return x << n | x >> (32 - n);
}

static void sha1_block(sha1_ctx *c, const uint8_t *p)
{
	uint32_t w[80];
	for (int i = 0; i < 16; i++)
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
	for (int i = 16; i < 80; i++)
		w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	uint32_t a = c->h[0], b = c->h[1], d = c->h[3], e = c->h[4], cc = c->h[2];
	for (int i = 0; i < 80; i++) {
		uint32_t f, k;
		if (i < 20) {
			f = (b & cc) | (~b & d);
			k = 0x5a827999;
		} else if (i < 40) {
			f = b ^ cc ^ d;
			k = 0x6ed9eba1;
		} else if (i < 60) {
			f = (b & cc) | (b & d) | (cc & d);
			k = 0x8f1bbcdc;
		} else {
			f = b ^ cc ^ d;
			k = 0xca62c1d6;
		}
		uint32_t t = rol(a, 5) + f + e + k + w[i];
		e = d;
		d = cc;
		cc = rol(b, 30);
		b = a;
		a = t;
	}
	c->h[0] += a;
	c->h[1] += b;
	c->h[2] += cc;
	c->h[3] += d;
	c->h[4] += e;
}

static void sha1_init(sha1_ctx *c)
{
	static const uint32_t h0[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
	memcpy(c->h, h0, sizeof h0);
	c->len = 0;
	c->n = 0;
}

static void sha1_update(sha1_ctx *c, const void *data, size_t len)
{
	const uint8_t *p = data;
	c->len += len;
	while (len) {
		size_t take = 64 - c->n < len ? 64 - c->n : len;
		memcpy(c->buf + c->n, p, take);
		c->n += take;
		p += take;
		len -= take;
		if (c->n == 64) {
			sha1_block(c, c->buf);
			c->n = 0;
		}
	}
}

static void sha1_final(sha1_ctx *c, uint8_t out[20])
{
	uint64_t bits = c->len * 8;
	uint8_t pad = 0x80, zero = 0, len[8];
	sha1_update(c, &pad, 1);
	while (c->n != 56)
		sha1_update(c, &zero, 1);
	for (int i = 0; i < 8; i++)
		len[i] = (uint8_t)(bits >> (56 - 8 * i));
	sha1_update(c, len, 8);
	for (int i = 0; i < 5; i++) {
		out[4 * i] = (uint8_t)(c->h[i] >> 24);
		out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
		out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
		out[4 * i + 3] = (uint8_t)c->h[i];
	}
}

/* ---- the image ---- */

static void put32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

static void put64(uint8_t *p, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

static void sha1_section(sha1_ctx *c, const void *data, size_t len)
{
	uint8_t n[4];
	if (len)
		sha1_update(c, data, len);
	put32(n, (uint32_t)len);
	sha1_update(c, n, 4);
}

static size_t pages(size_t len, size_t page)
{
	return (len + page - 1) / page * page;
}

int bootimg_os_version(const char *version, const char *patch_level, uint32_t *out, char **err)
{
	unsigned a = 0, b = 0, c = 0, y = 0, m = 0;
	uint32_t v = 0, p = 0;
	if (version && *version) {
		int n = sscanf(version, "%u.%u.%u", &a, &b, &c);
		if (n < 1 || a > 127 || b > 127 || c > 127) {
			*err = xasprintf("android.os_version=%s: A.B.C, each below 128", version);
			return -1;
		}
		v = a << 14 | b << 7 | c;
	}
	if (patch_level && *patch_level) {
		if (sscanf(patch_level, "%u-%u", &y, &m) != 2 || y < 2000 || y > 2127 || m < 1 || m > 12) {
			*err = xasprintf("android.os_patch_level=%s: YYYY-MM", patch_level);
			return -1;
		}
		p = (y - 2000) << 4 | m;
	}
	*out = v << 11 | p;
	return 0;
}

void *bootimg_build(const bootimg *bi, size_t *out_len, char **err)
{
	unsigned v = bi->header_version;
	size_t ps = bi->page_size;
	if (v > 2) {
		*err = xasprintf("android.header=%u: 0, 1 or 2", v);
		return NULL;
	}
	if (ps < 2048 || ps > 16384 || (ps & (ps - 1))) {
		*err = xasprintf("android.pagesize=%zu: 2048, 4096, 8192 or 16384", ps);
		return NULL;
	}
	if (!bi->kernel_len) {
		*err = xstrdup("no kernel");
		return NULL;
	}
	if (v == 2 && !bi->dtb_len) {
		*err = xstrdup("header version 2 carries the device tree: set devicetree= (or android.header=0)");
		return NULL;
	}
	const char *cmd = bi->cmdline ? bi->cmdline : "";
	size_t cmd_len = strlen(cmd);
	if (cmd_len > BOOT_ARGS_SIZE - 1 + BOOT_EXTRA_ARGS_SIZE - 1) {
		*err = xasprintf("the command line has %zu bytes, a boot image holds %d", cmd_len,
				 BOOT_ARGS_SIZE - 1 + BOOT_EXTRA_ARGS_SIZE - 1);
		return NULL;
	}
	if (bi->kernel_len > UINT32_MAX || bi->ramdisk_len > UINT32_MAX || bi->dtb_len > UINT32_MAX) {
		*err = xstrdup("a section is larger than 4 GiB");
		return NULL;
	}
	size_t hsize = v == 0 ? 1632 : v == 1 ? 1648 : 1660;
	size_t dtb_len = v == 2 ? bi->dtb_len : 0;
	size_t total = pages(hsize, ps) + pages(bi->kernel_len, ps) + pages(bi->ramdisk_len, ps) + pages(dtb_len, ps);
	uint8_t *img = calloc(1, total);
	if (!img)
		die("out of memory");
	uint32_t base = bi->base;
	memcpy(img, BOOT_MAGIC, 8);
	put32(img + 8, (uint32_t)bi->kernel_len);
	put32(img + 12, base + bi->kernel_offset);
	put32(img + 16, (uint32_t)bi->ramdisk_len);
	put32(img + 20, bi->ramdisk_len ? base + bi->ramdisk_offset : 0);
	put32(img + 24, 0); /* no second stage */
	put32(img + 28, 0);
	put32(img + 32, base + bi->tags_offset);
	put32(img + 36, (uint32_t)ps);
	put32(img + 40, v);
	put32(img + 44, bi->os_version);
	if (bi->board)
		memcpy(img + 48, bi->board, strnlen(bi->board, BOOT_NAME_SIZE - 1));
	size_t first = cmd_len < BOOT_ARGS_SIZE - 1 ? cmd_len : BOOT_ARGS_SIZE - 1;
	memcpy(img + 64, cmd, first);
	memcpy(img + 64 + BOOT_ARGS_SIZE + 32, cmd + first, cmd_len - first);
	sha1_ctx c;
	sha1_init(&c);
	sha1_section(&c, bi->kernel, bi->kernel_len);
	sha1_section(&c, bi->ramdisk, bi->ramdisk_len);
	sha1_section(&c, NULL, 0); /* second */
	if (v > 0)
		sha1_section(&c, NULL, 0); /* recovery dtbo */
	if (v > 1)
		sha1_section(&c, bi->dtb, dtb_len);
	sha1_final(&c, img + 64 + BOOT_ARGS_SIZE);
	if (v > 0) {
		put32(img + 1632, 0); /* recovery dtbo: size, offset */
		put64(img + 1636, 0);
		put32(img + 1644, (uint32_t)hsize);
	}
	if (v > 1) {
		put32(img + 1648, (uint32_t)dtb_len);
		put64(img + 1652, (uint64_t)base + bi->dtb_offset);
	}
	uint8_t *p = img + pages(hsize, ps);
	memcpy(p, bi->kernel, bi->kernel_len);
	p += pages(bi->kernel_len, ps);
	if (bi->ramdisk_len)
		memcpy(p, bi->ramdisk, bi->ramdisk_len);
	p += pages(bi->ramdisk_len, ps);
	if (dtb_len)
		memcpy(p, bi->dtb, dtb_len);
	*out_len = total;
	return img;
}

/* ---- flashing ---- */

static int read_full(int fd, void *buf, size_t len, off_t off)
{
	char *p = buf;
	while (len) {
		ssize_t r = pread(fd, p, len, off);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -1;
		p += r;
		off += r;
		len -= (size_t)r;
	}
	return 0;
}

static int write_full(int fd, const void *buf, size_t len, off_t off)
{
	const char *p = buf;
	while (len) {
		ssize_t w = pwrite(fd, p, len, off);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return -1;
		p += w;
		off += w;
		len -= (size_t)w;
	}
	return 0;
}

/* the whole partition into BACKUP, once */
static int save_backup(int fd, uint64_t size, const char *backup)
{
	char *tmp = xasprintf("%s.new", backup);
	int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	int rc = out < 0 ? -1 : 0;
	char *buf = xmalloc(1 << 20);
	for (uint64_t off = 0; rc == 0 && off < size;) {
		size_t n = size - off < (1 << 20) ? (size_t)(size - off) : (1 << 20);
		if (read_full(fd, buf, n, (off_t)off) < 0 || write_full(out, buf, n, (off_t)off) < 0)
			rc = -1;
		off += n;
	}
	free(buf);
	if (out >= 0 && (fsync(out) < 0 || close(out) < 0))
		rc = -1;
	if (rc == 0 && rename(tmp, backup) < 0)
		rc = -1;
	if (rc < 0)
		unlink(tmp);
	free(tmp);
	return rc;
}

int bootimg_flash(const void *img, size_t len, const char *dev, const char *backup, bool dry_run)
{
	int fd = open(dev, (dry_run ? O_RDONLY : O_RDWR) | O_CLOEXEC);
	if (fd < 0) {
		warn_errno("android: cannot open the boot partition %s", dev);
		return -1;
	}
	struct stat st;
	uint64_t size = 0;
	int rc = -1;
	char *cur = NULL;
	if (fstat(fd, &st) < 0) {
		warn_errno("android: %s", dev);
		goto out;
	}
	if (S_ISBLK(st.st_mode)) {
		if (ioctl(fd, BLKGETSIZE64, &size) < 0) {
			warn_errno("android: the size of %s", dev);
			goto out;
		}
	} else if (S_ISREG(st.st_mode)) {
		size = (uint64_t)st.st_size;
	} else {
		warnx("android: %s is no block device", dev);
		goto out;
	}
	if (size < len) {
		warnx("android: %s has %llu bytes, the boot image %zu: not flashed", dev, (unsigned long long)size, len);
		goto out;
	}
	cur = xmalloc(len);
	if (read_full(fd, cur, len, 0) < 0) {
		warn_errno("android: cannot read %s", dev);
		goto out;
	}
	if (!memcmp(cur, img, len)) {
		rc = 0; /* flashed already */
		goto out;
	}
	bool blank = true;
	for (size_t i = 0; i < len && i < 4096; i++)
		if (cur[i]) {
			blank = false;
			break;
		}
	if (!blank && memcmp(cur, BOOT_MAGIC, 8)) {
		warnx("android: %s holds no Android boot image (no ANDROID! at its start): not flashed, check android.partition", dev);
		goto out;
	}
	msg("%s %s", dry_run ? "would flash" : "flashing", dev);
	if (dry_run) {
		rc = 0;
		goto out;
	}
	if (backup && !exists(backup)) {
		if (save_backup(fd, size, backup) < 0) {
			warn_errno("android: cannot save %s into %s: not flashed", dev, backup);
			goto out;
		}
		msg("the former content of %s: %s", dev, backup);
	}
	if (write_full(fd, img, len, 0) < 0 || fsync(fd) < 0) {
		warn_errno("android: cannot write %s", dev);
		goto out;
	}
	rc = 0;
out:
	free(cur);
	close(fd);
	return rc;
}
