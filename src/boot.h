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

/* bootimg.c: Android boot images (mkbootimg header versions 0, 1 and 2) */
#include <stdint.h>
typedef struct {
	unsigned header_version;
	size_t page_size;
	uint32_t base, kernel_offset, ramdisk_offset, tags_offset, dtb_offset;
	uint32_t os_version; /* bootimg_os_version */
	const char *board, *cmdline;
	const void *kernel, *ramdisk, *dtb; /* dtb: its own section with header version 2 */
	size_t kernel_len, ramdisk_len, dtb_len;
} bootimg;
/* the image (malloc-ed, *len bytes), or NULL with *err (malloc-ed) */
void *bootimg_build(const bootimg *bi, size_t *len, char **err);
/* "13.0.0" and "2024-05" packed as in the header; -1 with *err when malformed */
int bootimg_os_version(const char *version, const char *patch_level, uint32_t *out, char **err);
/* IMG to the start of the partition DEV, when DEV holds an Android boot image (or zeros) and the
 * image fits; the first flash saves DEV into BACKUP. 0 also when DEV holds IMG already */
int bootimg_flash(const void *img, size_t len, const char *dev, const char *backup, bool dry_run);

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
int cmd_zram(int argc, char **argv);
int cmd_backlight(int argc, char **argv);
int cmd_power(int argc, char **argv);

#endif
