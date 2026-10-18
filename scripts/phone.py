#!/usr/bin/env python3
"""Run a shell command on the Hike Buddy console over the dev telnet (172.16.42.1).
Usage: scripts/phone.py 'dmesg | tail'   (raw socket, answers no telnet options)"""
import socket, sys, time

cmd = " ".join(sys.argv[1:]) or "uname -a"
s = socket.create_connection(("172.16.42.1", 23), timeout=10)
END = "__HIKE_BUDDY_END__"
s.sendall(f"stty -echo; {cmd}; echo __HIKE_BUDDY''_END__\n".encode())  # echoed text never contains END
buf = b""
deadline = time.time() + float(__import__("os").environ.get("T", "20"))
while time.time() < deadline:
    try:
        d = s.recv(65536)
    except socket.timeout:
        break
    if not d:
        break
    # drop telnet IAC negotiation (0xff xx yy)
    while b"\xff" in d:
        i = d.index(b"\xff"); d = d[:i] + d[i + 3:]
    buf += d
    if END.encode() in buf:
        break
out = buf.decode(errors="replace").replace("\r", "")
out = out.split(END, 1)[0]
out = out.split("_END__", 1)[-1]  # drop the echoed command line
print(out.lstrip("\n"), end="")
s.close()
