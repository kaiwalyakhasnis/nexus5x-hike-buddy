# config/ — kernel configuration

`hike-buddy.config` is a config fragment merged on top of the stock
`bullhead_defconfig` by `scripts/build-kernel.sh` (via the kernel's
`merge_config.sh`). The result lives in `out/kobj/.config`.

It mostly **switches things off** that a hiking console doesn't need, so the
kernel is smaller and nothing wakes the phone for no reason:

- Android-only: binder, ashmem, low-memory killer, SELinux, sdcardfs.
- Hardware not used: camera, video codec, GPU (KGSL), audio/ADSP, Wi-Fi,
  Bluetooth, NFC, fingerprint, secure-world client (QSEECOM).
- Networking extras: IPv6, netfilter, bridging, PPP.
- Debugging overhead: SLUB debug, schedstats, timer stats, dynamic debug,
  tracing.

It switches on `DEVTMPFS` (+ automount), so `/dev` populates itself without
Android's `ueventd`.

Some options stay on despite looking unneeded, because something else
depends on them; the fragment's comments say why (e.g. ALSA core for the USB
gadget, IPA for the modem's handshake).
