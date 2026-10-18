#include "altitude.h"

#include <math.h>

// Standard atmosphere, sea level 1013.25 hPa. Absolute altitude is off by
// whatever the weather is doing (tens of metres) until GPS calibrates it;
// gain within the 3 h window is not affected either way.
float pressure_to_altitude_m(float hpa) {
  return 44330.0f * (1.0f - powf(hpa / 1013.25f, 0.190295f));
}

static const float kHysteresisM = 3.0f;
static const float kMaxCalUncM = 15.0f;  // only trust fairly good vertical fixes

void Altitude::add_pressure(double t_boot_s, float hpa) {
  if (!init_) {
    for (int i = 0; i < kMinutes; ++i) tag_[i] = -1;
    init_ = true;
  }
  if (hpa < 100 || hpa > 1200) return;  // not a plausible reading

  int64_t m = int64_t(t_boot_s / 60.0);
  if (m < cur_min_) return;  // late sample for a closed minute
  if (m != cur_min_) {
    close_minute();
    cur_min_ = m;
  }
  sum_hpa_ += hpa;
  count_++;

  live_raw_m_ = pressure_to_altitude_m(hpa);
  have_live_ = true;
}

void Altitude::calibrate(float gps_msl_m, float vert_unc_m) {
  if (!have_live_ || vert_unc_m <= 0 || vert_unc_m > kMaxCalUncM) return;
  float target = gps_msl_m - live_raw_m_;
  if (!calibrated_) offset_m_ = target;
  else offset_m_ += 0.2f * (target - offset_m_);  // GPS altitude is noisy; ease in
  calibrated_ = true;
}

void Altitude::close_minute() {
  if (cur_min_ < 0 || count_ == 0) return;
  int slot = cur_min_ % kMinutes;
  alt_[slot] = pressure_to_altitude_m(sum_hpa_ / count_);
  tag_[slot] = cur_min_;
  sum_hpa_ = 0;
  count_ = 0;
}

bool Altitude::minute(int ago, float* m) const {
  if (!init_ || cur_min_ < 0) return false;
  int64_t want = cur_min_ - ago;
  if (want < 0) return false;
  if (ago == 0) {  // the minute still being filled
    if (count_ == 0) return false;
    *m = pressure_to_altitude_m(sum_hpa_ / count_) + offset_m_;
    return true;
  }
  int slot = want % kMinutes;
  if (tag_[slot] != want) return false;
  *m = alt_[slot] + offset_m_;
  return true;
}

void Altitude::totals(float* up_m, float* down_m) const {
  *up_m = *down_m = 0;
  bool have_ref = false;
  float ref = 0;
  for (int ago = kMinutes - 1; ago >= 0; --ago) {
    float a;
    if (!minute(ago, &a)) continue;
    if (!have_ref) {
      ref = a;
      have_ref = true;
    } else if (a - ref >= kHysteresisM) {
      *up_m += a - ref;
      ref = a;
    } else if (ref - a >= kHysteresisM) {
      *down_m += ref - a;
      ref = a;
    }
  }
}
