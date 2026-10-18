#!/bin/sh
# Build the Hike Buddy kernel from ./kernel (branch "hike-buddy") out-of-tree into ./out/kobj,
# with GCC 4.9 (see scripts/toolchain.sh; current GCCs can't build this 3.10 kernel).
# Usage: scripts/build-kernel.sh                 (incremental Image.gz-dtb)
#        scripts/build-kernel.sh menuconfig      (edit out/kobj/.config)
# PC runs hot: default -j4, override with JOBS=n.
set -e
P=$(cd "$(dirname "$0")/.." && pwd)
. "$P/scripts/toolchain.sh"
KOBJ=${KOBJ:-$P/out/kobj}
mkdir -p "$KOBJ"
if [ ! -x "${KCROSS}gcc" ]; then
	echo "no kernel compiler at ${KCROSS}gcc: run scripts/fetch-toolchains.sh (or set LINEAGE=/KCROSS=)" >&2
	exit 1
fi
MK="make -C $P/kernel O=$KOBJ ARCH=arm64 CROSS_COMPILE=$KCROSS"
# bullhead_defconfig + config/hike-buddy.config -> out/kobj/.config (regenerated when the fragment changes)
if [ ! -f "$KOBJ/.config" ] || [ "$P/config/hike-buddy.config" -nt "$KOBJ/.config" ]; then
	$MK bullhead_defconfig
	(cd "$KOBJ" && ARCH=arm64 "$P/kernel/scripts/kconfig/merge_config.sh" -m -O "$KOBJ" .config "$P/config/hike-buddy.config")
	$MK olddefconfig
fi
$MK -j"${JOBS:-4}" "${@:-Image.gz-dtb}"
