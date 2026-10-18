# Sourced by the build scripts: picks the compilers and sets
#   CROSS  prefix of a stock aarch64 Linux cross compiler (glibc), for
#          hike-buddy and the tools (Debian/Ubuntu: g++-aarch64-linux-gnu)
#   KGCC   directory of a GCC 4.9 for the 3.10 kernel (newer GCCs don't
#          build it): AOSP's aarch64-linux-android-4.9, fetched into
#          out/toolchains/ by scripts/fetch-toolchains.sh, or the copy in a
#          LineageOS 15.1 / AOSP 8.1 tree (LINEAGE=...), which is what this
#          project was developed with. Any aarch64 GCC 4.9 works with KCROSS=.
# Machine-specific settings go in an untracked local.env at the repo root.

[ -f "$P/local.env" ] && . "$P/local.env"

CROSS=${CROSS:-aarch64-linux-gnu-}
if [ -n "${LINEAGE:-}" ]; then
	KGCC=${KGCC:-$LINEAGE/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9}
else
	KGCC=${KGCC:-$P/out/toolchains/aarch64-linux-android-4.9}
fi
KCROSS=${KCROSS:-$KGCC/bin/aarch64-linux-android-}
