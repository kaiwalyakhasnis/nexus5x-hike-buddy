#!/bin/sh
# DEV ONLY: USB RNDIS + telnet shell at 172.16.42.1 (host gets .2 by DHCP).
# Autosleep would drop the USB link, so dev boots hold a "usbdev" wakelock.
# For a battery test: echo usbdev > /sys/power/wake_unlock, then unplug.
# Costs nothing on battery: the gadget only draws power with a cable in,
# and then the charger holds the system awake anyway.
G=/sys/class/android_usb/android0
[ -d $G ] || exit 0
echo usbdev > /sys/power/wake_lock
echo 0     > $G/enable
echo 18d1  > $G/idVendor
echo 4ee3  > $G/idProduct
echo hike-buddy > $G/iProduct
echo rndis > $G/functions
echo 1     > $G/enable
sleep 1
ifconfig rndis0 172.16.42.1 netmask 255.255.255.0 up
udhcpd /etc/udhcpd.conf
telnetd -l /bin/sh
