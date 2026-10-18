#!/bin/bash
# kernel (out/kobj) + hike-buddy + rootfs/ -> out/hike-buddy-boot.img, for `fastboot boot` only.
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
KOBJ=$P/out/kobj
. "$P/scripts/toolchain.sh"
BUSYBOX=${BUSYBOX:-}          # static aarch64 busybox: init, shell, dev tools
if [ ! -x "$BUSYBOX" ]; then
	echo "set BUSYBOX to a static aarch64 busybox binary (env or local.env)" >&2
	exit 1
fi

if ! command -v "${CROSS}g++" >/dev/null; then
	echo "no ${CROSS}g++: apt install g++-aarch64-linux-gnu (or set CROSS=)" >&2
	exit 1
fi

make -C "$P/hike-buddy" -s CROSS="$CROSS"
mkdir -p "$P/out/tools"
"${CROSS}gcc" -O2 -static -s -o "$P/out/tools/hike-buddy-reboot" "$P/tools/hike-buddy-reboot.c"
# Loaded into Android's rmt_storage: freestanding, no libc of its own.
"${CROSS}gcc" -O2 -fPIC -shared -nostdlib -ffreestanding -fno-builtin -fno-stack-protector -s \
	-Wl,-soname,libhikebuddyprops.so -Wl,--hash-style=both -Wl,-z,max-page-size=4096 \
	-o "$P/out/tools/libhikebuddyprops.so" "$P/tools/hike-buddy-props.c"

# Local time zone as a POSIX TZ string from this PC's offset (no DST rules),
# e.g. JST-9. Override with HIKE_BUDDY_TZ=...
if [ -n "${HIKE_BUDDY_TZ:-}" ]; then
	echo "$HIKE_BUDDY_TZ" > "$P/out/TZ"
else
	off=$(date +%z)   # +0900
	sign=${off:0:1}; hh=$((10#${off:1:2})); mm=${off:3:2}
	[ "$sign" = "+" ] && psign=- || psign=+
	[ "$mm" = "00" ] && echo "$(date +%Z)$psign$hh" > "$P/out/TZ" || echo "$(date +%Z)$psign$hh:$mm" > "$P/out/TZ"
fi

# XTRA predicted orbits (Qualcomm's file, as Android downloads it). This
# modem rejects today's files, so HIKE_BUDDY_XTRA=0 leaves it out (release builds).
XTRA=$P/out/xtra/xtra3gr.bin
mkdir -p "$P/out/xtra"
if [ "${HIKE_BUDDY_XTRA:-1}" = 0 ]; then
	XTRA=
elif [ -z "$(find "$XTRA" -mmin -1440 2>/dev/null)" ]; then
	for n in 1 2 3; do
		curl -sf --max-time 30 -o "$XTRA.new" "http://xtrapath$n.izatcloud.net/xtra3gr.bin" &&
			mv "$XTRA.new" "$XTRA" && break
	done
	[ -f "$XTRA" ] || echo "note: no XTRA file (the modem rejects them anyway)" >&2
fi

LIST=$P/out/initramfs.list
{
	echo "dir /bin 755 0 0"
	echo "dir /sbin 755 0 0"
	echo "dir /usr 755 0 0"
	echo "dir /usr/bin 755 0 0"
	echo "dir /usr/sbin 755 0 0"
	echo "dir /etc 755 0 0"
	for d in proc sys dev tmp; do echo "dir /$d 755 0 0"; done
	echo "nod /dev/console 600 0 0 c 5 1"
	echo "file /init $P/rootfs/init 755 0 0"
	echo "file /bin/busybox $BUSYBOX 755 0 0"
	echo "file /bin/hike-buddy $P/out/hike-buddy/hike-buddy 755 0 0"
	echo "file /bin/hike-buddy-reboot $P/out/tools/hike-buddy-reboot 755 0 0"
	echo "dir /lib 755 0 0"
	echo "file /lib/libhikebuddyprops.so $P/out/tools/libhikebuddyprops.so 644 0 0"
	echo "file /etc/TZ $P/out/TZ 644 0 0"
	# XTRA predicted orbits, the file the modem asks Android for (refreshed daily).
	echo "dir /share 755 0 0"
	if [ -n "$XTRA" ] && [ -f "$XTRA" ]; then
		echo "file /share/xtra3gr.bin $XTRA 644 0 0"
	fi
	# Offline map from tools/map/build_map.py (optional).
	if [ -f "$P/out/map/hike-buddy.map" ]; then
		echo "file /share/hike-buddy.map $P/out/map/hike-buddy.map 644 0 0"
	fi
	for f in "$P"/rootfs/etc/*; do
		echo "file /etc/$(basename "$f") $f $(stat -c %a "$f") 0 0"
	done
} > "$LIST"
"$KOBJ/usr/gen_init_cpio" "$LIST" | gzip -9 > "$P/out/initramfs.cpio.gz"

# Stock bullhead cmdline minus the UART console and Android-only bits.
CMDLINE="boot_cpus=0-5 lpm_levels.sleep_disabled=1 msm_poweroff.download_mode=0"
python3 "$P/scripts/mkbootimg.py" \
	--kernel "$KOBJ/arch/arm64/boot/Image.gz-dtb" \
	--ramdisk "$P/out/initramfs.cpio.gz" \
	--cmdline "$CMDLINE" \
	--base 0x00000000 --pagesize 4096 \
	--ramdisk_offset 0x02000000 --tags_offset 0x01E00000 \
	-o "$P/out/hike-buddy-boot.img"
ls -la "$P/out/hike-buddy-boot.img"
