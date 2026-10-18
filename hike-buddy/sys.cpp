#include "sys.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int64_t clock_ms(clockid_t id) {
  timespec ts;
  clock_gettime(id, &ts);
  return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

int64_t now_mono_ms() { return clock_ms(CLOCK_MONOTONIC); }
int64_t now_boot_ms() { return clock_ms(CLOCK_BOOTTIME); }

// One-shot sysfs-style read; trailing newline/spaces stripped.
bool read_str(const char* path, char* buf, int len) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  int n = read(fd, buf, len - 1);
  close(fd);
  if (n <= 0) return false;
  buf[n] = 0;
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = 0;
  return true;
}

bool read_long(const char* path, long* out) {
  char buf[32];
  if (!read_str(path, buf, sizeof(buf))) return false;
  *out = strtol(buf, nullptr, 10);
  return true;
}

bool write_str(const char* path, const char* s) {
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  bool ok = write(fd, s, strlen(s)) == (ssize_t)strlen(s);
  close(fd);
  return ok;
}

// Goes to the kernel log so it shows in dmesg and survives in pstore.
void klog(const char* fmt, ...) {
  static int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
  char buf[256];
  int n = snprintf(buf, sizeof(buf), "<6>hike-buddy: ");
  va_list ap;
  va_start(ap, fmt);
  n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
  buf[n++] = '\n';
  if (fd >= 0) {
    if (write(fd, buf, n) < 0) {}  // nowhere to report a failed log write
  }
  else fwrite(buf + 3, 1, n - 3, stderr);
}

void wake_lock() { write_str("/sys/power/wake_lock", "hike-buddy"); }
void wake_unlock() { write_str("/sys/power/wake_unlock", "hike-buddy"); }
void autosleep(bool on) { write_str("/sys/power/autosleep", on ? "mem" : "off"); }

void backlight(int level) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", level);
  write_str("/sys/class/leds/lcd-backlight/brightness", buf);
}

// Snapshot of the fuel gauge (bms) and charger (battery) power_supply nodes.
// "Full" counts as charging: the cable is in either way.
void read_battery(Battery* b) {
  memset(b, 0, sizeof(*b));
  read_long("/sys/class/power_supply/battery/capacity", &b->capacity_pct);
  read_long("/sys/class/power_supply/bms/voltage_now", &b->voltage_uv);
  read_long("/sys/class/power_supply/bms/current_now", &b->current_ua);
  read_long("/sys/class/power_supply/bms/charge_now_raw", &b->charge_raw_uah);
  read_long("/sys/class/power_supply/bms/charge_full", &b->charge_full_uah);
  read_long("/sys/class/power_supply/bms/temp", &b->temp_dc);
  char st[32];
  b->charging = read_str("/sys/class/power_supply/battery/status", st, sizeof(st)) &&
                (!strcmp(st, "Charging") || !strcmp(st, "Full"));
}
