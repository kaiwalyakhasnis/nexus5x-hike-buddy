# rootfs/ — what goes into the ramdisk

The whole userspace besides the binaries built elsewhere. `scripts/build-bootimg.sh`
packs these files (plus busybox, `hike-buddy`, the tools, the map and XTRA
data) into the initramfs of `out/hike-buddy-boot.img`.

## Boot order

1. `init` (PID 1, busybox sh): mounts `/proc`, `/sys`, `/dev` (devtmpfs),
   `/tmp` (RAM), devpts, pstore, debugfs; then hands over to busybox init.
2. `etc/inittab` runs, in order:
   1. `etc/hike-buddy-power.sh` — lowest-power baseline: CPU cores 1–5
      offline, CPU0 on the powersave governor, retention idle off.
   2. `etc/hike-buddy-usb.sh` — **dev only**: USB network (RNDIS) with a
      root telnet shell at 172.16.42.1 and a DHCP server for the PC; holds
      a `usbdev` wakelock so the link survives.
   3. `etc/hike-buddy-modem.sh` — mounts the modem firmware, `system` and
      `vendor` partitions **read-only**, starts `rmt_storage` (the modem's
      storage helper) and boots the modem; once the modem is up it allows
      deep CPU sleep (enabling it earlier breaks the modem boot).
3. `/bin/hike-buddy` is started and respawned if it exits.

| File | What it does |
|---|---|
| `init` | PID 1 script described above. |
| `etc/inittab` | busybox init table. |
| `etc/hike-buddy-power.sh` | CPU and idle-state power settings. |
| `etc/hike-buddy-usb.sh` | Dev USB network + telnet (remove for a trail build). |
| `etc/hike-buddy-modem.sh` | Modem firmware, `rmt_storage`, modem boot, deep sleep. |
| `etc/udhcpd.conf` | DHCP server config for the USB network (PC gets .2). |

Files added by the build script (not in this folder): `/bin/busybox`,
`/bin/hike-buddy`, `/bin/hike-buddy-reboot`, `/lib/libhikebuddyprops.so`,
`/etc/TZ` (the PC's time zone), `/share/hike-buddy.map`, `/share/xtra3gr.bin`.
