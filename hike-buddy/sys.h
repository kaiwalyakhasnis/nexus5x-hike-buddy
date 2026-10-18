// sysfs, clocks and power-policy helpers for hike-buddy.
#pragma once

#include <stdint.h>

// Clocks. MONOTONIC stops while the SoC is suspended, BOOTTIME does not,
// so (boottime - monotonic) grows by exactly the time spent suspended.
int64_t now_mono_ms();
int64_t now_boot_ms();

bool read_long(const char* path, long* out);
bool read_str(const char* path, char* buf, int len);
bool write_str(const char* path, const char* s);

void klog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Power policy: hike-buddy holds its wakelock while the screen is on; with
// autosleep=mem the kernel suspends as soon as it lets go.
void wake_lock();
void wake_unlock();
void autosleep(bool on);
void backlight(int level);  // 0..255 on the lcd-backlight led class

struct Battery {
  long capacity_pct;   // battery/capacity
  long voltage_uv;     // bms/voltage_now
  long current_ua;     // bms/current_now (fuel gauge sign convention)
  long charge_raw_uah; // bms/charge_now_raw: the FG coulomb counter, counts while suspended
  long charge_full_uah;
  long temp_dc;        // deci-degC
  bool charging;
};
void read_battery(Battery* b);
