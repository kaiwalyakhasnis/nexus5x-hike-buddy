// Battery accounting while unplugged. The fuel gauge's coulomb counter is the
// only current sensor on the phone (no per-rail meters), so the drain is
// booked against what the phone was doing at the time -- the power mode --
// and each part's share is worked out from the differences between modes:
// GPS = (GPS-searching rate - standby rate), and so on.
//
// Output, both in RAM (lost on reboot): a log line per mode stint and a
// report rewritten at every mode change. scripts/power-report.sh prints them.
#pragma once

#include <stdint.h>

#include "sys.h"

enum PowerMode { PM_STANDBY, PM_SCREEN, PM_SCREEN_GPS, PM_GPS, kPowerModes };

struct ModeTotals {
  int64_t ms = 0;
  double uah = 0;
  int64_t suspended_ms = 0;  // kernel suspended (vs merely screen-off)
};

// Where the session's charge went, by part.
enum Part { PART_STANDBY, PART_SCREEN, PART_GPS, PART_SCREEN_GPS, kParts };
struct PartShare {
  double uah = 0;
  double extra_ma = -1;  // draw on top of standby while it was on; -1 unknown
  int64_t on_ms = 0;
};

struct WakeSource {
  char name[40];
  int64_t ms;
};

class PowerLedger {
 public:
  PowerLedger(const char* log_path, const char* report_path)
      : log_path_(log_path), report_path_(report_path) {}

  // Call whenever the mode may have changed and on every wake: notices
  // unplug (starts a session) and plug-in (freezes it), books the drain.
  void update(const Battery& b, PowerMode mode);

  bool have_session() const { return started_; }
  bool on_battery() const { return active_; }
  int64_t duration_ms() const;
  double used_uah() const;
  const ModeTotals& mode(int m) const { return modes_[m]; }
  void parts(PartShare out[kParts]) const;

  // Share of screen-off time the kernel spent suspended, 0..100; -1 if no data.
  int suspended_pct() const;
  // Completed suspend/resume cycles this session (debugfs suspend_stats).
  long sleeps() const;
  // Kernel wakeup sources that held the system awake longest this session.
  int top_wake_sources(WakeSource* out, int max) const;

  void write_report(const Battery& b) const;

 private:
  void book(const Battery& b, int64_t now, int64_t mono);
  void log(const char* fmt, ...) const __attribute__((format(printf, 2, 3)));
  void end_stint(const Battery& b, int64_t now, int64_t mono);

  const char* log_path_;
  const char* report_path_;

  bool started_ = false, active_ = false;
  int64_t start_ms_ = 0, end_ms_ = 0;
  long start_uah_ = 0, end_uah_ = 0;
  ModeTotals modes_[kPowerModes];

  int cur_ = PM_STANDBY;
  int64_t since_ms_ = 0, since_mono_ = 0;
  long since_uah_ = 0;

  // The current mode's uninterrupted stretch, for the log.
  int64_t stint_ms_ = 0, stint_mono_ = 0;
  long stint_uah_ = 0;
  mutable int64_t report_ms_ = 0;

  long sleeps_at_start_ = 0;
  static const int kMaxWake = 128;
  WakeSource wake_at_start_[kMaxWake];
  int nwake_at_start_ = 0;
};
