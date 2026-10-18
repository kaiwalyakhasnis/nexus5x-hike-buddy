# tools/ — small helpers

| File | Runs on | What it does |
|---|---|---|
| `hike-buddy-reboot.c` | phone | `hike-buddy-reboot bootloader` reboots with an argument (busybox `reboot` can't pass one); the bootloader reads it, e.g. `bootloader` → fastboot mode. |
| `hike-buddy-props.c` | phone | `LD_PRELOAD` shim (`/lib/libhikebuddyprops.so`) for Qualcomm vendor daemons run without Android's property service: `rmt_storage` refuses to start unless `ro.baseband` reads `msm`. |
| `map/` | PC | Builds the offline map file; see its README. |

Both are built by `scripts/build-bootimg.sh` with the distro's aarch64 GCC:
`hike-buddy-reboot` as a static glibc binary, the props shim freestanding
(`-nostdlib`, no libc of its own) because it is loaded into `rmt_storage`,
an Android binary; Android's loader takes it as is.
