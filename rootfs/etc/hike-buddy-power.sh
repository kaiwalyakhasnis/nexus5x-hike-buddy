#!/bin/sh
# Lowest-power baseline. Starting point is Google's own "on charger" block in
# device/lge/bullhead/init.bullhead.rc: one A53 core, powersave governor.
S=/sys/devices/system/cpu
for c in 1 2 3 4 5; do echo 0 > $S/cpu$c/online; done
echo powersave > $S/cpu0/cpufreq/scaling_governor

# Retention stays off like on stock (it is disabled in every bullhead mode).
# Deeper idle/power-collapse (lpm_levels sleep_disabled=0) is enabled by
# hike-buddy-modem.sh only once the modem is up: like stock, which clears it at
# boot_completed. With it on, the MBA stage of the modem boot never finishes.
L=/sys/module/lpm_levels/system
for c in a53/cpu0 a53/cpu1 a53/cpu2 a53/cpu3 a57/cpu4 a57/cpu5; do
	echo 0 > $L/$c/retention/idle_enabled
done
echo 0 > $L/a53/a53-l2-retention/idle_enabled
echo 0 > $L/a57/a57-l2-retention/idle_enabled

echo noop > /sys/block/mmcblk0/queue/scheduler
