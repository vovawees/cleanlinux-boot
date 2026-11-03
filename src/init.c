/* SPDX-License-Identifier: MIT */
/* cleanlinux-init: PID 1 of the initramfs that "cleanlinux-boot initramfs" makes.
 *
 * Only systems that cannot boot without one need it (an encrypted root, storage drivers that
 * are modules): it mounts the API filesystems, loads the modules listed in the initramfs and
 * the drivers of the devices it finds, opens LUKS (rd.luks.uuid=, with cryptsetup in the
 * initramfs), waits for root= (/dev/..., UUID=, LABEL=, PARTUUID=, PARTLABEL=), mounts it
 * with rootfstype=, rootflags=, ro/rw, and hands over to init= (default /sbin/init).
 * rd.break drops to a shell (when the initramfs has one) right before the switch. */
#define _GNU_SOURCE
#include "boot.h"

#include <blkid.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <libkmod.h>
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

/* a device of /sys/class/block whose TAG (UUID, LABEL, PARTUUID, PARTLABEL, TYPE) is VALUE */
static char *find_by_tag(const char *tag, const char *value)
{
	DIR *d = opendir("/sys/class/block");
	if (!d)
		return NULL;
	char *found = NULL;
	struct dirent *e;
	while (!found && (e = readdir(d))) {
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
					found = xstrdup(dev);
			}
			blkid_free_probe(pr);
		}
		free(dev);
	}
	closedir(d);
	return found;
}

static char *resolve(const char *spec)
{
	static const char *const tags[] = {"UUID", "LABEL", "PARTUUID", "PARTLABEL"};
	for (size_t i = 0; i < ARRAY_LEN(tags); i++) {
		size_t n = strlen(tags[i]);
		if (!strncmp(spec, tags[i], n) && spec[n] == '=')
			return find_by_tag(tags[i], spec + n + 1);
	}
	if (str_prefix(spec, "/dev/"))
		return exists(spec) ? xstrdup(spec) : NULL;
	return NULL;
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
	unsigned long mflags = rw ? 0 : MS_RDONLY;
	int r = -1;
	if (fstype && *fstype) {
		r = mount(dev, NEW_ROOT, fstype, mflags, flags);
	} else {
		/* every filesystem the kernel has, as the kernel does it */
		char *fs = read_file("/proc/filesystems", NULL);
		for (char *save = NULL, *l = fs ? strtok_r(fs, "\n", &save) : NULL; l && r < 0; l = strtok_r(NULL, "\n", &save)) {
			if (str_prefix(l, "nodev"))
				continue;
			r = mount(dev, NEW_ROOT, trim(l), mflags, flags);
		}
		free(fs);
	}
	if (r < 0) {
		char *why = xasprintf("cannot mount %s on " NEW_ROOT ": %s", dev, strerror(errno));
		emergency(why);
	}
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
