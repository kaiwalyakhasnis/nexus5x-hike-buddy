#include "power.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// The fuel gauge refreshes its SRAM copy of the counter every 30 s, so a
// reading is at most that stale: a mode switch books up to 30 s of the old
// mode's drain against the new one. Good enough for whole-session shares.
static const int64_t kSampleMs = 60000;    // drain-curve point while awake
static const int64_t kMinRateMs = 30000;   // shorter modes give no usable rate
static const int64_t kReportMs = 300000;   // refresh the report at least this often (when awake anyway)

static const char* const kModeName[kPowerModes] = {"standby", "screen", "screen+gps", "gps"};
static const char* const kPartName[kParts] = {"standby", "screen", "gps", "screen+gps"};

// "HH:MM:SS" local time once the clock is set, else "+H:MM:SS" since boot.
static void stamp(char* buf, size_t n) {
  time_t t = time(nullptr);
  if (t > 1600000000) {
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, n, "%m-%d %H:%M:%S", &tm);
  } else {
    int64_t s = now_boot_ms() / 1000;
    snprintf(buf, n, "+%lld:%02lld:%02lld", (long long)(s / 3600), (long long)(s / 60 % 60),
             (long long)(s % 60));
  }
}

static void fmt_dur(char* buf, size_t n, int64_t ms) {
  int64_t s = ms / 1000;
  if (s < 60) snprintf(buf, n, "%llds", (long long)s);
  else if (s < 3600) snprintf(buf, n, "%lldm%02llds", (long long)(s / 60), (long long)(s % 60));
  else snprintf(buf, n, "%lldh%02lldm", (long long)(s / 3600), (long long)(s / 60 % 60));
}

void PowerLedger::log(const char* fmt, ...) const {
  FILE* f = fopen(log_path_, "a");
  if (!f) return;
  char ts[32];
  stamp(ts, sizeof(ts));
  fprintf(f, "%s  ", ts);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fclose(f);
}

static int read_wake_sources(WakeSource* out, int max) {
  FILE* f = fopen("/sys/kernel/debug/wakeup_sources", "r");
  if (!f) return 0;
  char line[256];
  int n = 0;
  bool header = fgets(line, sizeof(line), f) != nullptr;
  while (header && n < max && fgets(line, sizeof(line), f)) {
    // name active_count event_count wakeup_count expire_count active_since total_time ...
    char name[64];
    long long c1, c2, c3, c4, since, total;
    if (sscanf(line, "%63s %lld %lld %lld %lld %lld %lld", name, &c1, &c2, &c3, &c4, &since,
               &total) != 7)
      continue;
    snprintf(out[n].name, sizeof(out[n].name), "%.39s", name);
    out[n].ms = total;  // includes the current active period
    n++;
  }
  fclose(f);
  return n;
}

static long read_sleeps() {
  FILE* f = fopen("/sys/kernel/debug/suspend_stats", "r");
  if (!f) return 0;
  long n = 0;
  if (fscanf(f, "success: %ld", &n) != 1) n = 0;
  fclose(f);
  return n;
}

void PowerLedger::update(const Battery& b, PowerMode mode) {
  int64_t now = now_boot_ms(), mono = now_mono_ms();
  bool discharging = !b.charging && b.charge_raw_uah > 0;  // FG reads 0 until settled

  if (!active_) {
    if (!discharging) return;
    // Unplugged: a new session.
    started_ = active_ = true;
    start_ms_ = since_ms_ = now;
    since_mono_ = mono;
    start_uah_ = since_uah_ = b.charge_raw_uah;
    for (auto& m : modes_) m = ModeTotals();
    cur_ = mode;
    stint_ms_ = now;
    stint_mono_ = mono;
    stint_uah_ = b.charge_raw_uah;
    sleeps_at_start_ = read_sleeps();
    nwake_at_start_ = read_wake_sources(wake_at_start_, kMaxWake);
    klog("power: on battery, %ld uAh", b.charge_raw_uah);
    log("UNPLUGGED, %.0f mAh in the battery (%ld%%)", b.charge_raw_uah / 1000.0, b.capacity_pct);
    write_report(b);
    return;
  }

  if (b.charging) {  // plugged in: freeze the session
    book(b, now, mono);
    end_stint(b, now, mono);
    active_ = false;
    end_ms_ = now;
    end_uah_ = b.charge_raw_uah;
    klog("power: charging; session %llds used %.0f uAh", (long long)(duration_ms() / 1000),
         used_uah());
    char d[24];
    fmt_dur(d, sizeof(d), duration_ms());
    log("PLUGGED IN after %s on battery, used %.1f mAh", d, used_uah() / 1000.0);
    write_report(b);
    return;
  }
  if (!discharging) return;  // FG glitch

  if (mode != cur_) {
    book(b, now, mono);
    end_stint(b, now, mono);
    cur_ = mode;
    write_report(b);
  } else if (now - since_ms_ >= kSampleMs) {
    book(b, now, mono);
    if (now - report_ms_ >= kReportMs) write_report(b);
  }
}

// One log line for the stretch of the current mode that just ended.
void PowerLedger::end_stint(const Battery& b, int64_t now, int64_t mono) {
  int64_t dt = now - stint_ms_;
  double used = (stint_uah_ - b.charge_raw_uah) / 1000.0;
  int64_t susp = dt - (mono - stint_mono_);
  char d[24];
  fmt_dur(d, sizeof(d), dt);
  if (dt > 0)
    log("%-10s %8s  %7.2f mAh  %7.1f mA  suspended %3d%%  battery %.0f mAh", kModeName[cur_], d,
        used, used / (dt / 3600000.0), int(100 * susp / dt), b.charge_raw_uah / 1000.0);
  stint_ms_ = now;
  stint_mono_ = mono;
  stint_uah_ = b.charge_raw_uah;
}

void PowerLedger::book(const Battery& b, int64_t now, int64_t mono) {
  ModeTotals& m = modes_[cur_];
  int64_t dt = now - since_ms_;
  m.ms += dt;
  m.uah += since_uah_ - b.charge_raw_uah;
  m.suspended_ms += dt - (mono - since_mono_);
  since_ms_ = now;
  since_mono_ = mono;
  since_uah_ = b.charge_raw_uah;
}

int64_t PowerLedger::duration_ms() const {
  if (!started_) return 0;
  return (active_ ? since_ms_ : end_ms_) - start_ms_;
}

double PowerLedger::used_uah() const {
  if (!started_) return 0;
  double u = 0;
  for (const auto& m : modes_) u += m.uah;
  return u;
}

// uA averaged over a mode, or -1 if it didn't last long enough to say.
static double rate_ua(const ModeTotals& m) {
  return m.ms >= kMinRateMs ? m.uah / (m.ms / 3600000.0) : -1;
}

void PowerLedger::parts(PartShare out[kParts]) const {
  for (int i = 0; i < kParts; ++i) out[i] = PartShare();
  double base = rate_ua(modes_[PM_STANDBY]);
  double r_screen = rate_ua(modes_[PM_SCREEN]);
  double r_sg = rate_ua(modes_[PM_SCREEN_GPS]);
  double r_gps = rate_ua(modes_[PM_GPS]);
  double b0 = base < 0 ? 0 : base;

  // Extra draw of each part on top of standby. Screen-on normally runs GPS
  // too, so each of screen/GPS can be learnt from the other.
  double x_gps = r_gps >= 0 ? r_gps - b0 : (r_sg >= 0 && r_screen >= 0 ? r_sg - r_screen : -1);
  double x_screen = r_screen >= 0 ? r_screen - b0 : (r_sg >= 0 && x_gps >= 0 ? r_sg - b0 - x_gps : -1);
  if (x_gps < 0 && r_gps >= 0) x_gps = 0;
  if (x_screen < 0 && r_screen >= 0) x_screen = 0;

  auto base_of = [&](int m) {
    double v = b0 * modes_[m].ms / 3600000.0;
    return v < modes_[m].uah ? v : (modes_[m].uah > 0 ? modes_[m].uah : 0);
  };
  auto over = [&](int m) {
    double v = modes_[m].uah - base_of(m);
    return v > 0 ? v : 0;
  };

  for (int m = 0; m < kPowerModes; ++m) out[PART_STANDBY].uah += base_of(m);
  out[PART_STANDBY].uah += over(PM_STANDBY);
  out[PART_STANDBY].extra_ma = base >= 0 ? base / 1000 : -1;
  out[PART_STANDBY].on_ms = duration_ms();

  out[PART_SCREEN].uah = over(PM_SCREEN);
  out[PART_GPS].uah = over(PM_GPS);
  double sg = over(PM_SCREEN_GPS);
  int64_t sg_ms = modes_[PM_SCREEN_GPS].ms;
  if (sg > 0) {
    double h = sg_ms / 3600000.0;
    if (x_screen >= 0 && x_gps >= 0 && x_screen + x_gps > 0) {
      double s = sg * x_screen / (x_screen + x_gps);
      out[PART_SCREEN].uah += s;
      out[PART_GPS].uah += sg - s;
    } else if (x_gps >= 0) {
      double g = x_gps * h < sg ? x_gps * h : sg;
      out[PART_GPS].uah += g;
      out[PART_SCREEN].uah += sg - g;
    } else if (x_screen >= 0) {
      double s = x_screen * h < sg ? x_screen * h : sg;
      out[PART_SCREEN].uah += s;
      out[PART_GPS].uah += sg - s;
    } else {
      out[PART_SCREEN_GPS].uah = sg;  // can't split yet
    }
  }
  out[PART_SCREEN].on_ms = modes_[PM_SCREEN].ms + sg_ms;
  out[PART_GPS].on_ms = modes_[PM_GPS].ms + sg_ms;
  out[PART_SCREEN_GPS].on_ms = sg_ms;
  out[PART_SCREEN].extra_ma = x_screen >= 0 ? x_screen / 1000 : -1;
  out[PART_GPS].extra_ma = x_gps >= 0 ? x_gps / 1000 : -1;
  if (out[PART_SCREEN_GPS].uah > 0) out[PART_SCREEN_GPS].extra_ma = (r_sg - b0) / 1000;
}

int PowerLedger::suspended_pct() const {
  // Screen-off time only: with the screen on we hold a wakelock on purpose.
  int64_t total = 0, susp = 0;
  static const int kOff[] = {PM_STANDBY, PM_GPS};
  for (int i : kOff) {
    total += modes_[i].ms;
    susp += modes_[i].suspended_ms;
  }
  if (total <= 0) return -1;
  int p = int(100 * susp / total);
  return p < 0 ? 0 : p > 100 ? 100 : p;
}

long PowerLedger::sleeps() const {
  return started_ ? read_sleeps() - sleeps_at_start_ : 0;
}

int PowerLedger::top_wake_sources(WakeSource* out, int max) const {
  if (!started_) return 0;
  static WakeSource cur[kMaxWake];
  int n = read_wake_sources(cur, kMaxWake);
  int k = 0;
  for (int i = 0; i < n; ++i) {
    // Our own lock is the screen being on, already counted as SCREEN.
    if (!strcmp(cur[i].name, "hike-buddy")) continue;
    int64_t ms = cur[i].ms;
    for (int j = 0; j < nwake_at_start_; ++j)
      if (!strcmp(wake_at_start_[j].name, cur[i].name)) {
        ms -= wake_at_start_[j].ms;
        break;
      }
    if (ms < 1000) continue;
    // Insertion into the top `max`.
    int pos = k < max ? k : max;
    while (pos > 0 && out[pos - 1].ms < ms) {
      if (pos < max) out[pos] = out[pos - 1];
      pos--;
    }
    if (pos < max) {
      out[pos] = cur[i];
      out[pos].ms = ms;
      if (k < max) k++;
    }
  }
  return k;
}

void PowerLedger::write_report(const Battery& b) const {
  if (!started_) return;
  char tmp[128];
  snprintf(tmp, sizeof(tmp), "%s.new", report_path_);
  FILE* f = fopen(tmp, "w");
  if (!f) return;
  char ts[32], d[24];
  stamp(ts, sizeof(ts));
  int64_t ms = duration_ms();
  double used = used_uah() / 1000.0;
  double avg = ms > 0 ? used / (ms / 3600000.0) : 0;
  fmt_dur(d, sizeof(d), ms);
  fprintf(f, "hike-buddy power report, written %s\n\n", ts);
  fprintf(f, "%s %s: used %.1f mAh, average %.1f mA\n", active_ ? "on battery for" : "last time unplugged,",
          d, used, avg);
  if (active_ && avg > 0.05 && ms >= 10 * 60000)
    fprintf(f, "%.0f mAh left -> about %.0f h more at this average\n", b.charge_raw_uah / 1000.0,
            b.charge_raw_uah / 1000.0 / avg);

  PartShare parts[kParts];
  this->parts(parts);
  double total = 0;
  for (const auto& p : parts) total += p.uah;
  fprintf(f, "\nWHERE IT WENT       mAh   share  draw on top of standby\n");
  for (int p = 0; p < kParts; ++p) {
    if (p == PART_SCREEN_GPS && parts[p].uah <= 0) continue;
    fmt_dur(d, sizeof(d), parts[p].on_ms);
    char extra[48];
    if (p == PART_STANDBY)
      snprintf(extra, sizeof(extra), parts[p].extra_ma >= 0 ? "%.1f mA all the time" : "not measured yet",
               parts[p].extra_ma);
    else if (parts[p].on_ms <= 0) snprintf(extra, sizeof(extra), "not used");
    else if (parts[p].extra_ma >= 0) snprintf(extra, sizeof(extra), "+%.0f mA while on, %s", parts[p].extra_ma, d);
    else snprintf(extra, sizeof(extra), "on %s, rate not known yet", d);
    fprintf(f, "  %-12s %7.1f  %4.0f%%   %s\n", kPartName[p], parts[p].uah / 1000.0,
            total > 0 ? 100 * parts[p].uah / total : 0, extra);
  }

  fprintf(f, "\nBY MODE             time      mAh    avg mA  suspended\n");
  for (int m = 0; m < kPowerModes; ++m) {
    const ModeTotals& t = modes_[m];
    fmt_dur(d, sizeof(d), t.ms);
    fprintf(f, "  %-12s %9s  %7.1f  %7.1f   %3d%%\n", kModeName[m], d, t.uah / 1000.0,
            t.ms > 0 ? t.uah / 1000.0 / (t.ms / 3600000.0) : 0.0,
            t.ms > 0 ? int(100 * t.suspended_ms / t.ms) : 0);
  }

  int susp = suspended_pct();
  fprintf(f, "\nscreen off: kernel suspended %d%% of the time, %ld suspend cycles\n", susp < 0 ? 0 : susp,
          sleeps());
  fprintf(f, "kept awake by (kernel wakeup sources, time held this session):\n");
  WakeSource top[8];
  int n = top_wake_sources(top, 8);
  for (int i = 0; i < n; ++i) {
    fmt_dur(d, sizeof(d), top[i].ms);
    fprintf(f, "  %-32s %s\n", top[i].name, d);
  }
  if (n == 0) fprintf(f, "  nothing over 1 s\n");
  fclose(f);
  rename(tmp, report_path_);
  report_ms_ = now_boot_ms();
}
