# burn runs this after a transaction that touched one of the paths: the boot partition (and the
# Android boot images) follow the installed kernels, the loaders of the device (boot.d), the
# initramfs configuration and systemd-boot
path=usr/lib/modules/*/vmlinuz
path=usr/lib/systemd/boot/efi/*
path=usr/lib/cleanlinux/boot.d/*
path=usr/lib/cleanlinux/initramfs.d/*
# $ROOT: "/" on the running system, the target root for burn -r /mnt
run=/usr/bin/cleanlinux-boot --root $ROOT kernel sync
