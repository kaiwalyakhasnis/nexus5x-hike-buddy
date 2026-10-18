// Barometric altitude with a per-minute history of the last 3 hours.
// Implemented in Rust (src/altitude.rs); this is the C++ face of it.
#pragma once

#include <stdint.h>

struct HbAltitude;
extern "C" {
HbAltitude* hb_alt_new();
void hb_alt_free(HbAltitude* p);
void hb_alt_add_pressure(HbAltitude* p, double t_boot_s, float hpa);
void hb_alt_calibrate(HbAltitude* p, float gps_msl_m, float vert_unc_m);
bool hb_alt_calibrated(const HbAltitude* p);
bool hb_alt_valid(const HbAltitude* p);
float hb_alt_live_m(const HbAltitude* p);
bool hb_alt_minute(const HbAltitude* p, int ago, float* m);
void hb_alt_totals(const HbAltitude* p, float* up_m, float* down_m);
}

class Altitude {
 public:
  static const int kMinutes = 180;

  Altitude() : p_(hb_alt_new()) {}
  ~Altitude() { hb_alt_free(p_); }
  Altitude(const Altitude&) = delete;
  Altitude& operator=(const Altitude&) = delete;

  // One barometer sample (hPa) taken at t_boot_s (CLOCK_BOOTTIME seconds).
  void add_pressure(double t_boot_s, float hpa) { hb_alt_add_pressure(p_, t_boot_s, hpa); }

  // GPS altitude above sea level with its vertical uncertainty: pulls the
  // barometric reading onto it. The whole history shifts together, so
  // gain/loss over the window is unaffected.
  void calibrate(float gps_msl_m, float vert_unc_m) { hb_alt_calibrate(p_, gps_msl_m, vert_unc_m); }
  bool calibrated() const { return hb_alt_calibrated(p_); }

  bool valid() const { return hb_alt_valid(p_); }
  float live_m() const { return hb_alt_live_m(p_); }

  // Altitude of the minute `ago` minutes before the current one; false if no data.
  bool minute(int ago, float* m) const { return hb_alt_minute(p_, ago, m); }

  // Total climb / descent over the window, ignoring wiggles under the
  // hysteresis so sensor noise doesn't pile up as fake ascent.
  void totals(float* up_m, float* down_m) const { hb_alt_totals(p_, up_m, down_m); }

 private:
  HbAltitude* p_;
};
