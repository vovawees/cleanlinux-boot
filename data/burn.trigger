# burn runs this after a transaction that touched one of the paths: the EFI system partition
# follows the installed kernels, the initramfs configuration and systemd-boot
path=usr/lib/modules/*/vmlinuz
path=usr/lib/systemd/boot/efi/*
path=usr/lib/cleanlinux/initramfs.d/*
# $ROOT: "/" on the running system, the target root for burn -r /mnt
run=/usr/bin/cleanlinux-boot --root $ROOT kernel sync
