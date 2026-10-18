#!/usr/bin/env python3
"""Screen capture from the phone over the dev USB link (needs Pillow).

Frames are read straight from the framebuffer (/dev/fb0, the half that is
showing) and streamed to the PC with nc. Input can be injected into the
phone's evdev nodes, so a demo can be scripted.

  scripts/screencap.py shot out/screen.png         # one screenshot
  scripts/screencap.py demo out/demo.gif           # scripted tour -> GIF

Run the demo with demo mode on (no real GPS position on screen):
  scripts/phone.py 'touch /tmp/hike-buddy-demo; killall hike-buddy'
"""
import base64, os, socket, struct, subprocess, sys, threading, time, zlib

from PIL import Image

W, H, STRIDE = 1080, 1920, 4352  # /sys/class/graphics/fb0: stride is 1088 px
FRAME = STRIDE * H
HOST, PORT = "172.16.42.2", 5555
EV_TOUCH, EV_PON, EV_KEYS = "/dev/input/event0", "/dev/input/event1", "/dev/input/event2"
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
KEY_VOLUMEDOWN, KEY_VOLUMEUP, KEY_POWER = 114, 115, 116
ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID = 0x2F, 0x35, 0x36, 0x39
HERE = os.path.dirname(os.path.abspath(__file__))


def phone(cmd, timeout=15):
    env = dict(os.environ, T=str(timeout))
    return subprocess.run([sys.executable, os.path.join(HERE, "phone.py"), cmd], env=env,
                          capture_output=True, text=True).stdout


def put(path, data):
    """Writes a file on the phone in small base64 pieces (long telnet lines get mangled)."""
    b64 = base64.b64encode(data).decode()
    phone(f"rm -f {path}.b64")
    for i in range(0, len(b64), 400):
        phone(f"echo -n {b64[i:i + 400]} >> {path}.b64")
    phone(f"base64 -d {path}.b64 > {path}; rm {path}.b64")


def ev(type_, code, value):
    return struct.pack("<qqHHi", 0, 0, type_, code, value)  # struct input_event, arm64


def inject(node, events):
    data = b"".join(events)
    if len(data) <= 480:  # short enough for one telnet line
        phone(f"echo {base64.b64encode(data).decode()} | base64 -d > {node}")
    else:
        put("/tmp/ev.bin", data)
        phone(f"cat /tmp/ev.bin > {node}")


def key(code, node=EV_KEYS):
    inject(node, [ev(EV_KEY, code, 1), ev(EV_SYN, 0, 0), ev(EV_KEY, code, 0), ev(EV_SYN, 0, 0)])


def touch(points, tid=1):
    """A finger down at points[0], through the others, up at the last.
    The input core drops a coordinate equal to the slot's previous one, so
    every touch starts a couple of pixels off its first point."""
    points = [(points[0][0] + 2, points[0][1] + 2)] + list(points)
    evs = [ev(EV_ABS, ABS_MT_SLOT, 0), ev(EV_ABS, ABS_MT_TRACKING_ID, tid)]
    for x, y in points:
        evs += [ev(EV_ABS, ABS_MT_POSITION_X, x), ev(EV_ABS, ABS_MT_POSITION_Y, y), ev(EV_SYN, 0, 0)]
    evs += [ev(EV_ABS, ABS_MT_TRACKING_ID, -1), ev(EV_SYN, 0, 0)]
    inject(EV_TOUCH, evs)


def tap(x, y):
    touch([(x, y), (x, y)])


def wake():
    """Power key if the screen is off (backlight 0); hike-buddy wakes on its last page."""
    out = phone("cat /sys/class/leds/lcd-backlight/brightness")
    vals = [l.strip() for l in out.splitlines() if l.strip().isdigit()]
    if vals and vals[-1] == "0":
        key(KEY_POWER, EV_PON)
        time.sleep(1.5)


def drag(x0, y0, x1, y1, steps=8):
    touch([(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps) for i in range(steps + 1)])


class Grabber:
    """Streams frames from the phone until stop(); keeps (time, image) pairs."""

    def __init__(self):
        self.frames, self.done = [], threading.Event()
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("0.0.0.0", PORT))
        self.srv.listen(1)
        threading.Thread(target=self._recv, daemon=True).start()
        # Each frame: a 16-byte header "HBF <uptime>", then the pixels.
        # USB networking moves ~2.5 MB/s and a raw frame is 8 MB: compress on
        # the phone (frames are mostly black), with the CPU at full speed
        # while capturing; stop() puts the powersave governor back.
        loop = ("while [ ! -f /tmp/cap.stop ]; do printf 'HBF %-12s' $(cut -d' ' -f1 /proc/uptime); "
                "y=$(cut -d, -f2 /sys/class/graphics/fb0/pan); "
                f"dd if=/dev/fb0 bs={STRIDE} skip=$y count={H} iflag=fullblock 2>/dev/null; done | gzip -1 | nc {HOST} {PORT}")
        # Detached, so it outlives the telnet session that starts it.
        put("/tmp/cap.sh", loop.encode())
        phone("rm -f /tmp/cap.stop; echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")
        phone("nohup sh /tmp/cap.sh >/tmp/cap.log 2>&1 & echo started")  # phone.py appends "; echo": "&;" is a syntax error

    def _recv(self):
        conn, _ = self.srv.accept()
        buf = bytearray()
        z = zlib.decompressobj(31)
        while True:
            chunk = conn.recv(1 << 20)
            if not chunk:
                break
            buf += z.decompress(chunk)
            while len(buf) >= 16 + FRAME:
                if buf[:4] != b"HBF ":  # a short frame: skip to the next header
                    i = buf.find(b"HBF ", 1)
                    del buf[:i if i > 0 else len(buf)]
                    continue
                t = float(buf[4:16])
                img = Image.frombuffer("RGBX", (STRIDE // 4, H), bytes(buf[16:16 + FRAME]), "raw", "RGBX", 0, 1)
                self.frames.append((t, img.crop((0, 0, W, H)).convert("RGB")))
                del buf[:16 + FRAME]
        self.done.set()

    def stop(self):
        phone("touch /tmp/cap.stop")
        self.done.wait(120)  # frames still queued on the phone drain first
        phone("echo powersave > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")
        self.srv.close()
        return self.frames


def shot(path):
    wake()
    g = Grabber()
    while not g.frames:
        time.sleep(0.2)
    g.stop()[0][1].save(path)
    print(path)


def demo(path):
    g = Grabber()                       # setting up takes a while: start it first,
    phone("killall hike-buddy")         # then restart hike-buddy (screen on, main page)
    time.sleep(7)                       # main page: altitude, compass, steps
    tap(W - 70, 1850)                   # -> map
    time.sleep(3)
    drag(700, 1100, 350, 900)           # pan
    time.sleep(2.5)
    tap(W - 70, 1850)                   # zoom out
    time.sleep(2.5)
    tap(W - 70, 1850)
    time.sleep(2.5)
    tap(W - 70, 1722)                   # zoom in
    time.sleep(2.5)
    tap(70, 1850)                       # back to the main page
    time.sleep(3)
    frames = g.stop()
    def dark(img):  # screen off, or hike-buddy restarting
        return max(img.convert("L").resize((54, 96)).getdata()) < 40
    while frames and dark(frames[0][1]):
        frames.pop(0)
    while frames and dark(frames[-1][1]):
        frames.pop()
    print(f"{len(frames)} frames in {frames[-1][0] - frames[0][0]:.1f} s")
    # Drop repeats, scale down, keep real timing.
    out, durs, last = [], [], None
    for i, (t, img) in enumerate(frames):
        small = img.resize((360, 640), Image.LANCZOS)
        if last is not None and small.tobytes() == last.tobytes():
            durs[-1] += int((t - frames[i - 1][0]) * 1000)
            continue
        out.append(small)
        durs.append(int((frames[i + 1][0] - t) * 1000) if i + 1 < len(frames) else 1500)
        last = small
    out[0].save(path, save_all=True, append_images=out[1:], duration=durs, loop=0, optimize=True)
    print(path, len(out), "frames")


if __name__ == "__main__":
    cmd, path = sys.argv[1], sys.argv[2]
    {"shot": shot, "demo": demo}[cmd](path)
