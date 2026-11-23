#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
KSRC=$(realpath "$1"); KOUT=$(realpath "$2")
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
rel=$(realpath --relative-to="$KSRC" "$repo/kernel")

kmake() {
	if [[ -n ${KMAKE:-} ]]; then
		KSRC=$KSRC KOUT=$KOUT "$KMAKE" -s M="$rel" "$@"
	else
		make -s -C "$KSRC" O="$KOUT" ARCH=arm64 LLVM=1 M="$rel" "$@"
	fi
}

kmake MO="$T/werror" W=1 KCFLAGS=-Werror modules
sparse=$(kmake MO="$T/sparse" C=2 CHECK=sparse modules 2>&1 | grep -E 'warning:|error:' |
	grep -v -e 'epd_neon\.c' -e 'arm_neon\.h' || true)
if [[ -n $sparse ]]; then
	echo "$sparse"
	exit 1
fi
cd "$repo"
perl "$KSRC/scripts/checkpatch.pl" --terse -f kernel/*.[ch] kernel/core/*.[ch] tests/qemu/*.c
