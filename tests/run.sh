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
fi

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
