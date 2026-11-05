/* SPDX-License-Identifier: MIT */
/* cleanlinux-boot: the boot of CleanLinux under dinit, one small program with a command per
 * step. The services in /usr/lib/dinit.d call it; as /usr/lib/dinit/shutdown-hook it is the
 * last thing that runs before the power goes off. */
#define _GNU_SOURCE
#include "boot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
	const char *name;
	int (*fn)(int, char **);
	const char *help;
} commands[] = {
	{"early-mounts", cmd_early_mounts, "mount /proc, /sys, /dev, /run and the other API filesystems"},
	{"fsck-root", cmd_fsck_root, "check the root filesystem (fsck.mode=, fstab pass number)"},
	{"remount-root", cmd_remount_root, "remount the root read-write (unless fstab says ro)"},
	{"fsck-all", cmd_fsck_all, "check the other filesystems of fstab"},
	{"mount-all", cmd_mount_all, "mount fstab (without _netdev) and enable swap"},
	{"sysctl", cmd_sysctl, "apply sysctl.d and /etc/sysctl.conf"},
	{"modules", cmd_modules, "load modules-load.d and modules-load= of the command line"},
	{"coldplug", cmd_coldplug, "load the module of every device (systems without udev)"},
	{"udev-trigger", cmd_udev, "compile the hwdb when needed, replay the kernel's device events"},
	{"hostname", cmd_hostname, "set the host name from /etc/hostname [--print]"},
	{"seed", cmd_seed, "load|save the random seed"},
	{"clock", cmd_clock, "load|save the system clock (RTC, adjtime, never before the build)"},
	{"machine-id", cmd_machine_id, "create /etc/machine-id on the first boot"},
	{"tmpfiles", cmd_tmpfiles, "create the files of tmpfiles.d [--boot] [--remove]"},
	{"sysusers", cmd_sysusers, "add the system users and groups of sysusers.d [FILE...]"},
	{"getty", cmd_getty, "run agetty on a terminal: getty TTY"},
	{"consoles", cmd_consoles, "start a serial getty on each serial console [--print]"},
	{"kernel", cmd_kernel, "sync|list|config: the kernels for the loaders of boot.d (systemd-boot, extlinux, rpi, android)"},
	{"initramfs", cmd_initramfs, "make an initramfs: initramfs [KVER] [-o FILE]"},
	{"zram", cmd_zram, "start|stop|--print: swap in compressed RAM, sized by the RAM (zram.d)"},
	{"backlight", cmd_backlight, "load|save the brightness of the backlights"},
	{"power", cmd_power, "apply power.d: platform profile, CPU governor and EPP, charge and power limits [--print]"},
	{"vconsole", cmd_vconsole, "the keyboard map and font of the virtual terminals (vconsole.conf) [--print]"},
	{"shutdown-hook", cmd_shutdown_hook, "swap off, unmount, remount the root read-only"},
};

static void usage(FILE *f)
{
	fprintf(f, "usage: cleanlinux-boot [--root DIR] [--kmsg] COMMAND [ARGS]\n\ncommands:\n");
	for (size_t i = 0; i < ARRAY_LEN(commands); i++)
		fprintf(f, "  %-14s %s\n", commands[i].name, commands[i].help);
}

int main(int argc, char **argv)
{
	const char *base = strrchr(argv[0], '/');
	base = base ? base + 1 : argv[0];
	/* the dinit shutdown hook: a link to this program */
	if (!strcmp(base, "shutdown-hook")) {
		prog = "shutdown-hook";
		to_kmsg = true;
		return cmd_shutdown_hook(argc, argv);
	}
	int i = 1;
	for (; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "--root") && i + 1 < argc) {
			root = argv[++i];
			if (!strcmp(root, "/"))
				root = "";
		} else if (!strcmp(argv[i], "--kmsg")) {
			to_kmsg = true;
		} else if (!strcmp(argv[i], "--version")) {
			puts("cleanlinux-boot " VERSION);
			return 0;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(stdout);
			return 0;
		} else {
			usage(stderr);
			return 2;
		}
	}
	if (i >= argc) {
		usage(stderr);
		return 2;
	}
	for (size_t c = 0; c < ARRAY_LEN(commands); c++) {
		if (strcmp(argv[i], commands[c].name))
			continue;
		prog = xasprintf("cleanlinux-boot %s", commands[c].name);
		return commands[c].fn(argc - i, argv + i);
	}
	fprintf(stderr, "cleanlinux-boot: unknown command: %s\n", argv[i]);
	usage(stderr);
	return 2;
}
