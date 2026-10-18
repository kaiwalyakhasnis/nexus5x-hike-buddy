#!/bin/sh
# GPS lives in the modem. Boot it the way Android does, minus Android:
#  - the phone's own modem/system/vendor partitions, mounted read-only
#    (ext4 with noload: not even a journal replay writes to them)
#  - firmware loaded by the kernel straight from /lib/firmware (no ueventd)
#  - rmt_storage (from /system) serves the modem's EFS out of the
#    modemst1/modemst2/fsg partitions, as on every Android boot
#  - irsc_util (from /system) loads the IPC router security rules
#  - holding /dev/subsys_modem open boots the modem (PIL) and keeps it up
# hike-buddy then talks QMI to it over the IPC router.
B=/dev/block/bootdevice/by-name
mkdir -p $B /firmware /system /vendor /lib
for p in /sys/block/mmcblk0/mmcblk0p*; do
	n=$(sed -n 's/^PARTNAME=//p' $p/uevent)
	[ -n "$n" ] && ln -sf /dev/${p##*/} $B/$n
done

# Deep CPU sleep goes on once the modem is up (or gave up), see hike-buddy-power.sh.
allow_deep_sleep() {
	echo 0 > /sys/module/lpm_levels/parameters/sleep_disabled
	echo "hike-buddy-modem: deep sleep enabled ($1)" > /dev/kmsg
}

mounted=1
mount -t vfat -o ro,shortname=lower $B/modem /firmware || mounted=0
mount -t ext4 -o ro,noload $B/system /system || mounted=0
mount -t ext4 -o ro,noload $B/vendor /vendor || mounted=0
if [ $mounted = 0 ]; then allow_deep_sleep "no modem partitions"; exit 1; fi
ln -sfn /firmware/image /lib/firmware

# IPC router security rules, as stock init does. Until a rules socket has
# been closed, every IPC router send (our QMI requests) blocks in the kernel.
/system/bin/irsc_util /system/etc/sec_config

LD_PRELOAD=/lib/libhikebuddyprops.so /system/bin/rmt_storage > /dev/kmsg 2>&1 &
sleep 1
sleep 2147483647 < /dev/subsys_modem &
echo "hike-buddy-modem: modem boot requested" > /dev/kmsg

(
	for d in /sys/bus/msm_subsys/devices/*; do
		[ "$(cat $d/name)" = modem ] && S=$d/state
	done
	for i in $(seq 60); do
		[ "$(cat $S)" = ONLINE ] && { allow_deep_sleep "modem online"; exit; }
		sleep 1
	done
	allow_deep_sleep "modem not online after 60 s"
) &
