// GPS from the modem's GNSS engine: a minimal QMI LOC client over the
// kernel IPC router (AF_MSM_IPC), no Qualcomm userspace libraries. It sends
// what Android's location stack sends (setup, time, XTRA, MSB start).
// Message layouts follow libqmi's data/qmi-service-loc.json.
#pragma once

#include <stdint.h>

#include <functional>

struct GpsFix {
  bool valid;  // session status SUCCESS with a position
  uint32_t status;  // QMI LOC session status (0 success, 1 in progress, 2 failure, 3 timeout...)
  double lat, lon;
  float hunc_m;  // horizontal uncertainty (circular)
  bool have_alt;
  float alt_msl_m, vunc_m;
  uint64_t utc_ms;  // 0 if not reported
  int sats_used;
};

struct GpsSky {
  int in_view;  // satellites the engine reports
  int heard;    // ... with a usable signal
};

// Opens the IPC router socket; returns false if the kernel has no AF_MSM_IPC.
bool gps_open();
int gps_fd();

// Finds the modem's LOC (and DMS) services. False while the modem is still
// booting; call again later. Finding DMS also puts the cellular radio into
// low-power mode (GNSS keeps working, like airplane mode); DMS can register
// after LOC, so keep calling until gps_radio_low_power() too.
bool gps_connect();
bool gps_connected();
bool gps_radio_low_power();  // DMS found and the low-power request sent

// XTRA predicted orbits (xtra3gr.bin, as Android downloads it): injected
// whenever the modem asks for it. Load once before gps_connect().
bool gps_load_xtra(const char* path);
// Hands the system clock to the engine, if it has been set (as Android does with NTP time).
void gps_inject_time();

// Periodic fixes every interval_ms until gps_stop().
void gps_start(uint32_t interval_ms);
void gps_stop();
bool gps_running();

using GpsFixCb = std::function<void(const GpsFix&)>;
using GpsSkyCb = std::function<void(const GpsSky&)>;
void gps_read(const GpsFixCb& on_fix, const GpsSkyCb& on_sky);
