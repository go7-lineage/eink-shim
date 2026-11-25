#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); repo=$(cd "$here/../.." && pwd)
KSRC=$(realpath "$1"); KOUT=$(realpath "$2"); SECS=${3:-600}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
Q="nix shell nixpkgs#qemu nixpkgs#dtc nixpkgs#cpio -c"
M=(-M virt -cpu cortex-a76 -smp 2 -m 1G -nographic -no-reboot)

kbuild() {
	local rel
	rel=$(realpath --relative-to="$KSRC" "$1")
	if [[ -n ${KMAKE:-} ]]; then
		KSRC=$KSRC KOUT=$KOUT "$KMAKE" -s M="$rel" MO="$2" W=1 modules
	else
		make -s -C "$KSRC" O="$KOUT" ARCH=arm64 LLVM=1 M="$rel" MO="$2" W=1 modules
	fi
}
kbuild "$repo/kernel" "$T/mo"
kbuild "$here" "$T/mo-test"
cc -std=gnu11 -O1 -I"$repo/kernel/core" -I"$repo/tests" -o "$T/mkwbf" "$here/mkwbf.c" \
	"$repo/tests/fixture.c" "$repo/tests/wbf_build.c"
mcc=$(nix build --no-link --print-out-paths nixpkgs#pkgsStatic.stdenv.cc | grep -v -- '-man$')
"$mcc/bin/aarch64-unknown-linux-musl-gcc" -static -std=gnu11 -O1 -Wall -Wextra -Werror -o "$T/kmsfill" "$here/kmsfill.c"
"$mcc/bin/aarch64-unknown-linux-musl-gcc" -static -std=gnu11 -O1 -Wall -Wextra -Werror -o "$T/hwcseq" "$here/hwcseq.c"

$Q qemu-system-aarch64 "${M[@]}" -machine dumpdtb="$T/virt.dtb" > /dev/null 2>&1
$Q dtc -q -I dtb -O dts "$T/virt.dtb" > "$T/test.dts"
G=$(grep -o -m1 'pl061@[0-9a-f]*' "$T/test.dts")
sed "s|@GPIO@|/$G|g" "$here/test.dtsi" >> "$T/test.dts"
$Q dtc -q -I dts -O dtb "$T/test.dts" > "$T/test.dtb"

root=$T/root
mkdir -p "$root"/{bin,dev,proc,sys,lib/modules,lib/firmware}
bb=$(nix build --no-link --print-out-paths nixpkgs#pkgsStatic.busybox)/bin/busybox
cp "$bb" "$root/bin/busybox"
for a in $("$bb" --list); do ln -sf busybox "$root/bin/$a"; done
cp "$T"/mo/*.ko "$T"/mo-test/*.ko "$root/lib/modules/"
cp "$T/kmsfill" "$root/bin/kmsfill"
cp "$T/hwcseq" "$root/bin/hwcseq"
"$T/mkwbf" "$root/lib/firmware/eink_waveform.wbf"
cp "$here/init.sh" "$root/init"
chmod +x "$root/init"
(cd "$root" && find . | $Q cpio -o -H newc --quiet | gzip) > "$T/initrd.gz"

timeout "$SECS" $Q qemu-system-aarch64 "${M[@]}" -dtb "$T/test.dtb" -kernel "$KOUT/arch/arm64/boot/Image" \
	-initrd "$T/initrd.gz" -append "console=ttyAMA0 rdinit=/init loglevel=7 fp9931:vcom=-1840000" \
	> "$T/console.log" 2>&1 || true
grep -a -E 'epd-test:|WARNING:|BUG:|KASAN|Unable to handle|Oops|possible circular|INFO: task' "$T/console.log" || true
cp "$T/console.log" "${CONSOLE_LOG:-/tmp/epd-qemu-console.log}"
grep -a -q '^epd-test: RESULT ok' "$T/console.log"
