#!/bin/sh
# Copies the PC's clock (UTC) to the phone over the USB telnet link (dev only).
# hike-buddy keeps the time from then on; it is lost on reboot.
cd "$(dirname "$0")/.." && T=10 exec python3 scripts/phone.py "date -u -s @$(date +%s) && date"
