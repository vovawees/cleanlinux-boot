/* SPDX-License-Identifier: MIT */
#ifndef CLB_BOOT_H
#define CLB_BOOT_H

#include "util.h"

#ifndef VERSION
#define VERSION "0"
#endif
/* the clock never starts earlier than the build of this program (SOURCE_DATE_EPOCH) */
#ifndef BUILD_EPOCH
#define BUILD_EPOCH 0
#endif
#ifndef LIBEXECDIR
#define LIBEXECDIR "/usr/lib/cleanlinux-boot"
#endif
#define STATE_DIR "/var/lib/cleanlinux"

int mount_api(bool quiet);

/* modules.c */
struct kmod_ctx;
struct kmod_ctx *kmod_open(const char *kver);
int modules_load_list(struct kmod_ctx *ctx, const strv *names, bool quiet);
int modules_coldplug(struct kmod_ctx *ctx);

/* initramfs.c */
bool initramfs_wanted(void);
int initramfs_build(const char *kver, const char *out_path);

/* commands: cleanlinux-boot <command> [args] */
int cmd_early_mounts(int argc, char **argv);
int cmd_fsck_root(int argc, char **argv);
int cmd_remount_root(int argc, char **argv);
int cmd_fsck_all(int argc, char **argv);
int cmd_mount_all(int argc, char **argv);
int cmd_shutdown_hook(int argc, char **argv);
int cmd_sysctl(int argc, char **argv);
int cmd_modules(int argc, char **argv);
int cmd_coldplug(int argc, char **argv);
int cmd_hostname(int argc, char **argv);
int cmd_seed(int argc, char **argv);
int cmd_clock(int argc, char **argv);
int cmd_machine_id(int argc, char **argv);
int cmd_tmpfiles(int argc, char **argv);
int cmd_sysusers(int argc, char **argv);
int cmd_udev(int argc, char **argv);
int cmd_getty(int argc, char **argv);
int cmd_consoles(int argc, char **argv);
int cmd_kernel(int argc, char **argv);
int cmd_initramfs(int argc, char **argv);

#endif
