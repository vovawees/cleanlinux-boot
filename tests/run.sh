#!/usr/bin/env bash
# Unit tests of cleanlinux-boot: every command that can run against a root directory runs
# against a scratch one; nothing here needs root or touches the host.
#
#   tests/run.sh ./cleanlinux-boot
set -euo pipefail

BIN=$(readlink -f "${1:-./cleanlinux-boot}")
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fails=0 count=0

ok() { count=$((count + 1)) && printf 'ok %d - %s\n' "$count" "$1"; }
not_ok() { count=$((count + 1)) && fails=$((fails + 1)) && printf 'not ok %d - %s\n' "$count" "$1"; }
check() { # check NAME COMMAND...
	local name=$1 && shift
	if "$@"; then ok "$name"; else not_ok "$name"; fi
}
clb() { "$BIN" --root "$R" "$@"; }

new_root() {
	R=$TMP/root.$count
	mkdir -p "$R"/etc "$R"/usr/lib "$R"/proc/sys/kernel "$R"/proc/sys/net/ipv4/conf/eth0 "$R"/sys/class/tty/console "$R"/dev
}

# ---- hostname ----
new_root
check "hostname: default" test "$(clb hostname --print)" = cleanlinux
printf '# comment\n\n  box-1  \n' >"$R/etc/hostname"
check "hostname: /etc/hostname, comments and blanks skipped" test "$(clb hostname --print)" = box-1
printf 'bad_name!\n' >"$R/etc/hostname"
check "hostname: an invalid name falls back" test "$(clb hostname --print 2>/dev/null)" = cleanlinux

# ---- sysctl ----
new_root
: >"$R/proc/sys/kernel/a" && : >"$R/proc/sys/kernel/b" && : >"$R/proc/sys/net/ipv4/conf/eth0/rp_filter"
mkdir -p "$R/usr/lib/sysctl.d" "$R/etc/sysctl.d"
printf 'kernel.a = 1\nkernel.b=2\n' >"$R/usr/lib/sysctl.d/50-default.conf"
printf 'kernel.a = 3\n' >"$R/etc/sysctl.d/90-local.conf"
printf 'kernel/b = 4\n-kernel.missing = 1\nnet.ipv4.conf.eth0.rp_filter = 2\n' >"$R/etc/sysctl.conf"
clb sysctl
check "sysctl: later files win" test "$(cat "$R/proc/sys/kernel/a")" = 3
check "sysctl: /etc/sysctl.conf last, slashes" test "$(cat "$R/proc/sys/kernel/b")" = 4
check "sysctl: dotted keys" test "$(cat "$R/proc/sys/net/ipv4/conf/eth0/rp_filter")" = 2
ln -s /dev/null "$R/etc/sysctl.d/50-default.conf"
printf '' >"$R/proc/sys/kernel/a" && rm "$R/etc/sysctl.d/90-local.conf"
clb sysctl
check "sysctl: a link to /dev/null masks a file" test -z "$(cat "$R/proc/sys/kernel/a")"

# ---- tmpfiles ----
new_root
printf 'root:x:0:0::/root:/bin/sh\nalice:x:1000:1000::/home/alice:/bin/sh\n' >"$R/etc/passwd"
printf 'root:x:0:\nutmp:x:13:\nalice:x:1000:\n' >"$R/etc/group"
mkdir -p "$R/usr/lib/tmpfiles.d" "$R/etc/tmpfiles.d"
cat >"$R/usr/lib/tmpfiles.d/test.conf" <<'CONF'
# a comment
d /run/lock 0755 - - -
d /var/lib/thing/deep 0700 - - -
f /run/hello 0640 - - - hello world
L /var/run-link - - - - ../run
L+ /var/replaced - - - - target
D /var/cache/empty 0755 - - -
d!  /run/bootonly 0755 - - -
w /run/wfile - - - - written
w /run/nothing-* - - - - x
q /var/unsupported - - - -
d /run/%m 0755 - - -
CONF
echo 0123456789abcdef0123456789abcdef >"$R/etc/machine-id"
mkdir -p "$R/var/cache/empty/sub" && : >"$R/var/cache/empty/sub/file"
: >"$R/var/replaced"
mkdir -p "$R/run" && echo old >"$R/run/wfile"
clb tmpfiles
check "tmpfiles: d" test -d "$R/run/lock"
check "tmpfiles: d with parents and mode" test "$(stat -c %a "$R/var/lib/thing/deep")" = 700
check "tmpfiles: f with content" test "$(cat "$R/run/hello")" = "hello world"
check "tmpfiles: w writes to existing files" test "$(cat "$R/run/wfile")" = written
check "tmpfiles: w creates nothing" bash -c "! compgen -G '$R/run/nothing*'"
check "tmpfiles: f mode" test "$(stat -c %a "$R/run/hello")" = 640
check "tmpfiles: L" test "$(readlink "$R/var/run-link")" = ../run
check "tmpfiles: L+ replaces" test "$(readlink "$R/var/replaced")" = target
check "tmpfiles: D keeps contents without --remove" test -e "$R/var/cache/empty/sub/file"
check "tmpfiles: ! only with --boot" test ! -e "$R/run/bootonly"
check "tmpfiles: %m" test -d "$R/run/0123456789abcdef0123456789abcdef"
clb tmpfiles --boot --remove
check "tmpfiles: D --remove empties" test -z "$(ls -A "$R/var/cache/empty")"
check "tmpfiles: ! with --boot" test -d "$R/run/bootonly"
printf 'f /run/owned 0600 alice utmp -\n' >"$R/etc/tmpfiles.d/owned.conf"
if [[ $(id -u) == 0 ]]; then
	clb tmpfiles
	check "tmpfiles: owner and group" test "$(stat -c %u:%g "$R/run/owned")" = 1000:13
fi
printf 'd /run/x 0755 nosuchuser - -\n' >"$R/etc/tmpfiles.d/owned.conf"
check "tmpfiles: an unknown user is reported" bash -c "'$BIN' --root '$R' tmpfiles 2>&1 | grep -q 'unknown user nosuchuser'"

# ---- sysusers ----
new_root
printf 'root:x:0:0::/root:/bin/bash\nbin:x:1:1::/:/usr/bin/nologin\n' >"$R/etc/passwd"
printf 'root:!*:20000::::::\nbin:!*:20000::::::\n' >"$R/etc/shadow"
printf 'root:x:0:\nbin:x:1:\nwheel:x:998:\n' >"$R/etc/group"
printf 'root:!::\nbin:!::\nwheel:!::\n' >"$R/etc/gshadow"
printf 'SYS_UID_MIN 100\nSYS_UID_MAX 999\nSYS_GID_MIN 100\nSYS_GID_MAX 999\n' >"$R/etc/login.defs"
mkdir -p "$R/usr/lib/sysusers.d"
cat >"$R/usr/lib/sysusers.d/test.conf" <<'CONF'
# a comment
g netdev -
u dhcpcd - "dhcpcd privilege separation" /var/lib/dhcpcd
u messagebus 81 "System message bus"
u polkitd 102:netdev "PolicyKit" - /usr/bin/nologin
m dhcpcd netdev
r - 500-900
CONF
clb sysusers >/dev/null
check "sysusers: g takes the highest free ID" grep -qx 'netdev:x:999:dhcpcd' "$R/etc/group"
check "sysusers: u with its own group, free in both files" grep -qx 'dhcpcd:x:997:997:dhcpcd privilege separation:/var/lib/dhcpcd:/usr/bin/nologin' "$R/etc/passwd"
check "sysusers: u with a fixed ID" grep -qx 'messagebus:x:81:81:System message bus:/:/usr/bin/nologin' "$R/etc/passwd"
check "sysusers: u UID:GROUP" grep -qx 'polkitd:x:102:999:PolicyKit:/:/usr/bin/nologin' "$R/etc/passwd"
check "sysusers: locked shadow entries" grep -q '^dhcpcd:!\*:[0-9]*::::::$' "$R/etc/shadow"
check "sysusers: gshadow members" grep -qx 'netdev:!::dhcpcd' "$R/etc/gshadow"
check "sysusers: shadow stays 0600" test "$(stat -c %a "$R/etc/shadow")" = 600
sum=$(cat "$R"/etc/{passwd,shadow,group,gshadow} | cksum)
clb sysusers >/dev/null
check "sysusers: a second run changes nothing" test "$(cat "$R"/etc/{passwd,shadow,group,gshadow} | cksum)" = "$sum"
printf 'u bad:name - "x"\n' >"$R/etc/sysusers.d.conf"
check "sysusers: invalid names are refused" bash -c "! '$BIN' --root '$R' sysusers '$R/etc/sysusers.d.conf' 2>/dev/null"

# ---- machine-id ----
new_root
clb machine-id
id=$(cat "$R/etc/machine-id")
check "machine-id: 32 hex digits" bash -c "[[ '$id' =~ ^[0-9a-f]{32}$ ]]"
check "machine-id: version 4" test "${id:12:1}" = 4
clb machine-id
check "machine-id: kept" test "$(cat "$R/etc/machine-id")" = "$id"

# ---- seed ----
new_root
clb seed save
check "seed: saved, 512 bytes, 0600" test "$(stat -c %s:%a "$R/var/lib/cleanlinux/random-seed")" = 512:600

# ---- getty and consoles ----
new_root
echo "tty0 ttyS0 ttyAMA0" >"$R/sys/class/tty/console/active"
: >"$R/dev/ttyS0"
check "consoles: serial consoles that exist" test "$(clb consoles --print)" = serial-getty@ttyS0

# ---- kernel sync ----
new_root
mkdir -p "$R/usr/lib/modules/7.3.0-a" "$R/usr/lib/modules/7.2.0-b/kernel" "$R/usr/lib/modules/7.1.0-c" "$R/etc/kernel" "$R/esp"
echo kernel-a >"$R/usr/lib/modules/7.3.0-a/vmlinuz"
echo kernel-b >"$R/usr/lib/modules/7.2.0-b/vmlinuz"
echo "quiet loglevel=3" >"$R/usr/lib/modules/7.3.0-a/cmdline"
printf 'root=PARTUUID=1234 rootflags=subvol=@\n# comment\nrw\n' >"$R/etc/kernel/cmdline"
mkdir -p "$R/usr/lib/kernel/cmdline.d" && echo "zswap.enabled=1" >"$R/usr/lib/kernel/cmdline.d/10-profile.conf"
mkdir -p "$R/usr/lib/systemd/boot/efi" && echo loader >"$R/usr/lib/systemd/boot/efi/systemd-bootx64.efi"
mkdir -p "$R/esp/cleanlinux/6.0-old" "$R/esp/loader/entries" && echo old >"$R/esp/cleanlinux/6.0-old/linux"
echo old >"$R/esp/loader/entries/cleanlinux-6.0-old.conf" && echo mine >"$R/esp/loader/entries/other.conf"
clb kernel sync --esp "$R/esp" >/dev/null 2>&1
E=$R/esp
check "kernel: kernels copied" test "$(cat "$E/cleanlinux/7.3.0-a/linux")$(cat "$E/cleanlinux/7.2.0-b/linux")" = kernel-akernel-b
check "kernel: a directory without vmlinuz is no kernel" test ! -e "$E/cleanlinux/7.1.0-c"
check "kernel: entry" grep -qx 'linux /cleanlinux/7.3.0-a/linux' "$E/loader/entries/cleanlinux-7.3.0-a.conf"
check "kernel: command line from /etc, cmdline.d and the profile" grep -qx 'options root=PARTUUID=1234 rootflags=subvol=@ rw zswap.enabled=1 quiet loglevel=3' "$E/loader/entries/cleanlinux-7.3.0-a.conf"
check "kernel: stale entries and kernels removed" test ! -e "$E/loader/entries/cleanlinux-6.0-old.conf" -a ! -e "$E/cleanlinux/6.0-old"
check "kernel: other entries kept" test -e "$E/loader/entries/other.conf"
if [[ $(uname -m) == x86_64 ]]; then
	check "kernel: systemd-boot as the fallback loader" test "$(cat "$E/EFI/BOOT/BOOTX64.EFI")" = loader
	check "kernel: loader.conf" grep -qx 'default cleanlinux-\*' "$E/loader/loader.conf"
fi
before=$(stat -c %Y.%i "$E/cleanlinux/7.3.0-a/linux")
sleep 1.1
clb kernel sync --esp "$E" >/dev/null 2>&1
check "kernel: unchanged files are not rewritten" test "$(stat -c %Y.%i "$E/cleanlinux/7.3.0-a/linux")" = "$before"
check "kernel: list" bash -c "'$BIN' --root '$R' kernel list | grep -q '^7.3.0-a	root=PARTUUID=1234'"

# ---- boot loaders: cleanlinux/boot.d ----
new_root
K=$R/usr/lib/modules E=$R/esp
mkdir -p "$R/etc/kernel" "$E" "$R/usr/lib/cleanlinux/boot.d" "$R/etc/cleanlinux/boot.d"
for v in 7.3.0-a 7.10.0-b; do
	mkdir -p "$K/$v/dtbs/rockchip/overlay"
	echo "efi-$v" >"$K/$v/vmlinuz" && echo "image-$v" >"$K/$v/Image"
	echo "dtb-$v" >"$K/$v/dtbs/rockchip/rk3566-x.dtb" && echo "dtb2-$v" >"$K/$v/dtbs/rockchip/rk3566-y.dtb"
	echo "ovl-$v" >"$K/$v/dtbs/rockchip/overlay/spi.dtbo"
done
echo 'root=PARTUUID=42 rw' >"$R/etc/kernel/cmdline"
printf 'loader=extlinux\ndevicetree=rockchip/rk3566-x.dtb\noverlays=rockchip/overlay/spi.dtbo\ntimeout=5\n' \
	>"$R/usr/lib/cleanlinux/boot.d/50-profile.conf"
clb kernel sync --esp "$E" >/dev/null 2>&1
X=$E/extlinux/extlinux.conf
check "extlinux: the newest kernel (by version) is the default" grep -qx 'default cleanlinux-7.10.0-b' "$X"
check "extlinux: the newest first" test "$(grep -m1 '^label' "$X")" = "label cleanlinux-7.10.0-b"
check "extlinux: the Image, not the EFI vmlinuz" grep -qx '	linux /cleanlinux/7.10.0-b/Image' "$X"
check "extlinux: Image copied, no EFI kernel" test "$(cat "$E/cleanlinux/7.10.0-b/Image")" = image-7.10.0-b -a ! -e "$E/cleanlinux/7.10.0-b/linux"
check "extlinux: device tree" grep -qx '	fdt /cleanlinux/7.10.0-b/dtbs/rockchip/rk3566-x.dtb' "$X"
check "extlinux: device tree copied" test "$(cat "$E/cleanlinux/7.10.0-b/dtbs/rockchip/rk3566-x.dtb")" = dtb-7.10.0-b
check "extlinux: overlays" grep -qx '	fdtoverlays /cleanlinux/7.3.0-a/dtbs/rockchip/overlay/spi.dtbo' "$X"
check "extlinux: command line" grep -qx '	append root=PARTUUID=42 rw' "$X"
check "extlinux: timeout in tenths of a second" grep -qx 'timeout 50' "$X"
check "extlinux: no systemd-boot entries" test ! -e "$E/loader/entries/cleanlinux-7.10.0-b.conf"
check "kernel config: the settings" bash -c "'$BIN' --root '$R' kernel config | grep -qx 'loader=extlinux'"
# /etc wins over the profile: default=, a directory of device trees (U-Boot picks its $fdtfile)
printf 'default=7.3.0-a\ndevicetree=rockchip/\noverlays=\n' >"$R/etc/cleanlinux/boot.d/90-local.conf"
clb kernel sync --esp "$E" >/dev/null 2>&1
check "extlinux: default=" grep -qx 'default cleanlinux-7.3.0-a' "$X"
check "extlinux: the default kernel first" test "$(grep -m1 '^label' "$X")" = "label cleanlinux-7.3.0-a"
check "extlinux: fdtdir" grep -qx '	fdtdir /cleanlinux/7.3.0-a/dtbs' "$X"
check "extlinux: the device trees of the directory" test -f "$E/cleanlinux/7.3.0-a/dtbs/rockchip/rk3566-y.dtb"
check "extlinux: overlays= empties the list" bash -c "! grep -q fdtoverlays '$X'"
check "boot.d: no way out of dtbs/" bash -c "printf 'devicetree=../../../etc/shadow\n' >'$R/etc/cleanlinux/boot.d/95-bad.conf' && '$BIN' --root '$R' kernel config 2>&1 | grep -q 'a path below'"
rm -f "$R/etc/cleanlinux/boot.d/95-bad.conf"
# systemd-boot with a device tree and an overlay
printf 'loader=systemd-boot\ndevicetree=rockchip/rk3566-x.dtb\noverlays=rockchip/overlay/spi.dtbo\n' >"$R/etc/cleanlinux/boot.d/90-local.conf"
clb kernel sync --esp "$E" >/dev/null 2>&1
S=$E/loader/entries/cleanlinux-7.10.0-b.conf
check "systemd-boot: devicetree" grep -qx 'devicetree /cleanlinux/7.10.0-b/dtbs/rockchip/rk3566-x.dtb' "$S"
check "systemd-boot: devicetree-overlay" grep -qx 'devicetree-overlay /cleanlinux/7.10.0-b/dtbs/rockchip/overlay/spi.dtbo' "$S"
check "systemd-boot: the EFI vmlinuz only" test "$(cat "$E/cleanlinux/7.10.0-b/linux")" = efi-7.10.0-b -a ! -e "$E/cleanlinux/7.10.0-b/Image"
check "extlinux.conf removed with its loader" test ! -e "$X"
# the Raspberry Pi firmware
printf 'loader=rpi\ndevicetree=rockchip/rk3566-x.dtb\noverlays=rockchip/overlay/spi.dtbo\n' >"$R/etc/cleanlinux/boot.d/90-local.conf"
clb kernel sync --esp "$E" >/dev/null 2>&1
C=$E/config.txt
check "rpi: os_prefix of the default kernel" grep -qx 'os_prefix=cleanlinux/7.10.0-b/' "$C"
check "rpi: kernel" grep -qx 'kernel=Image' "$C"
check "rpi: device tree" grep -qx 'device_tree=rk3566-x.dtb' "$C"
check "rpi: overlay" grep -qx 'dtoverlay=spi' "$C"
check "rpi: device tree and overlays/ next to the kernel" test -f "$E/cleanlinux/7.10.0-b/rk3566-x.dtb" -a -f "$E/cleanlinux/7.10.0-b/overlays/spi.dtbo"
check "rpi: cmdline.txt of each kernel" test "$(cat "$E/cleanlinux/7.3.0-a/cmdline.txt")" = "root=PARTUUID=42 rw"
check "rpi: usercfg.txt for the firmware settings" grep -qx 'include usercfg.txt' "$C"
check "rpi: no systemd-boot entries" test ! -e "$S"
echo '# mine' >"$C.mine" && printf 'loader=extlinux\n' >"$R/etc/cleanlinux/boot.d/90-local.conf"
clb kernel sync --esp "$E" >/dev/null 2>&1
check "rpi: config.txt removed with its loader" test ! -e "$C"
mv "$C.mine" "$C" && printf 'loader=rpi\n' >"$R/etc/cleanlinux/boot.d/90-local.conf"
check "rpi: a config.txt of someone else is left alone" bash -c "! '$BIN' --root '$R' kernel sync --esp '$E' >/dev/null 2>&1 && test \"\$(cat '$C')\" = '# mine'"
printf 'loader=none\n' >"$R/etc/cleanlinux/boot.d/90-local.conf"
clb kernel sync --esp "$E" >/dev/null 2>&1
check "loader=none: the boot partition is not touched" test -f "$E/cleanlinux/7.10.0-b/cmdline.txt"

# ---- Android boot images ----
new_root
K=$R/usr/lib/modules
mkdir -p "$K/7.3.0-a/dtbs/qcom" "$R/etc/kernel" "$R/etc/cleanlinux/boot.d"
head -c 5000 < <(yes kernel) >"$K/7.3.0-a/Image" && echo efi >"$K/7.3.0-a/vmlinuz"
printf 'dtb!' >"$K/7.3.0-a/dtbs/qcom/board.dtb"
echo 'root=PARTLABEL=linux rw' >"$R/etc/kernel/cmdline"
printf '%s\n' loader=android devicetree=qcom/board.dtb android.compress=none android.pagesize=2048 \
	android.os_version=13.0.0 android.os_patch_level=2024-05 >"$R/etc/cleanlinux/boot.d/50-android.conf"
clb kernel sync >/dev/null 2>&1
A=$R/var/lib/cleanlinux/android/7.3.0-a.img
u32() { od -An -tu4 -j "$2" -N4 "$1" | tr -d ' '; }
check "android: an image, no boot partition needed" test -f "$A"
check "android: ANDROID!" test "$(head -c 8 "$A")" = 'ANDROID!'
check "android: header 2, page size, kernel size" test "$(u32 "$A" 40) $(u32 "$A" 36) $(u32 "$A" 8)" = "2 2048 5000"
check "android: the command line" test "$(dd if="$A" bs=1 skip=64 count=24 2>/dev/null | tr -d '\0')" = 'root=PARTLABEL=linux rw'
check "android: the device tree section" test "$(u32 "$A" 1648)" = 4
if command -v python3 >/dev/null; then
	verify_bootimg() { # IMAGE KERNEL DTB OS_VERSION: the layout and id of AOSP mkbootimg
		python3 - "$@" <<'PY'
import hashlib, struct, sys
img = open(sys.argv[1], 'rb').read()
kernel, dtb = open(sys.argv[2], 'rb').read(), open(sys.argv[3], 'rb').read()
ks, ka, rs, ra, ss, sa, tags, page, hv, osv = struct.unpack_from('<10I', img, 8)
hsize = (1632, 1648, 1660)[hv]
if hv >= 1:
    assert struct.unpack_from('<IQI', img, 1632) == (0, 0, hsize)
ds = struct.unpack_from('<I', img, 1648)[0] if hv == 2 else 0
pad = lambda n: (n + page - 1) // page * page
off = pad(hsize)
k = img[off:off + ks]; off += pad(ks)
r = img[off:off + rs]; off += pad(rs)
d = img[off:off + ds]; off += pad(ds)
assert len(img) == off, 'size'
assert k == (kernel if hv == 2 else kernel + dtb), 'kernel'
assert hv < 2 or d == dtb, 'dtb'
sha = hashlib.sha1()
for blob in [k, r, b''] + ([b''] if hv > 0 else []) + ([d] if hv > 1 else []):
    sha.update(blob)
    sha.update(struct.pack('<I', len(blob)))
assert img[576:608] == sha.digest() + bytes(12), 'id'
assert osv == int(sys.argv[4]), 'os version'
assert (ka, ra, sa, tags) == (0x10008000, 0x11000000 if rs else 0, 0, 0x10000100), 'addresses'
PY
	}
	check "android: the layout and id of mkbootimg (header 2)" verify_bootimg "$A" "$K/7.3.0-a/Image" "$K/7.3.0-a/dtbs/qcom/board.dtb" $(((13 << 14 << 11) | (24 << 4 | 5)))
	printf 'android.header=0\n' >"$R/etc/cleanlinux/boot.d/60-v0.conf"
	clb kernel sync >/dev/null 2>&1
	check "android: header 0, the device tree appended to the kernel" verify_bootimg "$A" "$K/7.3.0-a/Image" "$K/7.3.0-a/dtbs/qcom/board.dtb" $(((13 << 14 << 11) | (24 << 4 | 5)))
	rm "$R/etc/cleanlinux/boot.d/60-v0.conf"
fi
if command -v gzip >/dev/null; then
	printf 'android.compress=gzip\n' >"$R/etc/cleanlinux/boot.d/60-gzip.conf"
	clb kernel sync >/dev/null 2>&1
	check "android: the kernel gzip-ed" test "$(od -An -tx1 -j 2048 -N2 "$A" | tr -d ' ')" = 1f8b
	rm "$R/etc/cleanlinux/boot.d/60-gzip.conf"
	clb kernel sync >/dev/null 2>&1
fi
P=$TMP/boot_a
head -c 1048576 /dev/zero >"$P"
printf 'android.partition=%s\n' "$P" >"$R/etc/cleanlinux/boot.d/70-flash.conf"
clb kernel sync >/dev/null 2>&1
check "android: not flashed with --root" test -z "$(head -c 8 "$P" | tr -d '\0')"
clb kernel sync --flash >/dev/null 2>&1
check "android: flashed with --flash" cmp -s -n "$(stat -c %s "$A")" "$A" "$P"
check "android: the former partition kept" cmp -s "$R/var/lib/cleanlinux/android/boot_a.orig" <(head -c 1048576 /dev/zero)
check "android: flashed only when it changed" bash -c "! '$BIN' --root '$R' kernel sync --flash 2>&1 | grep -q flashing"
printf 'XXXXXXXX' | dd of="$P" conv=notrunc 2>/dev/null
check "android: a partition without a boot image is not written" bash -c "! '$BIN' --root '$R' kernel sync --flash >/dev/null 2>&1 && test \"\$(head -c 8 '$P')\" = XXXXXXXX"
head -c 4096 /dev/zero >"$P"
check "android: an image larger than the partition is not written" bash -c "! '$BIN' --root '$R' kernel sync --flash >/dev/null 2>&1"
mkdir -p "$K/7.4.0-c" && cp "$K/7.3.0-a/Image" "$K/7.3.0-a/vmlinuz" "$K/7.4.0-c/" && mkdir -p "$K/7.4.0-c/dtbs" && cp -r "$K/7.3.0-a/dtbs/qcom" "$K/7.4.0-c/dtbs/"
rm -rf "$K/7.3.0-a" "$R/etc/cleanlinux/boot.d/70-flash.conf"
clb kernel sync >/dev/null 2>&1
check "android: images of removed kernels go, the backup stays" test ! -e "$A" -a -f "$R/var/lib/cleanlinux/android/7.4.0-c.img" -a -f "$R/var/lib/cleanlinux/android/boot_a.orig"

# ---- initramfs ----
INIT=$(dirname "$BIN")/cleanlinux-init
if [[ -x $INIT ]] && command -v cpio >/dev/null && command -v zstd >/dev/null; then
	new_root
	mkdir -p "$R/usr/lib/cleanlinux-boot" "$R/etc/cleanlinux/initramfs.d"
	cp "$INIT" "$R/usr/lib/cleanlinux-boot/init"
	# the libraries of the host stand in for the root's
	for lib in $(ldd "$INIT" | awk '/=>/ { print $3 } /ld-linux/ { print $1 }'); do cp -L "$lib" "$R/usr/lib/"; done
	echo "enable=yes" >"$R/etc/cleanlinux/initramfs.d/test.conf"
	check "initramfs: wanted" bash -c "'$BIN' --root '$R' initramfs 1.0 -o '$TMP/i.img' >/dev/null 2>&1"
	list=$(zstd -dc "$TMP/i.img" | cpio -it 2>/dev/null)
	check "initramfs: /init" grep -qx init <<<"$list"
	check "initramfs: /dev/console" grep -qx dev/console <<<"$list"
	check "initramfs: libraries of init" grep -q '^usr/lib/libkmod' <<<"$list"
	check "initramfs: the loader" grep -q '^lib64/ld-linux\|^usr/lib/ld-linux\|^lib/ld-linux' <<<"$list"
	SOURCE_DATE_EPOCH=1 "$BIN" --root "$R" initramfs 1.0 -o "$TMP/a.img" >/dev/null 2>&1
	SOURCE_DATE_EPOCH=1 "$BIN" --root "$R" initramfs 1.0 -o "$TMP/b.img" >/dev/null 2>&1
	check "initramfs: reproducible" cmp -s "$TMP/a.img" "$TMP/b.img"
	# optional= modules: packed when the kernel has them, never an error, wanting no initramfs
	mkdir -p "$R/usr/lib/modules/1.0/kernel" "$R/esp" && ln -s usr/lib "$R/lib"
	echo kernel >"$R/usr/lib/modules/1.0/vmlinuz"
	printf 'optional=no-such-panel\n' >"$R/etc/cleanlinux/initramfs.d/test.conf"
	clb kernel sync --esp "$R/esp" >/dev/null 2>&1
	check "initramfs: optional= alone wants none" test ! -e "$R/var/lib/cleanlinux/initramfs/1.0.img"
	printf 'enable=yes\noptional=no-such-panel\n' >"$R/etc/cleanlinux/initramfs.d/test.conf"
	check "initramfs: a missing optional= module is no error" bash -c "'$BIN' --root '$R' initramfs 1.0 -o '$TMP/o.img' >/dev/null 2>&1"
	printf 'modules=no-such-driver\n' >"$R/etc/cleanlinux/initramfs.d/test.conf"
	check "initramfs: a missing modules= module is an error" bash -c "! '$BIN' --root '$R' initramfs 1.0 -o '$TMP/m.img' >/dev/null 2>&1"
	if command -v depmod >/dev/null && command -v cc >/dev/null; then
		# an object with a .modinfo is a module as far as depmod is concerned
		printf 'static const char __attribute__((section(".modinfo"), used, aligned(1))) m%d[] = "%s";\n' \
			1 license=MIT 2 'alias=of:N*T*Cvendor,panel' >"$TMP/panel.c"
		cc -c "$TMP/panel.c" -o "$R/usr/lib/modules/1.0/kernel/panel-vendor.ko"
		depmod -b "$R" 1.0 2>/dev/null
		printf 'enable=yes\noptional=panel-vendor no-such-panel\n' >"$R/etc/cleanlinux/initramfs.d/test.conf"
		"$BIN" --root "$R" initramfs 1.0 -o "$TMP/p.img" >/dev/null 2>&1
		list=$(zstd -dc "$TMP/p.img" | cpio -it 2>/dev/null)
		check "initramfs: an optional= module the kernel has is packed" grep -qx 'usr/lib/modules/1.0/kernel/panel-vendor.ko' <<<"$list"
		check "initramfs: with an index for coldplug" grep -qx 'usr/lib/modules/1.0/modules.alias.bin' <<<"$list"
		check "initramfs: optional= modules are not in the load list" bash -c "! zstd -dc '$TMP/p.img' | cpio -i --to-stdout etc/initramfs/modules 2>/dev/null | grep -q panel"
	fi
fi

# ---- zram ----
new_root
printf 'MemTotal:        3902000 kB\nMemFree:          100000 kB\n' >"$R/proc/meminfo"
printf 'Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n' >"$R/proc/swaps"
check "zram: the RAM, at most 8 GiB, zstd, priority 100" test "$(clb zram --print | head -4 | tr '\n' ' ')" = "ram 3810 MiB zram 3810 MiB algorithm zstd priority 100 "
printf 'MemTotal:       32000000 kB\n' >"$R/proc/meminfo"
check "zram: capped at max" test "$(clb zram --print | sed -n 2p)" = "zram 8192 MiB"
mkdir -p "$R/usr/lib/cleanlinux/zram.d" "$R/etc/cleanlinux/zram.d"
printf 'size = 50%%\nmax = 0\nalgorithm = lz4\n' >"$R/usr/lib/cleanlinux/zram.d/50-profile.conf"
check "zram: zram.d percentage, no limit, algorithm" test "$(clb zram --print | sed -n 2,3p | tr '\n' ' ')" = "zram 15625 MiB algorithm lz4 "
printf 'size=2048\n' >"$R/etc/cleanlinux/zram.d/90-local.conf"
check "zram: /etc wins, size in MiB" test "$(clb zram --print | sed -n 2p)" = "zram 2048 MiB"
printf 'size=off\n' >"$R/etc/cleanlinux/zram.d/90-local.conf"
check "zram: off" test "$(clb zram --print | sed -n 2p)" = "zram 0 MiB"
check "zram start: off is not an error" clb zram start
rm "$R/etc/cleanlinux/zram.d/90-local.conf"
printf '/dev/nvme0n1p3\tpartition\t8388604\t0\t-2\n' >>"$R/proc/swaps"
check "zram: a disk swap is on" test "$(clb zram --print | tail -1 | cut -d' ' -f2)" = yes
check "zram start: nothing next to a disk swap" clb zram start
CLEANLINUX_CMDLINE='quiet cleanlinux.zram=1024' check "zram: cleanlinux.zram= wins" test "$(CLEANLINUX_CMDLINE='quiet cleanlinux.zram=1024' clb zram --print | sed -n 2p)" = "zram 1024 MiB"

# ---- backlight ----
new_root
mkdir -p "$R/sys/class/backlight/amdgpu_bl0" "$R/sys/class/backlight/acpi_video0" "$R/var/lib"
printf '200\n' >"$R/sys/class/backlight/amdgpu_bl0/brightness" && printf '255\n' >"$R/sys/class/backlight/amdgpu_bl0/max_brightness"
printf '3\n' >"$R/sys/class/backlight/acpi_video0/brightness" && printf '100\n' >"$R/sys/class/backlight/acpi_video0/max_brightness"
clb backlight save
check "backlight save: one file per device" test "$(cat "$R/var/lib/cleanlinux/backlight/amdgpu_bl0")" = 200
printf '0\n' >"$R/sys/class/backlight/amdgpu_bl0/brightness" && printf '50\n' >"$R/sys/class/backlight/acpi_video0/brightness"
clb backlight load
check "backlight load: restored" test "$(cat "$R/sys/class/backlight/amdgpu_bl0/brightness")" = 200
check "backlight load: never darker than 5%" test "$(cat "$R/sys/class/backlight/acpi_video0/brightness")" = 5
rm -r "$R/sys/class/backlight/acpi_video0"
check "backlight load: a device without a saved value is left alone" clb backlight load

# ---- power ----
new_root
P=$R/sys/devices/system/cpu/cpufreq
mkdir -p "$P/policy0" "$P/policy4" "$R/sys/firmware/acpi" "$R/sys/class/power_supply/BAT0" "$R/sys/class/power_supply/AC" \
	"$R/sys/class/firmware-attributes/asus-armoury/attributes/ppt_pl1_spl" "$R/sys/class/powercap/intel-rapl:0" \
	"$R/usr/lib/cleanlinux/power.d" "$R/etc/cleanlinux/power.d"
for f in "$P"/policy{0,4}/{scaling_governor,energy_performance_preference} "$R/sys/firmware/acpi/platform_profile" \
	"$R/sys/class/power_supply/BAT0/charge_control_end_threshold" \
	"$R/sys/class/firmware-attributes/asus-armoury/attributes/ppt_pl1_spl/current_value" \
	"$R/sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw"; do : >"$f"; done
check "power: nothing configured, nothing done" clb power
printf 'platform_profile=balanced\nepp=balance_power\ncharge_limit=90\n' >"$R/usr/lib/cleanlinux/power.d/50-profile.conf"
printf '# local\ncharge_limit = 80\nfirmware.ppt_pl1_spl=15\nrapl_long=12.5\n' >"$R/etc/cleanlinux/power.d/90-local.conf"
clb power
check "power: platform_profile" test "$(cat "$R/sys/firmware/acpi/platform_profile")" = balanced
check "power: epp on every policy" test "$(cat "$P/policy0/energy_performance_preference")-$(cat "$P/policy4/energy_performance_preference")" = "balance_power-balance_power"
check "power: /etc wins (charge limit)" test "$(cat "$R/sys/class/power_supply/BAT0/charge_control_end_threshold")" = 80
check "power: firmware attributes" test "$(cat "$R/sys/class/firmware-attributes/asus-armoury/attributes/ppt_pl1_spl/current_value")" = 15
check "power: RAPL in microwatts" test "$(cat "$R/sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw")" = 12500000
check "power: the governor is left alone unless set" test -z "$(cat "$P/policy0/scaling_governor")"
check "power: missing hardware is not an error" bash -c "printf 'charge_start=60\n' >>'$R/etc/cleanlinux/power.d/90-local.conf' && '$BIN' --root '$R' power"
printf 'turbo=yes\n' >"$R/etc/cleanlinux/power.d/95-bad.conf"
check "power: an unknown setting fails" bash -c "! '$BIN' --root '$R' power 2>/dev/null"
check "power --print" test "$(clb power --print 2>/dev/null | head -2 | tr '\n' ' ')" = "platform_profile=balanced epp=balance_power "

# ---- the services ----
SVC=$(dirname "$BIN")/services
DINIT_CHECK=$(command -v dinit-check || true)
services_ok() { # the graph loads: no unknown services, cycles or bad settings
	local out
	out=$("$DINIT_CHECK" -d "$SVC" boot recovery 2>&1 |
		grep -v 'could not stat \(stop \)\{0,1\}command executable\|boot.d: No such file\|One or more errors' |
		grep -i 'warning\|error\|unable\|cycl' || true)
	[[ -z $out ]] || { printf '%s\n' "$out" >&2 && return 1; }
}
if [[ -n $DINIT_CHECK && -d $SVC ]]; then
	check "services: dinit-check" services_ok
fi

printf '1..%d\n' "$count"
((fails == 0)) || { echo "$fails failed" && exit 1; }
