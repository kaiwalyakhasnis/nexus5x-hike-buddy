// Barometric altitude with a per-minute history of the last 3 hours.
#pragma once

#include <stdint.h>

class Altitude {
 public:
  static const int kMinutes = 180;

  void add_pressure(double t_boot_s, float hpa);

  // GPS altitude above sea level with its vertical uncertainty: pulls the
  // barometric reading onto it. The whole history shifts together, so
  // gain/loss over the window is unaffected.
  void calibrate(float gps_msl_m, float vert_unc_m);
  bool calibrated() const { return calibrated_; }

  bool valid() const { return have_live_; }
  float live_m() const { return live_raw_m_ + offset_m_; }

  // Altitude of the minute `ago` minutes before the current one; false if no data.
  bool minute(int ago, float* m) const;

  // Total climb / descent over the window, ignoring wiggles under the
  // hysteresis so sensor noise doesn't pile up as fake ascent.
  void totals(float* up_m, float* down_m) const;

 private:
  void close_minute();

  int64_t cur_min_ = -1;
  double sum_hpa_ = 0;
  int count_ = 0;

  float alt_[kMinutes];    // uncalibrated (standard-atmosphere) altitude
  int64_t tag_[kMinutes];  // which minute alt_[i] belongs to; slot = minute % kMinutes
  bool init_ = false;

  bool have_live_ = false;
  float live_raw_m_ = 0;

  bool calibrated_ = false;
  float offset_m_ = 0;
};

float pressure_to_altitude_m(float hpa);
