// Host-side preview: renders hike-buddy's screen to a PPM with fake sensor values.
// Build: make -C hike-buddy/preview   Run: out/preview/preview <heading> <steps> out.ppm [page]
#include "../sys.h"
static void fake_battery(Battery* b) {
  *b = {};
  b->capacity_pct = 97;
  b->charge_raw_uah = 2638481;
  b->charge_full_uah = 2704000;
}
#define read_battery fake_battery
#define main hike_buddy_main
#include "../main.cpp"
#undef main

#include "../minui/graphics_fbdev.h"

static const char* g_out;

// In-memory stand-in for the fbdev backend.
MinuiBackendFbdev::MinuiBackendFbdev() : gr_draw(nullptr), fb_fd(-1) {}
void MinuiBackendFbdev::Blank(bool) {}
void MinuiBackendFbdev::SetDisplayedFramebuffer(unsigned) {}
GRSurface* MinuiBackendFbdev::Init() {
  static GRSurface s = {1080, 1920, 1080 * 4, 4, nullptr};
  s.data = static_cast<unsigned char*>(calloc(1, 1080 * 1920 * 4));
  gr_draw = &s;
  return gr_draw;
}
GRSurface* MinuiBackendFbdev::Flip() {
  if (g_out) {
    FILE* f = fopen(g_out, "wb");
    fprintf(f, "P6 1080 1920 255\n");
    for (int i = 0; i < 1080 * 1920; ++i) fwrite(gr_draw->data + i * 4, 1, 3, f);
    fclose(f);
  }
  return gr_draw;
}
MinuiBackendFbdev::~MinuiBackendFbdev() {}

int main(int argc, char** argv) {
  float deg = argc > 1 ? atof(argv[1]) : 247;
  S.steps = argc > 2 ? atoi(argv[2]) : 8432;
  g_out = argc > 3 ? argv[3] : "preview.ppm";
  S.have_steps = true;
  S.have_heading = true;
  // A made-up hike: climb ~420 m over 2 h, short descent, a break, climb again.
  double t0 = 1000;
  for (int m = 0; m < 180; ++m) {
    float alt = 1180 + (m < 120 ? m * 3.5f : 420 - (m - 120) * 1.2f) + (m > 160 ? (m - 160) * 2.0f : 0);
    alt += 1.5f * sinf(m * 0.7f);
    float hpa = 1013.25f * powf(1 - alt / 44330.0f, 1 / 0.190295f);
    for (int k = 0; k < 6; ++k) S.alt.add_pressure(t0 + m * 60 + k * 10, hpa);
  }
  S.fix.valid = true;
  S.fix.lat = 35.36066;
  S.fix.lon = 138.72738;
  S.fix.hunc_m = 6;
  S.fix_at = now_s();
  S.hx = sinf(deg * M_PI / 180);
  S.hy = cosf(deg * M_PI / 180);
  S.page = argc > 4 ? atoi(argv[4]) : 0;
  map_open(argc > 5 ? argv[5] : "out/map/hike-buddy.map");
  if (argc > 7) {  // map view: lat lon [zoom index]
    S.fix.lat = atof(argv[6]);
    S.fix.lon = atof(argv[7]);
    S.fix.hunc_m = 25;
  }
  if (argc > 8) S.map_zoom = atoi(argv[8]);
  gr_init();
  draw();
  return 0;
}
