# hike-buddy/ — the daemon

The one program that runs on the phone: screen, buttons, touch, sensors,
GPS, map and battery accounting. Started (and restarted if it dies) by
busybox init from `rootfs/etc/inittab` as `/bin/hike-buddy`.

Built as a static aarch64 binary with a stock Linux cross compiler and glibc
(`g++-aarch64-linux-gnu`, see `Makefile`): it uses only plain Linux
interfaces, nothing from Android. `scripts/build-bootimg.sh`
builds it into `out/hike-buddy/hike-buddy` and packs it into the ramdisk.

**Being migrated to Rust**, in place and module by module: `src/` is a Rust
static library (`Cargo.toml`) linked into the same static binary, and C++
calls it through `rust.h`. Each module moves over only once it matches the
C++ one exactly (same outputs, pixel-identical screens, byte-identical GPS
messages) and passes on the phone. minui stays C++.

## Files

| File | What it does |
|---|---|
| `main.cpp` | Event loop, UI pages (main + map), header, key/touch handling, screen on/off, sensor rates per page, GPS duty cycle, clock handling. Start here. |
| `sys.*` | Small helpers: clocks (`CLOCK_MONOTONIC`/`BOOTTIME`), sysfs read/write, kernel log (`klog` → `dmesg`), wakelock, autosleep, backlight, battery/fuel gauge readings. |
| `hub.*` | Client for the STM32 sensor hub (`/dev/nanohub`): enables/batches the barometer, step counter and the hub-fused compass (geomagnetic rotation vector) and parses their samples. |
| `src/altitude.rs` (+ `altitude.h` wrapper) | **Rust.** Barometric altitude: pressure → metres, one value per minute for the last 3 hours, climb/descent totals with hysteresis, calibration from GPS height. |
| `gps.*` | QMI LOC client talking straight to the modem's GNSS engine over the kernel IPC router (`AF_MSM_IPC`). Replays the exact sequence Android's location stack sends (setup, UTC time injection, XTRA injection, MSB start), one request at a time. |
| `map.*` | Offline map: memory-maps `/share/hike-buddy.map` (made by `tools/map/build_map.py`) and rasterises the visible tiles straight into a surface (lines, trails, peaks, labels). |
| `power.*` | Battery accounting while unplugged: books fuel-gauge charge against the current mode (standby / screen / GPS), splits it into parts, and writes `/tmp/hike-buddy-power.log` and `.txt` (read with `scripts/power-report.sh`). |
| `src/draw.rs` (+ `draw.h`) | **Rust.** Drawing primitives on top of minui: thick lines, rings/discs, triangles, polar coordinates. |
| `src/segment.rs` (+ `segment.h`) | **Rust.** Seven-segment digits for the big numbers (altitude, steps). |
| `tests/` | Golden tests: a C++ driver run against the old and the new implementation of a module; outputs must be identical (see each file's header). |
| `minui_c.cpp`, `src/gr.rs` | The `extern "C"` bridge from Rust into minui's C++ drawing calls. |
| `minui/` | Android recovery's minimal graphics/input library, vendored and trimmed; see its README. |
| `preview/` | Host build that renders the screens to images on the PC; see its README. |

## Power model in one paragraph

Screen off: the wakelock is released and the kernel suspends; the hub keeps
counting steps and sampling the barometer every 10 s, batching them for
~5 minutes; GPS runs only for a first fix and a short refresh every 30 min
(woken by a `CLOCK_BOOTTIME_ALARM` timer). Screen on: wakelock held, compass
at 12.5 Hz (main page only), barometer at 1 Hz, GPS at 1 Hz, redraws only
when something visible changed. 15 s without input turns the screen off.

## Demo mode

For screenshots: `touch /tmp/hike-buddy-demo; killall hike-buddy` (init
restarts it). GPS positions are then ignored, so the real location never
appears on screen and the map opens on its default view (Mt. Takao).

## Debugging

    scripts/phone.py 'dmesg | grep hike-buddy'

Every subsystem logs with a prefix: `gps:`, `map:`, `power:`, `hub config`, ...
