// hike-buddy - the hiking console.
//
// Screen: a fixed header (clock, charge left, battery %, page dots) over two
// pages that slide sideways:
//   1. altitude with a 3-hour profile and climb/descent totals, compass
//      rose, step count, GPS status;
//   2. offline OpenStreetMap (Tokyo + Kanagawa): drag to pan, +/- or
//      VOL+/- to zoom, the target button follows the GPS position.
//
// Power policy:
//   screen off -> panel powered down, wakelock released, kernel autosleeps.
//                 Compass sensors are off. The hub keeps the barometer at
//                 0.1 Hz and the BMI160 counts steps, both batched: the hub
//                 wakes us about every 5 minutes.
//                 GPS runs only to get a fix: after boot, then a short
//                 refresh every 30 min (woken by a CLOCK_BOOTTIME_ALARM
//                 timer). A fix sets the clock and calibrates the altimeter.
//   screen on  -> hold the "hike-buddy" wakelock; baro 1 Hz, compass fused on the
//                 hub from accel + mag (no gyro) at 12.5 Hz, GPS 1 Hz;
//                 redraw only when something visible changes.
//   PWR toggles the screen, VOL+/- step the backlight, swipe or the corner
//   arrows change page,
//   15 s idle -> off. The screen wakes on the page it was left on. The touch
//   controller sleeps with the panel, so touch costs nothing screen-off.
//   The compass runs only while the main page shows.
//
// Battery accounting: see power.h. While unplugged it keeps a report and a
// log under /tmp (scripts/power-report.sh); the average current of each
// sleep also goes to the kernel log: `dmesg | grep hike-buddy`.

#include <linux/input.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "altitude.h"
#include "draw.h"
#include "gps.h"
#include "hub.h"
#include "map.h"
#include "minui/minui.h"
#include "power.h"
#include "segment.h"
#include "sys.h"

// Backlight steps for VOL+/- (lcd-backlight, 0..255). The backlight is most
// of the screen-on power, so it starts at the dimmest step.
static const int kBacklight[] = {1, 3, 10, 40, 120, 255};
static const int kNumBacklight = sizeof(kBacklight) / sizeof(kBacklight[0]);
static const int64_t kScreenTimeoutMs = 15000;
// After waking, wait this long so the fuel gauge has refreshed its SRAM copy
// of the coulomb counter before we read it.
static const int64_t kSettleMs = 3000;
static const uint64_t kSec = 1000000000ull;
static const uint64_t kBatchNs = 300 * kSec;  // hub batching while the screen is off
static const int64_t kMinRedrawMs = 150;  // at most ~7 fps: every frame redraws all 1080x1920 px
static const float kRedrawDeg = 1.0f;     // compass change worth a redraw
static const float kSmoothing = 0.25f;  // compass low-pass weight per 12.5 Hz sample
static const int kPages = 2;
static const int kMapPage = 1;
static const int kBodyTop = 170;      // below the header; only the body slides
static const int kSlideFrames = 6;    // frames of the page slide animation
static const int kSwipeMinPx = 150;   // horizontal travel that counts as a swipe
static const int kTapMaxPx = 60;      // movement still counted as a tap
static const int kArrowY = 1850;      // page arrows, bottom corners
static const int kArrowZone = 260;    // tap area around an arrow, px
static const int64_t kAccountEveryMs = 1000;  // battery booking + clock-jump check, when awake
static const int64_t kDragRedrawMs = 40;      // the map follows a finger at up to 25 fps
static const char* const kMapPath = "/share/hike-buddy.map";
static const char* const kXtraPath = "/share/xtra3gr.bin";  // downloaded at build time
// Demo mode for screenshots: GPS positions are ignored, so the real location
// never shows and the map opens on its default view. Checked at start:
//   touch /tmp/hike-buddy-demo; killall hike-buddy   (init restarts it)
static const char* const kDemoFlag = "/tmp/hike-buddy-demo";
static const int64_t kClockJumpMs = 2000;  // wall clock moved: tell the GPS engine
static const float kMapMpp[] = {1.5f, 3, 6, 12, 25, 50, 100};  // metres per pixel
static const int kMapZooms = sizeof(kMapMpp) / sizeof(kMapMpp[0]);
static const int kMapButtonR = 46;  // map buttons' radius, px
static const double kTakaoLat = 35.6251, kTakaoLon = 139.2436;  // first view without a fix

// GPS duty cycle (seconds on CLOCK_BOOTTIME).
static const double kGpsConnectRetry = 5;      // while the modem boots
static const double kGpsConnectGiveUp = 240;   // then only retry with the refresh
static const double kGpsFirstFix = 600;        // first fix after boot: true cold start, no time or position known
static const double kGpsRefreshEvery = 1800;
static const double kGpsRefreshFor = 120;      // warm start: ephemeris still valid
static const float kGoodFixM = 30;             // horizontal uncertainty that ends a search

static struct {
  bool screen = false;
  int bl = 0;
  int64_t last_key_ms = 0;
  bool dirty = true;

  Altitude alt;
  int drawn_alt = -100000;

  // Heading, smoothed as a unit vector so 359 -> 0 doesn't swing the long way.
  bool have_heading = false;
  float hx = 0, hy = 1;
  float drawn_heading = -1000;

  bool have_steps = false;
  uint32_t steps = 0;

  // GPS
  int alarm_fd = -1;
  double boot_s = 0;
  double search_until = 0;  // > now: searching for a fix even with the screen off
  double next_refresh = 0;
  GpsFix fix = {};
  double fix_at = -1;
  GpsSky sky = {};

  int page = 0;
  bool demo = false;
  PowerLedger power{"/tmp/hike-buddy-power.log", "/tmp/hike-buddy-power.txt"};

  // Map page.
  bool map_ready = false;
  MapView view = {};
  int map_zoom = 1;
  bool map_follow = true;  // keep the GPS position centred until the user pans
  GRSurface* map_surf = nullptr;
  bool map_valid = false;
  MapView map_drawn = {};
  int drag_dx = 0, drag_dy = 0;
  int64_t accounted_ms = 0;

  // Touch: one finger, tracked from down to up.
  bool touching = false, touch_start = false;
  int touch_slot = 0;
  int tx0 = 0, ty0 = 0, tx = 0, ty = 0;

  // Sleep accounting, from the moment the screen went off.
  bool measuring = false;
  int64_t off_boot_ms = 0, off_mono_ms = 0, on_boot_ms = 0;
  long off_charge_uah = 0;
  int hub_wakes = 0;
} S;

static double now_s() { return now_boot_ms() / 1000.0; }

// printf-style text with its top-left at x,y; scale multiplies the 10x18 font.
static void text(int x, int y, int scale, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
static void text(int x, int y, int scale, const char* fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  gr_text_scaled(gr_sys_font(), x, y, buf, scale);
}

// Text centred on cx,cy (font cells are 10x18).
static void text_centered(int cx, int cy, int scale, const char* s) {
  int w = 10 * scale * (int)strlen(s), h = 18 * scale;
  gr_text_scaled(gr_sys_font(), cx - w / 2, cy - h / 2, s, scale);
}

// Smoothed compass heading, degrees clockwise from north, 0..360.
static float heading_deg() {
  float d = atan2f(S.hx, S.hy) * 180.0f / float(M_PI);
  return d < 0 ? d + 360.0f : d;
}

// Altitude over the last 3 hours, oldest on the left, filled under the line.
static void draw_altitude_graph(int x0, int y0, int w, int h) {
  const int N = Altitude::kMinutes;
  float a[N];
  bool have[N];
  float lo = 1e9f, hi = -1e9f;
  for (int i = 0; i < N; ++i) {
    have[i] = S.alt.minute(N - 1 - i, &a[i]);
    if (!have[i]) continue;
    if (a[i] < lo) lo = a[i];
    if (a[i] > hi) hi = a[i];
  }

  gr_color(60, 60, 60, 255);
  gr_fill(x0, y0 + h, x0 + w, y0 + h + 2);
  for (int q = 1; q < 3; ++q) {  // hour ticks
    int x = x0 + w * q / 3;
    gr_fill(x, y0, x + 2, y0 + h);
  }
  gr_color(140, 140, 140, 255);
  text(x0, y0 + h + 12, 2, "-3h");
  text(x0 + w / 3 - 20, y0 + h + 12, 2, "-2h");
  text(x0 + 2 * w / 3 - 20, y0 + h + 12, 2, "-1h");
  text(x0 + w - 60, y0 + h + 12, 2, "now");
  if (lo > hi) return;  // no data yet

  // Never zoom in tighter than 20 m, so barometer noise stays flat.
  float mid = (lo + hi) / 2, span = hi - lo < 20 ? 20 : hi - lo;
  lo = mid - span / 2;
  hi = mid + span / 2;
  auto ypix = [&](float v) { return y0 + h - int((v - lo) / span * (h - 8)) - 4; };

  int prev_x = -1, prev_y = 0;
  for (int i = 0; i < N; ++i) {
    if (!have[i]) {
      prev_x = -1;
      continue;
    }
    int xa = x0 + w * i / N, xb = x0 + w * (i + 1) / N;
    int y = ypix(a[i]);
    gr_color(70, 70, 70, 255);
    gr_fill(xa, y, xb, y0 + h);
    gr_color(255, 255, 255, 255);
    if (prev_x >= 0) draw_line(prev_x, prev_y, xa, y, 4);
    gr_fill(xa, y - 2, xb, y + 2);
    prev_x = xb;
    prev_y = y;
  }

  gr_color(170, 170, 170, 255);
  text(x0 + 8, y0 + 4, 2, "%.0f", hi);
  text(x0 + 8, y0 + h - 40, 2, "%.0f", lo);
}

// Rose rotates; the pointer at the top is the direction the phone's top faces.
static void draw_compass(int cx, int cy, int r, float heading) {
  gr_color(90, 90, 90, 255);
  draw_ring(cx, cy, r - 3, r);

  for (int b = 0; b < 360; b += 10) {
    float a = b - heading;
    bool major = b % 30 == 0;
    int x0, y0, x1, y1;
    polar(cx, cy, r - (major ? 48 : 26), a, &x0, &y0);
    polar(cx, cy, r - 6, a, &x1, &y1);
    if (major) gr_color(255, 255, 255, 255);
    else gr_color(120, 120, 120, 255);
    draw_line(x0, y0, x1, y1, major ? 6 : 3);
  }

  static const struct { int deg; const char* name; } kCard[] = {
      {0, "N"}, {90, "E"}, {180, "S"}, {270, "W"}};
  for (const auto& c : kCard) {
    int x, y;
    polar(cx, cy, r - 100, c.deg - heading, &x, &y);
    if (c.deg == 0) gr_color(255, 70, 50, 255);
    else gr_color(255, 255, 255, 255);
    text_centered(x, y, 5, c.name);
  }

  // Fixed pointer (lubber line) above the rose, and a centre dot.
  gr_color(255, 255, 255, 255);
  draw_triangle(cx - 26, cy - r - 46, cx + 26, cy - r - 46, cx, cy - r + 4);
  draw_ring(cx, cy, 0, 7);
}

// The main page's GPS status line: coordinates while a fix is fresh (< 10 s),
// otherwise the search progress or how old the last fix is.
static void draw_gps_line(int y) {
  double now = now_s();
  if (!gps_connected()) {
    gr_color(100, 100, 100, 255);
    text(40, y, 3, now - S.boot_s < kGpsConnectGiveUp ? "GPS starting" : "GPS unavailable");
    return;
  }
  if (S.fix.valid && S.fix_at >= 0 && now - S.fix_at < 10) {
    gr_color(200, 200, 200, 255);
    text(40, y, 3, "%.5f%c %.5f%c +-%.0fm", fabs(S.fix.lat), S.fix.lat < 0 ? 'S' : 'N',
         fabs(S.fix.lon), S.fix.lon < 0 ? 'W' : 'E', S.fix.hunc_m);
    return;
  }
  gr_color(140, 140, 140, 255);
  if (gps_running()) {
    text(40, y, 3, "GPS searching  %d/%d sats", S.sky.heard, S.sky.in_view);
  } else if (S.fix_at >= 0) {
    int m = int((now - S.fix_at) / 60);
    text(40, y, 3, "GPS fix %d min ago", m);
  } else {
    text(40, y, 3, "GPS idle");
  }
}

// The fixed top strip on every page: clock ("--:--" until the PC or GPS sets
// it), charge left from the fuel gauge, battery % ("+" when charging), and
// the page dots. Positions are laid out for the 1080 px wide panel.
static void draw_header(const Battery& b) {
  const int W = gr_fb_width();
  char buf[32];

  // Clock (once GPS or the PC has set it) and battery.
  gr_color(255, 255, 255, 255);
  time_t t = time(nullptr);
  if (t > 1600000000) {
    struct tm tm;
    localtime_r(&t, &tm);
    text(40, 40, 5, "%02d:%02d", tm.tm_hour, tm.tm_min);
  } else {
    text(40, 40, 5, "--:--");
  }
  snprintf(buf, sizeof(buf), "%ld%%%s", b.capacity_pct, b.charging ? "+" : "");
  int pct_x = W - 40 - 50 * (int)strlen(buf);
  text(pct_x, 40, 5, "%s", buf);
  // Charge left from the fuel gauge's coulomb counter, smaller, bottom-aligned.
  if (b.charge_raw_uah > 0) {
    char mah[24];
    snprintf(mah, sizeof(mah), "%ldmAh", b.charge_raw_uah / 1000);
    gr_color(140, 140, 140, 255);
    text(pct_x - 30 - 30 * (int)strlen(mah), 40 + 90 - 54, 3, "%s", mah);
    gr_color(255, 255, 255, 255);
  }
  gr_fill(40, 160, W - 40, 166);

  // Page dots, in the gap after the clock (the battery text grows to the
  // left while charging).
  for (int p = 0; p < kPages; ++p) {
    int cx = 370 + 40 * p;
    gr_color(p == S.page ? 255 : 110, p == S.page ? 255 : 110, p == S.page ? 255 : 110, 255);
    draw_ring(cx, 85, p == S.page ? 0 : 7, 11);
  }
  gr_color(255, 255, 255, 255);
}

// Page 1: altitude (seven-segment) with 3 h climb/descent and profile,
// compass rose, step count, GPS line. Records what it drew in S.drawn_* so
// the sensor callbacks only ask for a redraw when the change is visible.
static void draw_main_page() {
  const int W = gr_fb_width();
  char buf[32];

  // Altitude: number, 3 h totals, profile.
  gr_color(140, 140, 140, 255);
  text(40, 190, 3, S.alt.calibrated() ? "ALTITUDE m  GPS" : "ALTITUDE m");
  gr_color(255, 255, 255, 255);
  if (S.alt.valid()) snprintf(buf, sizeof(buf), "%.0f", S.alt.live_m());
  else snprintf(buf, sizeof(buf), "----");
  seg_text(40, 250, 120, 210, 24, buf);
  S.drawn_alt = S.alt.valid() ? int(lroundf(S.alt.live_m())) : -100000;

  float up, down;
  S.alt.totals(&up, &down);
  gr_color(140, 140, 140, 255);
  text(700, 250, 3, "LAST 3H");
  gr_color(255, 255, 255, 255);
  text(700, 310, 5, "+%.0f", up);
  gr_color(140, 140, 140, 255);
  text(700, 395, 5, "-%.0f", down);

  draw_altitude_graph(40, 500, W - 80, 300);

  // Compass.
  float h = S.have_heading ? heading_deg() : 0;
  draw_compass(W / 2, 1200, 290, h);
  S.drawn_heading = h;

  // Steps.
  gr_color(140, 140, 140, 255);
  text(40, 1560, 3, "STEPS");
  gr_color(255, 255, 255, 255);
  snprintf(buf, sizeof(buf), "%u", S.have_steps ? S.steps : 0);
  seg_text(W - 40 - seg_measure(95, 19, buf), 1580, 95, 170, 19, buf);

  draw_gps_line(1810);
}

// Tap targets for the previous / next page, bottom corners.
static void draw_arrows() {
  const int W = gr_fb_width();
  gr_color(150, 150, 150, 255);
  if (S.page > 0) {
    draw_ring(70, kArrowY, 34, 37);
    draw_triangle(82, kArrowY - 20, 82, kArrowY + 20, 52, kArrowY);
  }
  if (S.page < kPages - 1) {
    draw_ring(W - 70, kArrowY, 34, 37);
    draw_triangle(W - 82, kArrowY - 20, W - 82, kArrowY + 20, W - 52, kArrowY);
  }
}

// --- map page -------------------------------------------------------------------

static void change_page(int to);
static void gps_policy();

// Zoom level z indexes kMapMpp (metres per pixel), clamped to the table.
static void map_set_zoom(int z) {
  S.map_zoom = z < 0 ? 0 : z >= kMapZooms ? kMapZooms - 1 : z;
  S.view.mpp = kMapMpp[S.map_zoom];
  S.dirty = true;
}

// Centres the view on the last GPS fix; false if there has never been one.
static bool map_center_on_fix() {
  if (S.fix_at < 0) return false;
  map_from_lonlat(S.fix.lon, S.fix.lat, &S.view.cx, &S.view.cy);
  return true;
}

// First visit to the map page: last fix if any, else Mt. Takao.
static void map_init_view() {
  if (S.map_ready) return;
  S.map_ready = true;
  map_set_zoom(S.map_zoom);
  if (!map_center_on_fix()) map_from_lonlat(kTakaoLon, kTakaoLat, &S.view.cx, &S.view.cy);
}

// Global units -> screen, for the current view (body band, drag offset applied).
static void map_to_screen(double gx, double gy, int* x, int* y) {
  const int W = gr_fb_width(), H = gr_fb_height();
  double upp = S.view.mpp * map_units_per_metre(S.view.cy);
  *x = int(W / 2 + (gx - S.view.cx) / upp) + S.drag_dx;
  *y = int(kBodyTop + (H - kBodyTop) / 2 + (gy - S.view.cy) / upp) + S.drag_dy;
}

// Round dark button with an optional text symbol, drawn over the map.
static void map_button(int cx, int cy, const char* sym) {
  gr_color(25, 25, 25, 255);
  draw_ring(cx, cy, 0, kMapButtonR);
  gr_color(150, 150, 150, 255);
  draw_ring(cx, cy, kMapButtonR - 3, kMapButtonR);
  gr_color(255, 255, 255, 255);
  if (sym) text_centered(cx, cy, 4, sym);
}

static void map_button_pos(int i, int* x, int* y) {  // 0 follow, 1 zoom in, 2 zoom out
  *x = gr_fb_width() - 70;
  *y = kArrowY - 2 * 128 + i * 128;
}

// Page 2. The vector map is rasterised into S.map_surf only when the view
// changes (it takes 50-200 ms); every other redraw just blits that cache,
// shifted by the drag offset while a finger moves it, and draws the
// overlays on top: GPS status, position dot, buttons, scale bar, credit.
static void draw_map_page() {
  const int W = gr_fb_width(), H = gr_fb_height();
  map_init_view();
  if (!S.map_surf) S.map_surf = gr_alloc_like_fb();
  if (!S.map_surf) return;
  if (!S.map_valid || memcmp(&S.map_drawn, &S.view, sizeof(MapView)) != 0) {
    int64_t t = now_mono_ms();
    map_render(S.map_surf, kBodyTop, H - kBodyTop, S.view);
    klog("map: render %.0f m/px in %lld ms", S.view.mpp, (long long)(now_mono_ms() - t));
    S.map_drawn = S.view;
    S.map_valid = true;
  }
  // The cached render, shifted while a finger drags it.
  int dx = S.drag_dx, dy = S.drag_dy;
  int sx = dx < 0 ? -dx : 0, sy = kBodyTop + (dy < 0 ? -dy : 0);
  int w = W - abs(dx), h = H - kBodyTop - abs(dy);
  if (w > 0 && h > 0) gr_blit(S.map_surf, sx, sy, w, h, sx + dx, sy + dy);

  if (!map_loaded()) {
    gr_color(140, 140, 140, 255);
    text(40, 300, 3, "No map data (%s)", kMapPath);
  }

  // GPS state while there's no fresh position to show.
  if (S.fix_at < 0 || now_s() - S.fix_at > 120) {
    char st[48];
    if (gps_running()) snprintf(st, sizeof(st), "GPS searching  %d/%d sats", S.sky.heard, S.sky.in_view);
    else snprintf(st, sizeof(st), "No GPS fix - tap the target");
    gr_color(0, 0, 0, 255);
    gr_fill(30, kBodyTop + 14, 40 + 30 * (int)strlen(st) + 10, kBodyTop + 80);
    gr_color(255, 200, 80, 255);
    text(40, kBodyTop + 20, 3, "%s", st);
  }

  // Where we are: dot, accuracy circle; grey once the fix is old.
  if (S.fix_at >= 0) {
    double gx, gy;
    map_from_lonlat(S.fix.lon, S.fix.lat, &gx, &gy);
    int x, y;
    map_to_screen(gx, gy, &x, &y);
    bool fresh = now_s() - S.fix_at < 120;
    int acc = int(S.fix.hunc_m / S.view.mpp);
    if (acc > 20 && acc < 2000) {
      gr_color(60, 140, 255, 255);
      draw_ring(x, y, acc - 2, acc);
    }
    gr_color(255, 255, 255, 255);
    draw_ring(x, y, 0, 17);
    if (fresh) gr_color(40, 120, 255, 255);
    else gr_color(120, 120, 120, 255);
    draw_ring(x, y, 0, 12);
  }

  // Buttons: follow (filled when following), zoom in, zoom out.
  int bx, by;
  map_button_pos(0, &bx, &by);
  map_button(bx, by, nullptr);
  gr_color(255, 255, 255, 255);
  draw_ring(bx, by, 18, 22);
  if (S.map_follow) draw_ring(bx, by, 0, 9);
  gr_fill(bx - 32, by - 2, bx - 20, by + 2);
  gr_fill(bx + 20, by - 2, bx + 32, by + 2);
  gr_fill(bx - 2, by - 32, bx + 2, by - 20);
  gr_fill(bx - 2, by + 20, bx + 2, by + 32);
  map_button_pos(1, &bx, &by);
  map_button(bx, by, "+");
  map_button_pos(2, &bx, &by);
  map_button(bx, by, "-");

  // Scale bar: the longest round distance under 280 px.
  static const int kNice[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000};
  int m = kNice[0];
  for (int v : kNice)
    if (v / S.view.mpp <= 280) m = v;
  int len = int(m / S.view.mpp), x0 = 150, y0 = kArrowY + 4;
  char lbl[16];
  if (m >= 1000) snprintf(lbl, sizeof(lbl), "%d km", m / 1000);
  else snprintf(lbl, sizeof(lbl), "%d m", m);
  gr_color(0, 0, 0, 255);
  gr_fill(x0 - 10, y0 - 56, x0 + (len > 230 ? len : 230) + 10, y0 + 54);
  gr_color(255, 255, 255, 255);
  gr_fill(x0, y0 - 3, x0 + len, y0 + 3);
  gr_fill(x0, y0 - 14, x0 + 4, y0 + 3);
  gr_fill(x0 + len - 4, y0 - 14, x0 + len, y0 + 3);
  text(x0, y0 - 52, 2, "%s", lbl);
  gr_color(150, 150, 150, 255);
  text(x0, y0 + 14, 2, "(c) OpenStreetMap");
}

// A tap or the end of a drag on the map page.
static void map_touch_up(int dx, int dy) {
  if (abs(dx) >= kTapMaxPx || abs(dy) >= kTapMaxPx) {  // drag: move the view
    double upp = S.view.mpp * map_units_per_metre(S.view.cy);
    S.view.cx -= dx * upp;
    S.view.cy -= dy * upp;
    S.map_follow = false;
    S.drag_dx = S.drag_dy = 0;
    S.dirty = true;
    return;
  }
  S.drag_dx = S.drag_dy = 0;
  for (int i = 0; i < 3; ++i) {
    int bx, by;
    map_button_pos(i, &bx, &by);
    if (abs(S.tx - bx) > kMapButtonR + 20 || abs(S.ty - by) > kMapButtonR + 20) continue;
    if (i == 0) {
      S.map_follow = true;
      if (!map_center_on_fix() || now_s() - S.fix_at > 120) {
        // No (recent) fix: search for one even after the screen goes off.
        S.search_until = now_s() + kGpsFirstFix;
        gps_policy();
      }
    } else {
      map_set_zoom(S.map_zoom + (i == 1 ? -1 : 1));
    }
    S.dirty = true;
    return;
  }
  if (S.tx < kArrowZone && S.ty > kArrowY - kArrowZone / 2) change_page(S.page - 1);
}

static void draw_page(int page, const Battery& b) {
  if (page == 0) draw_main_page();
  else draw_map_page();
}

// Full redraw of the current page and flip to the panel.
static void draw() {
  Battery b;
  read_battery(&b);
  gr_color(0, 0, 0, 255);
  gr_clear();
  draw_header(b);
  draw_page(S.page, b);
  draw_arrows();
  gr_flip();
  S.dirty = false;
}

// Slides the body from page `from` to S.page; the header stays put.
static void slide_from(int from) {
  static GRSurface* surf[2];
  if (!surf[0]) surf[0] = gr_alloc_like_fb();
  if (!surf[1]) surf[1] = gr_alloc_like_fb();
  if (!surf[0] || !surf[1]) {
    draw();
    return;
  }
  Battery b;
  read_battery(&b);
  const int W = gr_fb_width(), H = gr_fb_height();
  int pages[2] = {from, S.page};
  for (int i = 0; i < 2; ++i) {
    GRSurface* fb = gr_set_target(surf[i]);
    gr_color(0, 0, 0, 255);
    gr_clear();
    draw_page(pages[i], b);
    gr_set_target(fb);
  }
  bool forward = S.page > from;  // new page comes in from the right
  for (int k = 1; k <= kSlideFrames; ++k) {
    float t = float(k) / kSlideFrames;
    int off = int(W * (1 - (1 - t) * (1 - t) * (1 - t)));  // ease out
    gr_color(0, 0, 0, 255);
    gr_clear();
    draw_header(b);
    int h = H - kBodyTop;
    if (forward) {
      gr_blit(surf[0], off, kBodyTop, W - off, h, 0, kBodyTop);
      gr_blit(surf[1], 0, kBodyTop, off, h, W - off, kBodyTop);
    } else {
      gr_blit(surf[0], 0, kBodyTop, W - off, h, off, kBodyTop);
      gr_blit(surf[1], W - off, kBodyTop, off, h, 0, kBodyTop);
    }
    gr_flip();
  }
  S.dirty = false;
}

// --- GPS duty cycle ---------------------------------------------------------

// Wakes on_alarm() at at_s (CLOCK_BOOTTIME seconds). The timer is a
// CLOCK_BOOTTIME_ALARM timerfd, so it also wakes the phone from suspend.
static void arm_alarm(double at_s) {
  if (S.alarm_fd < 0) return;
  itimerspec its = {};
  double now = now_s();
  double in = at_s > now ? at_s - now : 0.05;
  its.it_value.tv_sec = time_t(in);
  its.it_value.tv_nsec = long((in - double(its.it_value.tv_sec)) * 1e9);
  timerfd_settime(S.alarm_fd, 0, &its, nullptr);
}

// Books the battery drain against what the phone is doing now.
static void account() {
  Battery b;
  read_battery(&b);
  PowerMode m = S.screen ? (gps_running() ? PM_SCREEN_GPS : PM_SCREEN)
                         : (gps_running() ? PM_GPS : PM_STANDBY);
  S.power.update(b, m);
  S.accounted_ms = now_boot_ms();
}

// Decides whether the GPS engine should run, and when to look again. Called
// on every event that can change the answer (screen on/off, alarm, fix).
//   - Until the modem's LOC service shows up, retry every kGpsConnectRetry s
//     for kGpsConnectGiveUp s after boot, then only at the refresh interval.
//   - Once connected: search up to kGpsFirstFix s for a first fix, then every
//     kGpsRefreshEvery s search for kGpsRefreshFor s; with the screen on GPS
//     runs regardless. A good fix (on_gps_fix) ends a search early.
// Re-arms the alarm for the next decision point.
static void gps_policy() {
  if (gps_fd() < 0) return;
  double now = now_s();
  double next;

  bool booting = now - S.boot_s < kGpsConnectGiveUp;
  if (gps_connected() && !gps_radio_low_power()) {
    gps_connect();  // DMS registers after LOC
  }
  if (!gps_connected()) {
    if (gps_connect()) {
      S.search_until = now + kGpsFirstFix;
      S.next_refresh = now + kGpsRefreshEvery;
    } else {
      arm_alarm(now + (booting ? kGpsConnectRetry : kGpsRefreshEvery));
      return;
    }
  }

  if (now >= S.next_refresh) {
    S.search_until = now + kGpsRefreshFor;
    S.next_refresh = now + kGpsRefreshEvery;
  }

  bool searching = S.search_until > now;
  bool want = S.screen || searching;
  if (want && !gps_running()) gps_start(1000);
  if (!want && gps_running()) {
    gps_stop();
    S.dirty = true;
  }

  account();
  next = searching ? S.search_until : S.next_refresh;
  if (!gps_radio_low_power() && booting && now + kGpsConnectRetry < next) next = now + kGpsConnectRetry;
  arm_alarm(next);
}

// Every position report from the engine, including "still searching" ones.
// A valid fix sets the system clock (the PMIC RTC is read-only), calibrates
// the barometric altitude, recentres a following map, and ends a search once
// it is better than kGoodFixM. Demo mode drops fixes before any of that.
static void on_gps_fix(const GpsFix& f) {
  S.dirty = true;
  static uint32_t logged_status = ~0u;
  if (f.status != logged_status) {
    klog("gps: report status %u%s", f.status, f.valid ? " (position)" : "");
    logged_status = f.status;
  }
  if (!f.valid || S.demo) return;
  S.fix = f;
  S.fix_at = now_s();

  // Set the clock (the PMIC RTC is read-only on this device).
  if (f.utc_ms > 1600000000000ull) {
    timeval tv = {time_t(f.utc_ms / 1000), suseconds_t((f.utc_ms % 1000) * 1000)};
    timeval cur;
    gettimeofday(&cur, nullptr);
    if (llabs((long long)cur.tv_sec - (long long)tv.tv_sec) > 2) {
      settimeofday(&tv, nullptr);
      klog("gps: clock set");
    }
  }
  if (f.have_alt) S.alt.calibrate(f.alt_msl_m, f.vunc_m);
  if (S.map_follow && S.map_ready) {
    // Recentre only when the dot has wandered, so the map isn't re-rendered every second.
    double gx, gy;
    map_from_lonlat(f.lon, f.lat, &gx, &gy);
    double upp = S.view.mpp * map_units_per_metre(S.view.cy);
    if (fabs(gx - S.view.cx) / upp > 150 || fabs(gy - S.view.cy) / upp > 250) {
      S.view.cx = gx;
      S.view.cy = gy;
    }
  }

  if (f.hunc_m > 0 && f.hunc_m < kGoodFixM && S.search_until > 0) {
    klog("gps: fix +-%.0fm, %d sats, alt %.0f +-%.0f", f.hunc_m, f.sats_used, f.alt_msl_m, f.vunc_m);
    S.search_until = 0;  // got what we came for
    gps_policy();
  }
}

// Satellite counts, for the "searching n/m sats" lines.
static void on_gps_sky(const GpsSky& s) {
  if (s.in_view != S.sky.in_view || s.heard != S.sky.heard)
    klog("gps: %d satellites in view, %d heard", s.in_view, s.heard);
  if (s.heard != S.sky.heard || s.in_view != S.sky.in_view) S.dirty = true;
  S.sky = s;
}

static int on_gps(int, uint32_t) {
  gps_read(on_gps_fix, on_gps_sky);
  return 0;
}

static int on_alarm(int fd, uint32_t) {
  uint64_t n;
  if (read(fd, &n, sizeof(n)) != sizeof(n)) return 0;
  gps_policy();
  return 0;
}

// --- sensor hub ---------------------------------------------------------------

// Screen off: compass off; baro and steps lazy and batched.
static void sensors_screen_off() {
  hub_config(SENS_TYPE_GEO_MAG_ROT_VEC, false, 0, 0);
  hub_config(SENS_TYPE_BARO, true, sensor_hz(0.1f), kBatchNs);
  hub_config(SENS_TYPE_STEP_COUNT, true, sensor_hz(1.0f / 60.0f), kBatchNs);
}

// The compass only runs while its page is showing.
static void compass_for_page() {
  if (S.page == 0) hub_config(SENS_TYPE_GEO_MAG_ROT_VEC, true, sensor_hz(12.5f), 0);
  else hub_config(SENS_TYPE_GEO_MAG_ROT_VEC, false, 0, 0);
}

// Screen on: compass (main page only), barometer at 1 Hz and steps every
// 10 s, unbatched; flush hands over what was batched while the screen was off.
static void sensors_screen_on() {
  compass_for_page();
  hub_config(SENS_TYPE_BARO, true, sensor_hz(1.0f), 0);
  hub_config(SENS_TYPE_STEP_COUNT, true, sensor_hz(1.0f / 10.0f), 0);
  hub_flush(SENS_TYPE_BARO);  // hand over whatever is still batched
  hub_flush(SENS_TYPE_STEP_COUNT);
}

// One sample from the sensor hub: pressure -> altitude history, rotation
// vector -> smoothed heading, step counter -> steps. Marks the screen dirty
// only when the drawn value would change.
static void on_hub_sample(const HubSample& s) {
  if (s.sens_type == SENS_TYPE_BARO) {
    S.alt.add_pressure(s.t_boot_s, s.f);
    if (S.alt.valid() && int(lroundf(S.alt.live_m())) != S.drawn_alt) S.dirty = true;
  } else if (s.sens_type == SENS_TYPE_GEO_MAG_ROT_VEC) {
    // Rotation vector (x, y, z, w = sqrt(1 - |v|^2)) -> azimuth, as in
    // SensorManager.getOrientation(): atan2(R[1], R[4]).
    float x = s.v[0], y = s.v[1], z = s.v[2];
    float w2 = 1.0f - x * x - y * y - z * z;
    float w = w2 > 0 ? sqrtf(w2) : 0;
    float r1 = 2 * x * y - 2 * z * w;
    float r4 = 1 - 2 * x * x - 2 * z * z;
    float n = sqrtf(r1 * r1 + r4 * r4);
    if (n < 1e-3f) return;  // phone upright: heading undefined
    float sx = r1 / n, sy = r4 / n;
    if (!S.have_heading) {
      S.hx = sx;
      S.hy = sy;
      S.have_heading = true;
    } else {
      S.hx += kSmoothing * (sx - S.hx);
      S.hy += kSmoothing * (sy - S.hy);
    }
    float d = fabsf(heading_deg() - S.drawn_heading);
    if (d > 180) d = 360 - d;
    if (d >= kRedrawDeg) S.dirty = true;
  } else if (s.sens_type == SENS_TYPE_STEP_COUNT) {
    if (!S.have_steps || s.i != S.steps) S.dirty = true;
    S.steps = s.i;
    S.have_steps = true;
  }
}

static int on_hub(int, uint32_t) {
  if (!S.screen) S.hub_wakes++;
  hub_read(on_hub_sample);
  return 0;
}

// --- screen -----------------------------------------------------------------

// Screen off: blank the panel, slow and batch the sensors, re-plan GPS,
// start the sleep measurement, then drop the wakelock so autosleep suspends.
static void screen_off() {
  gr_color(0, 0, 0, 255);
  gr_clear();
  gr_flip();
  backlight(0);
  gr_fb_blank(true);
  S.screen = false;
  S.touching = false;
  S.drag_dx = S.drag_dy = 0;
  S.have_heading = false;  // stale the moment the sensors stop
  sensors_screen_off();
  gps_policy();

  Battery b;
  read_battery(&b);
  S.measuring = !b.charging && b.charge_raw_uah > 0;  // FG reads 0 until it has settled after boot
  S.off_charge_uah = b.charge_raw_uah;
  S.off_boot_ms = now_boot_ms();
  S.off_mono_ms = now_mono_ms();
  S.hub_wakes = 0;
  klog("screen off, charge_raw=%ld uAh, altitude %.0f m%s, steps %u", b.charge_raw_uah,
       S.alt.live_m(), S.alt.calibrated() ? " (GPS)" : "", S.steps);
  hub_log_stats("while on");
  account();
  wake_unlock();  // autosleep takes it from here
}

// Screen on: take the wakelock first (so the kernel stays up), then sensors,
// panel and backlight; draws the page the screen was left on.
static void screen_on() {
  wake_lock();
  hub_log_stats("while off");
  S.on_boot_ms = now_boot_ms();
  sensors_screen_on();
  gr_fb_blank(false);
  backlight(kBacklight[S.bl]);
  S.screen = true;
  S.last_key_ms = now_boot_ms();
  gps_policy();
  account();
  draw();
}

// Called kSettleMs after waking: log the battery cost of the sleep that just ended.
static void finish_sleep_measurement() {
  S.measuring = false;
  Battery b;
  read_battery(&b);
  if (b.charging) return;

  int64_t boot_ms = now_boot_ms(), mono_ms = now_mono_ms();
  int64_t slept_ms = S.on_boot_ms - S.off_boot_ms;
  int64_t total_ms = boot_ms - S.off_boot_ms;
  int64_t suspended_ms = total_ms - (mono_ms - S.off_mono_ms);
  if (slept_ms < 10000) return;  // too short to say anything

  // The counter also ran for the kSettleMs we've been awake with the screen
  // on; take that out using the current draw right now.
  double used_uah = labs(S.off_charge_uah - b.charge_raw_uah);
  double awake_uah = labs(b.current_ua) * (boot_ms - S.on_boot_ms) / 3600000.0;
  if (used_uah > awake_uah) used_uah -= awake_uah;

  double ma = used_uah / 1000.0 / (slept_ms / 3600000.0);
  double left_h = ma > 0.05 ? b.charge_full_uah / 1000.0 * b.capacity_pct / 100.0 / ma : 0;
  klog("sleep %llds avg %.2f mA, suspended %d%%, hub wakes %d, ~%.0fh left at this rate",
       (long long)(slept_ms / 1000), ma, int(100 * suspended_ms / total_ms), S.hub_wakes, left_h);
}

// Key press (value 1 only). POWER toggles the screen; VOL+/- step the
// backlight, or zoom on the map page. A key on a dark screen only wakes it.
static void on_key(int code) {
  if (code != KEY_POWER && code != KEY_VOLUMEUP && code != KEY_VOLUMEDOWN) return;
  S.last_key_ms = now_boot_ms();
  if (code == KEY_POWER) {
    if (S.screen) screen_off();
    else screen_on();
    return;
  }
  if (!S.screen) {
    screen_on();  // any key wakes the screen; it doesn't also act
    return;
  }
  if (S.page == kMapPage) {  // volume keys zoom the map
    map_set_zoom(S.map_zoom + (code == KEY_VOLUMEUP ? -1 : 1));
    return;
  }
  if (code == KEY_VOLUMEUP && S.bl < kNumBacklight - 1) S.bl++;
  if (code == KEY_VOLUMEDOWN && S.bl > 0) S.bl--;
  backlight(kBacklight[S.bl]);
}

// Switch pages with the slide animation; the compass runs only on page 1.
static void change_page(int to) {
  if (to < 0 || to >= kPages || to == S.page) return;
  int from = S.page;
  S.page = to;
  compass_for_page();
  if (to != 0) S.have_heading = false;
  slide_from(from);
  draw();  // the live version of the new page
}

// Touch screen (multitouch protocol B, first finger only), panel pixels.
static void on_touch(int fd, const input_event& ev) {
  static int fd_scaled = -1;
  static input_absinfo ax, ay;
  if (fd != fd_scaled) {
    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) < 0 || ax.maximum <= 0) ax.maximum = 0;
    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) < 0 || ay.maximum <= 0) ay.maximum = 0;
    fd_scaled = fd;
  }
  if (!S.screen) return;  // the controller is asleep anyway

  // The Synaptics driver sends no BTN_TOUCH: a finger is a tracking id in
  // slot 0, from id >= 0 (down) to id -1 (up).
  if (ev.type == EV_ABS && ev.code == ABS_MT_SLOT) {
    S.touch_slot = ev.value;
  } else if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID) {
    if (S.touch_slot != 0) return;
    if (ev.value >= 0) {
      S.touching = true;
      S.touch_start = true;
      S.last_key_ms = now_boot_ms();
    } else if (S.touching) {
      S.touching = false;
      S.last_key_ms = now_boot_ms();
      int dx = S.tx - S.tx0, dy = S.ty - S.ty0;
      const int W = gr_fb_width();
      if (S.page == kMapPage) {
        map_touch_up(dx, dy);
      } else if (abs(dx) >= kSwipeMinPx && abs(dx) > 2 * abs(dy)) {
        change_page(S.page + (dx < 0 ? 1 : -1));
      } else if (abs(dx) < kTapMaxPx && abs(dy) < kTapMaxPx && S.ty > kArrowY - kArrowZone / 2) {
        if (S.tx < kArrowZone) change_page(S.page - 1);
        else if (S.tx > W - kArrowZone) change_page(S.page + 1);
      }
    }
  } else if (ev.type == EV_ABS && S.touching && S.touch_slot == 0) {
    int W = gr_fb_width(), H = gr_fb_height();
    if (ev.code == ABS_MT_POSITION_X)
      S.tx = ax.maximum ? int(int64_t(ev.value - ax.minimum) * W / (ax.maximum - ax.minimum + 1)) : ev.value;
    else if (ev.code == ABS_MT_POSITION_Y)
      S.ty = ay.maximum ? int(int64_t(ev.value - ay.minimum) * H / (ay.maximum - ay.minimum + 1)) : ev.value;
    else return;
  } else if (ev.type == EV_SYN && S.touch_start && S.touching) {
    S.tx0 = S.tx;  // first full position report of this touch
    S.ty0 = S.ty;
    S.touch_start = false;
  } else if (ev.type == EV_SYN && S.touching && S.page == kMapPage) {
    int dx = S.tx - S.tx0, dy = S.ty - S.ty0;
    if (abs(dx) >= kTapMaxPx || abs(dy) >= kTapMaxPx || S.drag_dx || S.drag_dy) {
      S.drag_dx = dx;  // the map follows the finger
      S.drag_dy = dy;
      S.dirty = true;
    }
  }
}

// minui input callback: one evdev event per call (epoll is level-triggered).
static int on_input(int fd, uint32_t epevents) {
  input_event ev;
  if (ev_get_input(fd, epevents, &ev) != 0) return 0;
  if (ev.type == EV_KEY && ev.value == 1) on_key(ev.code);
  else if (ev.type == EV_ABS || ev.type == EV_SYN) on_touch(fd, ev);
  return 0;
}

// Local time zone, written at build time from the build host's (POSIX TZ string).
static void load_timezone() {
  char tz[64];
  if (read_str("/etc/TZ", tz, sizeof(tz))) {
    setenv("TZ", tz, 1);
    tzset();
  }
}

// Sets everything up, then runs the event loop: with the screen off it blocks
// in epoll (the kernel can suspend) until a key, a hub batch, a GPS message or
// the alarm; with the screen on it also wakes for redraws and the timeout.
int main() {
  wake_lock();
  klog("start");
  S.demo = access(kDemoFlag, F_OK) == 0;
  if (S.demo) klog("demo mode: GPS positions ignored");
  load_timezone();
  S.boot_s = now_s();
  if (gr_init() != 0) {
    klog("gr_init failed");
    return 1;
  }
  if (ev_init(on_input, true) != 0) klog("ev_init: no input devices");
  if (hub_open()) ev_add_fd(hub_fd(), on_hub);
  map_open(kMapPath);
  if (gps_open()) {
    gps_load_xtra(kXtraPath);
    ev_add_fd(gps_fd(), on_gps);
  }
  S.alarm_fd = timerfd_create(CLOCK_BOOTTIME_ALARM, TFD_NONBLOCK | TFD_CLOEXEC);
  if (S.alarm_fd >= 0) ev_add_fd(S.alarm_fd, on_alarm);
  else klog("timerfd CLOCK_BOOTTIME_ALARM unavailable");

  screen_on();
  autosleep(true);

  int64_t drawn_min = -1, drawn_ms = 0;
  for (;;) {
    // Screen off: block until a key, a hub batch, GPS or the alarm; the
    // kernel may suspend. Screen on: wake for the redraw throttle and timeout.
    int timeout = S.screen ? (S.dirty ? int(kMinRedrawMs) : 1000) : -1;
    if (ev_wait(timeout) == 0) ev_dispatch();

    int64_t now = now_boot_ms();
    if (now - S.accounted_ms >= kAccountEveryMs) {
      account();
      // The clock was set (PC over USB, or GPS): hand it to the engine.
      static int64_t offset = 0;
      timeval tv;
      gettimeofday(&tv, nullptr);
      int64_t o = int64_t(tv.tv_sec) * 1000 + tv.tv_usec / 1000 - now;
      if (llabs(o - offset) > kClockJumpMs) {
        if (offset) gps_inject_time();
        offset = o;
      }
    }
    if (!S.screen) continue;
    if (S.measuring && now - S.on_boot_ms >= kSettleMs) finish_sleep_measurement();
    if (now - S.last_key_ms >= kScreenTimeoutMs) {
      screen_off();
      continue;
    }
    int64_t wall_min = time(nullptr) / 60;  // clock tick
    int64_t min_gap = S.touching && S.page == kMapPage ? kDragRedrawMs : kMinRedrawMs;
    if ((S.dirty && now - drawn_ms >= min_gap) || wall_min != drawn_min) {
      draw();
      drawn_min = wall_min;
      drawn_ms = now;
    }
  }
}
