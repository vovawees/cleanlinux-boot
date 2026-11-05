/* SPDX-License-Identifier: MIT */
/* cleanlinux-init: PID 1 of the initramfs that "cleanlinux-boot initramfs" makes.
 *
 * Only systems that cannot boot without one need it (an encrypted root, storage drivers that
 * are modules): it mounts the API filesystems, loads the modules listed in the initramfs and
 * the drivers of the devices it finds, opens LUKS (rd.luks.uuid=, with cryptsetup in the
 * initramfs), waits for root= (/dev/..., UUID=, LABEL=, PARTUUID=, PARTLABEL=), mounts it
 * with rootfstype=, rootflags=, ro/rw, and hands over to init= (default /sbin/init).
 * rd.break drops to a shell (when the initramfs has one) right before the switch.
 *
 * root=live:SPEC boots a live system instead (the CleanLinux ISO): the medium SPEC (LABEL= of the
 * ISO 9660 image on a USB stick or a CD, or any filesystem) holds a squashfs image of the root
 * (cleanlinux.live.image=, default /cleanlinux/live/rootfs.sfs); the root is an overlay of it with
 * a tmpfs on top, so the live system writes into memory. cleanlinux.live.toram copies the image
 * into memory first, and the medium can be removed. Mounts stay below /run/cleanlinux/live. */
#define _GNU_SOURCE
#include "boot.h"

#include <blkid.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <libkmod.h>
#include <linux/loop.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NEW_ROOT "/new_root"
#define MODULES_LIST "/etc/initramfs/modules"

static void console_stdio(void)
{
	int fd = open("/dev/console", O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd < 0)
		return;
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	if (fd > 2)
		close(fd);
}

static void sleep_ms(long ms)
{
	struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

static char *init_path;
static _Noreturn void switch_to_root(void);

/* a shell when the initramfs has one, otherwise wait forever: PID 1 must not exit */
static _Noreturn void emergency(const char *why)
{
	warnx("%s", why);
	static const char *const shells[] = {"/bin/sh", "/usr/bin/sh", "/bin/busybox"};
	for (size_t i = 0; i < ARRAY_LEN(shells); i++) {
		if (access(shells[i], X_OK) < 0)
			continue;
		msg("starting %s; mount the root on " NEW_ROOT " and exit it to continue the boot", shells[i]);
		pid_t pid = fork();
		if (pid == 0) {
			setsid();
			console_stdio();
			ioctl(0, TIOCSCTTY, 1);
			execl(shells[i], "sh", (char *)NULL);
			_exit(127);
		}
		if (pid > 0) {
			int st;
			while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
				;
		}
		break;
	}
	if (is_mountpoint(NEW_ROOT))
		switch_to_root(); /* the root was mounted by hand */
	msg("cannot continue the boot; press Ctrl+Alt+Del or reset the machine");
	for (;;)
		pause();
}

/* ---- the root device ---- */

/* the devices of /sys/class/block whose TAG (UUID, LABEL, PARTUUID, PARTLABEL, TYPE) is VALUE,
 * into OUT; with FIRST only the first one */
static void scan_by_tag(const char *tag, const char *value, strv *out, bool first)
{
	DIR *d = opendir("/sys/class/block");
	if (!d)
		return;
	struct dirent *e;
	while ((!first || !out->n) && (e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char *dev = xasprintf("/dev/%s", e->d_name);
		blkid_probe pr = blkid_new_probe_from_filename(dev);
		if (pr) {
			blkid_probe_enable_superblocks(pr, 1);
			blkid_probe_set_superblocks_flags(pr, BLKID_SUBLKS_LABEL | BLKID_SUBLKS_UUID | BLKID_SUBLKS_TYPE);
			blkid_probe_enable_partitions(pr, 1);
			blkid_probe_set_partitions_flags(pr, BLKID_PARTS_ENTRY_DETAILS);
			if (blkid_do_safeprobe(pr) == 0) {
				const char *key = !strcmp(tag, "PARTUUID") ? "PART_ENTRY_UUID"
						  : !strcmp(tag, "PARTLABEL") ? "PART_ENTRY_NAME"
									      : tag;
				const char *v = NULL;
				if (blkid_probe_lookup_value(pr, key, &v, NULL) == 0 && v && !strcasecmp(v, value))
					strv_push(out, dev);
			}
			blkid_free_probe(pr);
		}
		free(dev);
	}
	closedir(d);
}

/* every device SPEC names (/dev/..., UUID=, LABEL=, PARTUUID=, PARTLABEL=) into OUT */
static void resolve_all(const char *spec, strv *out, bool first)
{
	static const char *const tags[] = {"UUID", "LABEL", "PARTUUID", "PARTLABEL"};
	for (size_t i = 0; i < ARRAY_LEN(tags); i++) {
		size_t n = strlen(tags[i]);
		if (!strncmp(spec, tags[i], n) && spec[n] == '=') {
			scan_by_tag(tags[i], spec + n + 1, out, first);
			return;
		}
	}
	if (str_prefix(spec, "/dev/") && exists(spec))
		strv_push(out, spec);
}

static char *resolve(const char *spec)
{
	strv devs = STRV_INIT;
	resolve_all(spec, &devs, true);
	char *dev = devs.n ? xstrdup(devs.v[0]) : NULL;
	strv_free(&devs);
	return dev;
}

static struct kmod_ctx *kctx;

/* SPEC as a device node, loading the drivers of new devices while waiting; NULL on timeout
 * (timeout_ms < 0: forever, as rootwait) */
static char *wait_for(const char *spec, long timeout_ms)
{
	long waited = 0;
	bool told = false;
	for (;;) {
		char *dev = resolve(spec);
		if (dev)
			return dev;
		if (timeout_ms >= 0 && waited >= timeout_ms)
			return NULL;
		if (!told && waited >= 2000) {
			msg("waiting for %s", spec);
			told = true;
		}
		if (kctx && waited % 1000 == 0)
			modules_coldplug(kctx); /* hubs and controllers bring new devices */
		sleep_ms(100);
		waited += 100;
	}
}

static long root_timeout(void)
{
	char *v = cmdline_value("rootwait");
	if (v) {
		long t = *v ? atol(v) * 1000 : -1;
		free(v);
		return t;
	}
	v = cmdline_value("rootdelay");
	long t = v ? atol(v) * 1000 : 30000;
	free(v);
	return t;
}

/* rd.luks.uuid=UUID: cryptsetup open UUID=... luks-UUID (it asks for the passphrase on the
 * console); the root is then root=/dev/mapper/luks-UUID or the UUID of what is inside */
static void open_luks(void)
{
	char *uuid = cmdline_value("rd.luks.uuid");
	if (!uuid || !*uuid) {
		free(uuid);
		return;
	}
	if (str_prefix(uuid, "luks-"))
		memmove(uuid, uuid + 5, strlen(uuid + 5) + 1);
	char *spec = xasprintf("UUID=%s", uuid), *name = xasprintf("luks-%s", uuid);
	char *mapped = xasprintf("/dev/mapper/%s", name);
	const char *cs = access("/usr/bin/cryptsetup", X_OK) == 0 ? "/usr/bin/cryptsetup" : NULL;
	if (exists(mapped)) {
		/* already open */
	} else if (!cs) {
		warnx("rd.luks.uuid=%s, but the initramfs has no cryptsetup (files=/usr/bin/cryptsetup)", uuid);
	} else {
		char *dev = wait_for(spec, root_timeout());
		if (!dev)
			emergency("the encrypted device did not appear");
		for (int tries = 0; tries < 3 && !exists(mapped); tries++)
			runl(cs, "open", "--type", "luks", dev, name, (char *)NULL);
		free(dev);
		if (!exists(mapped))
			emergency("could not open the encrypted device");
	}
	free(mapped);
	free(name);
	free(spec);
	free(uuid);
}

/* DEV on DIR as FSTYPE, or as each filesystem the kernel has (as the kernel does it for the root) */
static int mount_dev(const char *dev, const char *dir, const char *fstype, unsigned long flags, const char *data)
{
	if (fstype && *fstype)
		return mount(dev, dir, fstype, flags, data);
	int r = -1, err = ENODEV;
	char *fs = read_file("/proc/filesystems", NULL);
	for (char *save = NULL, *l = fs ? strtok_r(fs, "\n", &save) : NULL; l && r < 0; l = strtok_r(NULL, "\n", &save)) {
		if (str_prefix(l, "nodev"))
			continue;
		r = mount(dev, dir, trim(l), flags, data);
		if (r < 0 && errno != EINVAL)
			err = errno; /* the error of a filesystem that recognized it, not "not mine" */
	}
	free(fs);
	if (r < 0)
		errno = err;
	return r;
}

/* ---- the live system: root=live:SPEC ---- */

#define LIVE_DIR "/run/cleanlinux/live"
#define LIVE_IMAGE "/cleanlinux/live/rootfs.sfs"

/* the drivers of a live boot, when they are modules (built in: nothing to do) */
static void live_modules(void)
{
	static const char *const names[] = {"loop", "squashfs", "overlay", "isofs", "sr_mod", "uas", "usb-storage"};
	if (!kctx)
		return;
	strv v = STRV_INIT;
	for (size_t i = 0; i < ARRAY_LEN(names); i++)
		strv_push(&v, names[i]);
	modules_load_list(kctx, &v, true);
	strv_free(&v);
}

/* the medium: the first device SPEC names that has IMAGE, mounted read-only on DIR; NULL when
 * none appears in TIMEOUT_MS (< 0: forever). Devices come late (USB), so they are looked for
 * again and again, loading the drivers of new devices meanwhile */
static char *live_medium(const char *spec, const char *image, const char *dir, long timeout_ms)
{
	strv without = STRV_INIT; /* mounted, but without the image: not this one */
	long waited = 0;
	bool told = false;
	int err = 0;
	mkdir_p(dir, 0755);
	for (;;) {
		strv devs = STRV_INIT;
		resolve_all(spec, &devs, false);
		for (size_t i = 0; i < devs.n; i++) {
			if (strv_contains(&without, devs.v[i]))
				continue;
			if (mount_dev(devs.v[i], dir, NULL, MS_RDONLY, NULL) < 0) {
				err = errno;
				continue;
			}
			char *img = xasprintf("%s%s", dir, image);
			bool ok = is_file(img);
			free(img);
			if (ok) {
				char *dev = xstrdup(devs.v[i]);
				strv_free(&devs);
				strv_free(&without);
				return dev;
			}
			warnx("%s has no %s", devs.v[i], image);
			umount2(dir, 0);
			strv_push(&without, devs.v[i]);
		}
		strv_free(&devs);
		if (timeout_ms >= 0 && waited >= timeout_ms)
			break;
		if (!told && waited >= 2000) {
			msg("waiting for the live medium %s", spec);
			told = true;
		}
		if (kctx && waited % 1000 == 0)
			modules_coldplug(kctx);
		sleep_ms(100);
		waited += 100;
	}
	if (err)
		warnx("mounting the medium: %s", strerror(err));
	strv_free(&without);
	return NULL;
}

/* a loop device for FILE, read-only and detached once unmounted; *FD_OUT keeps it until the
 * mount (closing the last descriptor of an autoclear device before that detaches it) */
static char *loop_attach(const char *file, int *fd_out)
{
	int ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
	if (ctl < 0) {
		warn_errno("/dev/loop-control");
		return NULL;
	}
	int fd = open(file, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		warn_errno("%s", file);
		close(ctl);
		return NULL;
	}
	char *dev = NULL;
	for (int tries = 0; tries < 8 && !dev; tries++) {
		int n = ioctl(ctl, LOOP_CTL_GET_FREE);
		if (n < 0) {
			warn_errno("LOOP_CTL_GET_FREE");
			break;
		}
		char *path = xasprintf("/dev/loop%d", n);
		int lfd = -1;
		/* devtmpfs makes the node of a new loop device */
		for (int w = 0; w < 100 && (lfd = open(path, O_RDWR | O_CLOEXEC)) < 0 && errno == ENOENT; w++)
			sleep_ms(10);
		if (lfd < 0) {
			warn_errno("%s", path);
			free(path);
			break;
		}
		struct loop_config cfg;
		memset(&cfg, 0, sizeof cfg);
		cfg.fd = (uint32_t)fd;
		cfg.info.lo_flags = LO_FLAGS_READ_ONLY | LO_FLAGS_AUTOCLEAR;
		snprintf((char *)cfg.info.lo_file_name, sizeof cfg.info.lo_file_name, "%s", file);
		if (ioctl(lfd, LOOP_CONFIGURE, &cfg) == 0) {
			dev = path;
			*fd_out = lfd;
			continue;
		}
		int e = errno;
		close(lfd);
		free(path);
		if (e != EBUSY) { /* EBUSY: another one took it meanwhile */
			errno = e;
			warn_errno("attaching %s to a loop device", file);
			break;
		}
	}
	close(fd);
	close(ctl);
	return dev;
}

static unsigned long long mem_available(void)
{
	char *s = read_file("/proc/meminfo", NULL);
	unsigned long long kib = 0;
	const char *p = s ? strstr(s, "MemAvailable:") : NULL;
	if (p)
		kib = strtoull(p + 13, NULL, 10);
	free(s);
	return kib * 1024;
}

/* SRC into DST (a new file), with the progress in tens of percent */
static int copy_progress(const char *src, const char *dst, unsigned long long size)
{
	int in = open(src, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return -1;
	int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (out < 0) {
		close(in);
		return -1;
	}
	enum { CHUNK = 4 << 20 };
	char *buf = xmalloc(CHUNK);
	unsigned long long done = 0;
	int last = -1, rc = 0;
	for (;;) {
		ssize_t n = read(in, buf, CHUNK);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			rc = -1;
		if (n <= 0)
			break;
		for (ssize_t off = 0; off < n;) {
			ssize_t w = write(out, buf + off, (size_t)(n - off));
			if (w < 0 && errno == EINTR)
				continue;
			if (w <= 0) {
				rc = -1;
				break;
			}
			off += w;
		}
		if (rc < 0)
			break;
		done += (unsigned long long)n;
		int pct = size ? (int)(done * 100 / size) : 100;
		if (pct / 10 != last) {
			last = pct / 10;
			msg("copying the live system into memory: %d%%", pct);
		}
	}
	free(buf);
	close(in);
	if (close(out) < 0)
		rc = -1;
	return rc;
}

/* the root of a live system on NEW_ROOT: medium -> squashfs image (in memory with toram) ->
 * overlay with a tmpfs on top */
static void live_root(const char *spec)
{
	live_modules();
	char *image = cmdline_value("cleanlinux.live.image");
	if (!image || !*image) {
		free(image);
		image = xstrdup(LIVE_IMAGE);
	}
	const char *medium_dir = LIVE_DIR "/medium";
	char *dev = live_medium(spec, image, medium_dir, root_timeout());
	if (!dev) {
		char *why = xasprintf("the live medium %s did not appear (the image must be written to the whole "
				      "stick as it is, with dd or a raw writer, not copied as a file)",
				      spec);
		emergency(why);
	}
	msg("live medium %s", dev);
	char *img = xasprintf("%s%s", medium_dir, image);
	char *toram = cmdline_value("cleanlinux.live.toram");
	if (toram && strcmp(toram, "0") && strcmp(toram, "no")) {
		struct stat st;
		unsigned long long size = stat(img, &st) == 0 ? (unsigned long long)st.st_size : 0;
		unsigned long long avail = mem_available();
		const char *ram_dir = LIVE_DIR "/toram";
		char *opts = xasprintf("mode=0755,size=%llu", size + (16ULL << 20));
		mkdir_p(ram_dir, 0755);
		if (!size || (avail && size > avail / 10 * 7)) {
			warnx("cleanlinux.live.toram: the image (%llu MiB) does not fit into memory (%llu MiB available), "
			      "it stays on the medium",
			      size >> 20, avail >> 20);
		} else if (mount("tmpfs", ram_dir, "tmpfs", MS_NOSUID | MS_NODEV, opts) < 0) {
			warn_errno("mounting a tmpfs on %s", ram_dir);
		} else {
			char *copy = xasprintf("%s/rootfs.sfs", ram_dir);
			if (copy_progress(img, copy, size) == 0) {
				free(img);
				img = copy;
				copy = NULL;
				if (umount2(medium_dir, MNT_DETACH) == 0)
					msg("the live system runs from memory: the medium %s can be removed", dev);
			} else {
				warn_errno("copying %s into memory", img);
				umount2(ram_dir, MNT_DETACH);
			}
			free(copy);
		}
		free(opts);
	}
	free(toram);
	const char *lower = LIVE_DIR "/rootfs", *rw = LIVE_DIR "/rw";
	mkdir_p(lower, 0755);
	int lfd = -1;
	char *loop = loop_attach(img, &lfd);
	if (!loop)
		emergency("no loop device for the live image");
	static const char *const types[] = {"squashfs", "erofs"};
	int r = -1;
	for (size_t i = 0; r < 0 && i < ARRAY_LEN(types); i++)
		r = mount_dev(loop, lower, types[i], MS_RDONLY, NULL);
	if (r < 0) {
		char *why = xasprintf("cannot mount the live image %s: %s", img, strerror(errno));
		emergency(why);
	}
	close(lfd);
	mkdir_p(rw, 0755);
	if (mount("tmpfs", rw, "tmpfs", 0, "mode=0755") < 0)
		emergency("cannot mount the tmpfs of the live system");
	mkdir_p(LIVE_DIR "/rw/upper", 0755);
	mkdir_p(LIVE_DIR "/rw/work", 0755);
	mkdir_p(NEW_ROOT, 0755);
	if (mount("overlay", NEW_ROOT, "overlay", 0,
		  "lowerdir=" LIVE_DIR "/rootfs,upperdir=" LIVE_DIR "/rw/upper,workdir=" LIVE_DIR "/rw/work") < 0) {
		char *why = xasprintf("cannot mount the overlay of the live system: %s", strerror(errno));
		emergency(why);
	}
	msg("live system: %s from %s", image, dev);
	free(loop);
	free(img);
	free(image);
	free(dev);
}

/* the root of an installed system: SPEC mounted on NEW_ROOT */
static void mount_root(const char *spec)
{
	char *dev = wait_for(spec, root_timeout());
	if (!dev) {
		char *why = xasprintf("the root device %s did not appear", spec);
		emergency(why);
	}
	char *fstype = cmdline_value("rootfstype"), *flags = cmdline_value("rootflags");
	/* the last of ro and rw wins (the kernel's default is ro) */
	bool rw = false;
	for (const char *c = cmdline(), *p = c; *p; p++)
		if ((p == c || p[-1] == ' ') && (p[0] == 'r') && (p[1] == 'o' || p[1] == 'w') && (p[2] == ' ' || !p[2]))
			rw = p[1] == 'w';
	mkdir_p(NEW_ROOT, 0755);
	if (mount_dev(dev, NEW_ROOT, fstype, rw ? 0 : MS_RDONLY, flags) < 0) {
		char *why = xasprintf("cannot mount %s on " NEW_ROOT ": %s", dev, strerror(errno));
		emergency(why);
	}
	free(fstype);
	free(flags);
	free(dev);
}

/* ---- switching ---- */

static int rm_initramfs(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
	(void)st;
	if (ftw->level == 0 || str_prefix(path, NEW_ROOT))
		return 0;
	if (type == FTW_DP)
		rmdir(path); /* mount points stay */
	else
		unlink(path);
	return 0;
}

static _Noreturn void switch_to_root(void)
{
	static const char *const moves[] = {"/dev", "/proc", "/sys", "/run"};
	for (size_t i = 0; i < ARRAY_LEN(moves); i++) {
		char *to = xasprintf(NEW_ROOT "%s", moves[i]);
		if (is_dir(to) && mount(moves[i], to, NULL, MS_MOVE, NULL) == 0) {
			free(to);
			continue;
		}
		warn_errno("moving %s", moves[i]);
		umount2(moves[i], MNT_DETACH);
		free(to);
	}
	/* free the memory of the initramfs, then make the root the root */
	nftw("/", rm_initramfs, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
	if (chdir(NEW_ROOT) < 0 || mount(".", "/", NULL, MS_MOVE, NULL) < 0 || chroot(".") < 0 || chdir("/") < 0)
		die("switching to the root: %s", strerror(errno));
	if (!init_path)
		init_path = xstrdup("/sbin/init");
	char *argv[] = {init_path, NULL};
	execv(init_path, argv);
	die("exec %s: %s", init_path, strerror(errno));
}

static void load_listed_modules(void)
{
	char *s = read_file(MODULES_LIST, NULL);
	if (!s || !kctx) {
		free(s);
		return;
	}
	strv names = STRV_INIT;
	for (char *save = NULL, *l = strtok_r(s, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		l = trim(l);
		if (*l && *l != '#')
			strv_push(&names, l);
	}
	modules_load_list(kctx, &names, true);
	strv_free(&names);
	free(s);
}

int main(void)
{
	prog = "cleanlinux-init";
	to_kmsg = true;
	if (getpid() != 1)
		die("this is the init of an initramfs, it runs as PID 1 only");
	console_stdio();
	umask(022);
	if (mount_api(true))
		warnx("some API filesystems could not be mounted");
	console_stdio(); /* /dev is devtmpfs now */
	kctx = kmod_open(NULL);
	if (kctx) {
		load_listed_modules();
		char *extra = cmdline_value("rd.modules-load");
		if (extra) {
			strv names = STRV_INIT;
			for (char *save = NULL, *m = strtok_r(extra, ",", &save); m; m = strtok_r(NULL, ",", &save))
				strv_push(&names, m);
			modules_load_list(kctx, &names, false);
			strv_free(&names);
			free(extra);
		}
		modules_coldplug(kctx);
	}
	open_luks();
	char *spec = cmdline_value("root");
	if (!spec || !*spec)
		emergency("no root= on the kernel command line");
	if (str_prefix(spec, "live:"))
		live_root(spec + 5);
	else
		mount_root(spec);
	char *init = cmdline_value("init");
	static const char *const inits[] = {"/sbin/init", "/usr/bin/init", "/usr/bin/dinit"};
	for (size_t i = 0; !init && i < ARRAY_LEN(inits); i++) {
		char *p = xasprintf(NEW_ROOT "%s", inits[i]);
		if (access(p, X_OK) == 0)
			init = xstrdup(inits[i]);
		free(p);
	}
	if (!init)
		emergency("the root has no init (/sbin/init)");
	init_path = init;
	if (kctx)
		kmod_unref(kctx);
	char *brk = cmdline_value("rd.break");
	if (brk) {
		free(brk);
		emergency("rd.break: the root is mounted on " NEW_ROOT);
	}
	switch_to_root();
}
