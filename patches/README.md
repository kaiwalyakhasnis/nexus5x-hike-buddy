# patches/ — kernel changes for publishing

`kernel/` is a separate git clone of LineageOS's bullhead kernel,
<https://github.com/LineageOS/android_kernel_lge_bullhead> (not tracked in
this repo). This folder is for its commits exported as patch
files (`000N-*.patch`), so the kernel changes can be published with the
project. To refresh them after changing the kernel:

    git -C kernel format-patch -o ../patches lineage-15.1..hike-buddy

(`lineage-15.1` = the commit the branch was started from; with a shallow
clone, use that commit's hash.) To apply them to a fresh clone:

    git -C kernel am ../patches/*.patch

The `hike-buddy` branch currently carries three commits on top of
LineageOS 15.1's `lineage-15.1` branch:

1. `dtc: fix yylloc multiple definition with newer host gcc` — lets the
   kernel build with a modern host compiler.
2. `leds: qpnp rgb start sysfs + heartbeat trigger` — the RGB LED work from
   [nexus5x-rgbled](https://github.com/kaiwalyakhasnis/nexus5x-rgbled).
3. `regulator: dw8768: include device.h` — build fix exposed by the config
   fragment.
