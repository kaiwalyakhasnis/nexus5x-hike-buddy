#!/bin/sh
# Fetches the kernel compiler into out/toolchains/: GCC 4.9 for aarch64 from
# AOSP (branch oreo-mr1-release, the same build LineageOS 15.1 ships). The
# 3.10 kernel does not build with current GCCs. Not needed with LINEAGE=.
# hike-buddy itself uses the distro cross compiler (g++-aarch64-linux-gnu).
set -e
P=$(cd "$(dirname "$0")/.." && pwd)
D=$P/out/toolchains/aarch64-linux-android-4.9
if [ -x "$D/bin/aarch64-linux-android-gcc" ]; then
	echo "$D already there"
	exit 0
fi
mkdir -p "$(dirname "$D")"
git clone --depth 1 -b oreo-mr1-release \
	https://android.googlesource.com/platform/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9 "$D"
echo "kernel GCC in $D"
