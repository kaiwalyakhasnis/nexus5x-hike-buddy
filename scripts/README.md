# scripts/ — build and development helpers (run on the PC)

All builds are out-of-tree and write only to `out/`; nothing here touches
the LineageOS tree or flashes the phone. The build scripts read machine-specific
settings from `local.env` at the repo root (not committed): `BUSYBOX`
(static aarch64 busybox, required) and optionally `LINEAGE`, `KCROSS` or
`CROSS` for the compilers (see `toolchain.sh`).

| Script | What it does |
|---|---|
| `fetch-toolchains.sh` | Clones AOSP's GCC 4.9 for aarch64 (branch `oreo-mr1-release`, ~90 MB) into `out/toolchains/`: the kernel compiler. |
| `toolchain.sh` | Sourced by the build scripts: the userspace cross compiler (`CROSS`, default the distro's `aarch64-linux-gnu-`) and the kernel GCC 4.9 (`KCROSS`; from `out/toolchains/` or a LineageOS/AOSP 8.1 tree with `LINEAGE=`). |
| `build-kernel.sh` | Builds the kernel from `kernel/` (branch `hike-buddy`) into `out/kobj` with GCC 4.9. Merges `config/hike-buddy.config` onto `bullhead_defconfig` when the fragment changes. `-j4` by default (`JOBS=` to change). `build-kernel.sh menuconfig` edits the config. |
| `build-bootimg.sh` | Builds `hike-buddy` and the tools, downloads today's XTRA file (once a day; `HIKE_BUDDY_XTRA=0` leaves it out), packs `rootfs/` + binaries + map into the ramdisk and writes `out/hike-buddy-boot.img`. |
| `mkbootimg.py` | Minimal Android boot image (v0) writer used by `build-bootimg.sh`. |
| `phone.py` | Runs one shell command on the phone over the dev USB telnet: `scripts/phone.py 'dmesg | tail'` (`T=` sets the timeout). |
| `set-time.sh` | Copies the PC's clock to the phone (lost on reboot; the GPS also sets it on its first fix). |
| `screencap.py` | Screenshots and GIFs straight from the phone's framebuffer, streamed over USB; can inject taps, drags and key presses to script a demo. `screencap.py shot out.png`, `screencap.py demo out.gif` (needs Pillow). Turn on demo mode first so the real position isn't shown. |
| `power-report.sh` | Prints the battery report and per-mode power log from the phone (`/tmp`, read before rebooting). |

## Typical loop

    scripts/build-bootimg.sh
    adb reboot bootloader          # or: scripts/phone.py '/bin/hike-buddy-reboot bootloader'
    fastboot boot out/hike-buddy-boot.img   # RAM only, nothing flashed
    scripts/set-time.sh
