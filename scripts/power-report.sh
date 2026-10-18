#!/bin/sh
# Prints hike-buddy's battery report and per-mode log from the phone (USB telnet).
# Both live in RAM on the phone: read them before rebooting it.
cd "$(dirname "$0")/.." && T=15 exec python3 scripts/phone.py \
	'cat /tmp/hike-buddy-power.txt 2>/dev/null || echo "no report yet (only written while unplugged)"; echo; echo "--- log ---"; cat /tmp/hike-buddy-power.log 2>/dev/null'
