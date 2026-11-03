/* SPDX-License-Identifier: MIT */
/* Filesystems: the kernel's API filesystems early in the boot, fsck, the root read-write,
 * fstab, swap, and the reverse at shutdown (the dinit shutdown hook). */
#define _GNU_SOURCE
#include "boot.h"

#include <errno.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <unistd.h>

static bool fs_supported(const char *type)
{
	char *fs = read_file("/proc/filesystems", NULL);
	bool ok = false;
	if (!fs)
		return true; /* /proc is not there yet: just try */
	for (char *l = strtok(fs, "\n"); l && !ok; l = strtok(NULL, "\n")) {
		char *t = strchr(l, '\t');
		ok = !strcmp(t ? t + 1 : l, type);
	}
	free(fs);
	return ok;
}

struct api_mount {
	const char *what, *where, *type;
	unsigned long flags;
	const char *data;
	bool optional; /* only when the kernel has the filesystem and the directory exists */
};

static const struct api_mount api_mounts[] = {
	{"proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL, false},
	{"sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL, false},
	{"devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755", false},
	{"devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "gid=5,mode=0620,ptmxmode=0666", false},
	{"shm", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777", false},
	{"mqueue", "/dev/mqueue", "mqueue", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL, true},
	{"run", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755,size=20%", false},
	{"securityfs", "/sys/kernel/security", "securityfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL, true},
	{"cgroup2", "/sys/fs/cgroup", "cgroup2", MS_NOSUID | MS_NODEV | MS_NOEXEC, "nsdelegate", true},
	{"efivarfs", "/sys/firmware/efi/efivars", "efivarfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL, true},
	{"bpf", "/sys/fs/bpf", "bpf", MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=0700", true},
	{"pstore", "/sys/fs/pstore", "pstore", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL, true},
	{"configfs", "/sys/kernel/config", "configfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL, true},
	{"debugfs", "/sys/kernel/debug", "debugfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=0700", true},
	{"tracefs", "/sys/kernel/tracing", "tracefs", MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=0700", true},
};

int mount_api(bool quiet)
{
	int failed = 0;
	for (size_t i = 0; i < ARRAY_LEN(api_mounts); i++) {
		const struct api_mount *m = &api_mounts[i];
		if (is_mountpoint(m->where))
			continue;
		if (m->optional && (!is_dir(m->where) || !fs_supported(m->type)))
			continue;
		if (!m->optional)
			mkdir_p(m->where, 0755);
		if (mount(m->what, m->where, m->type, m->flags, m->data) < 0) {
			/* older kernels: cgroup2 without nsdelegate */
			if (!strcmp(m->type, "cgroup2") && mount(m->what, m->where, m->type, m->flags, NULL) == 0)
				continue;
			if (!m->optional || !quiet)
				warn_errno("mounting %s on %s", m->type, m->where);
			failed += !m->optional;
		}
	}
	/* what devtmpfs does not create */
	static const char *const links[][2] = {
		{"/proc/self/fd", "/dev/fd"},     {"/proc/self/fd/0", "/dev/stdin"},
		{"/proc/self/fd/1", "/dev/stdout"}, {"/proc/self/fd/2", "/dev/stderr"},
	};
	for (size_t i = 0; i < ARRAY_LEN(links); i++)
		if (symlink(links[i][0], links[i][1]) < 0 && errno != EEXIST)
			warn_errno("symlink %s", links[i][1]);
	return failed ? 1 : 0;
}

int cmd_early_mounts(int argc, char **argv)
{
	(void)argc, (void)argv;
	return mount_api(false);
}

/* the fstab entry of DIR: its options and pass number (the last entry wins, as for mount -a) */
struct fstab_ent {
	bool found;
	char *opts;
	int passno;
};

static struct fstab_ent fstab_entry(const char *dir)
{
	struct fstab_ent e = {false, NULL, 0};
	char *path = rpath("/etc/fstab");
	FILE *f = setmntent(path, "r");
	free(path);
	if (!f)
		return e;
	struct mntent *m;
	while ((m = getmntent(f)))
		if (!strcmp(m->mnt_dir, dir)) {
			free(e.opts);
			e.found = true;
			e.opts = xstrdup(m->mnt_opts);
			e.passno = m->mnt_passno;
		}
	endmntent(f);
	return e;
}

static bool has_opt(const char *opts, const char *opt)
{
	if (!opts)
		return false;
	size_t n = strlen(opt);
	for (const char *s = opts; s && *s; s = strchr(s, ',') ? strchr(s, ',') + 1 : NULL)
		if (!strncmp(s, opt, n) && (s[n] == ',' || s[n] == '\0'))
			return true;
	return false;
}

static bool root_is_ro(void)
{
	return access("/", W_OK) < 0 && errno == EROFS;
}

/* fsck exit codes: 0 clean, 1 fixed, 2 fixed and the system must reboot, 4+ problems left */
static int fsck_result(int rc, const char *what)
{
	if (rc == 0 || rc == 1)
		return 0;
	if (rc == 2 || rc == 3) {
		msg("fsck repaired %s and asks for a reboot", what);
		sync();
		reboot(RB_AUTOBOOT);
		return 1;
	}
	if (rc < 0) {
		warnx("fsck could not run; %s is not checked", what);
		return 0;
	}
	warnx("fsck found problems on %s it could not fix (exit %d); repair it by hand", what, rc);
	return 1;
}

int cmd_fsck_root(int argc, char **argv)
{
	(void)argc, (void)argv;
	char *force = cmdline_value("fsck.mode");
	bool skip = force && !strcmp(force, "skip"), forced = force && !strcmp(force, "force");
	free(force);
	if (skip || !root_is_ro())
		return 0; /* a read-write root cannot be checked */
	struct fstab_ent e = fstab_entry("/");
	free(e.opts);
	if (e.found && e.passno == 0 && !forced)
		return 0;
	char *fsck = find_program("fsck");
	if (!fsck)
		return 0;
	int rc = forced ? runl(fsck, "-a", "-f", "/", (char *)NULL) : runl(fsck, "-a", "/", (char *)NULL);
	free(fsck);
	return fsck_result(rc, "the root filesystem");
}

int cmd_remount_root(int argc, char **argv)
{
	(void)argc, (void)argv;
	struct fstab_ent e = fstab_entry("/");
	bool ro = has_opt(e.opts, "ro");
	free(e.opts);
	if (ro)
		return 0; /* a read-only root by choice */
	if (!root_is_ro())
		return 0;
	char *mnt = find_program("mount");
	int rc = mnt ? runl(mnt, "-o", "remount,rw", "/", (char *)NULL) : -1;
	free(mnt);
	if (rc != 0 && mount(NULL, "/", NULL, MS_REMOUNT, NULL) < 0) {
		warn_errno("remounting / read-write");
		return 1;
	}
	return 0;
}

int cmd_fsck_all(int argc, char **argv)
{
	(void)argc, (void)argv;
	char *force = cmdline_value("fsck.mode");
	bool skip = force && !strcmp(force, "skip"), forced = force && !strcmp(force, "force");
	free(force);
	if (skip)
		return 0;
	char *fsck = find_program("fsck");
	if (!fsck)
		return 0;
	/* every fstab filesystem with a pass number except the root, skipping network ones */
	int rc = forced ? runl(fsck, "-A", "-R", "-T", "-a", "-f", "-t", "noopts=_netdev", (char *)NULL)
			: runl(fsck, "-A", "-R", "-T", "-a", "-t", "noopts=_netdev", (char *)NULL);
	free(fsck);
	return fsck_result(rc, "a filesystem from /etc/fstab");
}

int cmd_mount_all(int argc, char **argv)
{
	(void)argc, (void)argv;
	char *mnt = find_program("mount");
	if (!mnt)
		die("mount (util-linux) is missing");
	int rc = runl(mnt, "-a", "-O", "no_netdev", (char *)NULL);
	free(mnt);
	char *swapon = find_program("swapon");
	if (swapon) {
		if (runl(swapon, "-a", (char *)NULL) != 0)
			warnx("swapon -a failed");
		free(swapon);
	}
	return rc == 0 ? 0 : 1;
}

/* ---- shutdown ---- */

static bool api_mount(const char *dir)
{
	static const char *const keep[] = {"/", "/proc", "/sys", "/dev", "/run"};
	for (size_t i = 0; i < ARRAY_LEN(keep); i++)
		if (!strcmp(dir, keep[i]))
			return true;
	return str_prefix(dir, "/proc/") || str_prefix(dir, "/sys/") || str_prefix(dir, "/dev/") ||
	       str_prefix(dir, "/run/");
}

static void swaps_off(void)
{
	char *s = read_file("/proc/swaps", NULL);
	if (!s)
		return;
	char *save, *line = strtok_r(s, "\n", &save); /* header */
	while ((line = strtok_r(NULL, "\n", &save))) {
		line[strcspn(line, " \t")] = '\0';
		/* /proc/swaps escapes spaces as \040 */
		char path[4096];
		size_t n = 0;
		for (char *c = line; *c && n + 1 < sizeof(path); c++) {
			if (c[0] == '\\' && c[1] >= '0' && c[1] <= '3' && c[2] && c[3]) {
				path[n++] = (char)((c[1] - '0') * 64 + (c[2] - '0') * 8 + (c[3] - '0'));
				c += 3;
			} else {
				path[n++] = *c;
			}
		}
		path[n] = '\0';
		if (swapoff(path) < 0)
			warn_errno("swapoff %s", path);
	}
	free(s);
}

/* unmount everything but the API filesystems, newest first; read-only when busy */
static int unmount_all(void)
{
	int left = 0;
	for (int pass = 0; pass < 3; pass++) {
		FILE *f = setmntent("/proc/self/mounts", "r");
		if (!f)
			return 1;
		strv dirs = STRV_INIT;
		struct mntent *m;
		while ((m = getmntent(f)))
			if (!api_mount(m->mnt_dir))
				strv_push(&dirs, m->mnt_dir);
		endmntent(f);
		left = 0;
		for (size_t i = dirs.n; i-- > 0;) {
			if (umount2(dirs.v[i], 0) == 0)
				continue;
			if (errno == EINVAL || errno == ENOENT)
				continue; /* gone with its parent */
			if (pass == 2) {
				if (mount(NULL, dirs.v[i], NULL, MS_REMOUNT | MS_RDONLY, NULL) < 0) {
					warn_errno("%s stays mounted read-write", dirs.v[i]);
					left++;
				}
			} else {
				left++;
			}
		}
		strv_free(&dirs);
		if (!left)
			break;
	}
	return left;
}

int cmd_shutdown_hook(int argc, char **argv)
{
	(void)argc, (void)argv;
	sync();
	swaps_off();
	int left = unmount_all();
	sync();
	if (mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL) < 0 && errno != EBUSY) {
		warn_errno("remounting / read-only");
		left++;
	} else if (!root_is_ro()) {
		warnx("/ is still writable");
		left++;
	}
	sync();
	/* 0 tells dinit's shutdown that everything is unmounted or read-only */
	return left ? 1 : 0;
}
