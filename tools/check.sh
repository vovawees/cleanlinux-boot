#!/usr/bin/env bash
# Local checks of cleanlinux-boot: run them before every push (there is no hosted CI).
#   tools/check.sh [--quick]
# build with -Werror and `make check` with gcc and clang (when installed), shellcheck of the scripts,
# `make install` into a temporary DESTDIR. --quick: gcc only (tools/hooks/pre-push).
set -euo pipefail
cd "$(dirname "$0")/.."
QUICK=0
case ${1:-} in
	--quick) QUICK=1 ;;
	-h | --help) sed -n '2,5p' "$0" | cut -c3- && exit 0 ;;
	'') ;;
	*) echo "tools/check.sh: unknown argument: $1" >&2 && exit 2 ;;
esac
step() { printf '\n\033[1;36m==>\033[0m %s\n' "$*"; }
trap 'make -s clean >/dev/null 2>&1; make -s >/dev/null 2>&1 || true' EXIT

ccs=(gcc)
if ((!QUICK)) && command -v clang >/dev/null; then ccs+=(clang); fi
for cc in "${ccs[@]}"; do
	step "build and make check with $cc, warnings are errors"
	make -s clean
	make -s CC="$cc" CFLAGS="-O2 -g -Werror"
	make -s CC="$cc" CFLAGS="-O2 -g -Werror" check
done
step shellcheck
shellcheck tests/run.sh tools/*.sh tools/hooks/pre-push
((QUICK)) && exit 0
step "make install DESTDIR=…"
dest=$(mktemp -d)
make -s install DESTDIR="$dest"
(cd "$dest" && find . -type f -o -type l | sort)
rm -rf "$dest"
