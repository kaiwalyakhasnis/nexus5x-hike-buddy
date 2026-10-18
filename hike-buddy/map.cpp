#include "map.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "draw.h"
#include "minui/minui.h"
#include "sys.h"

static const int kWorldBits = 28;
static const double kWorld = double(1 << kWorldBits);

// Kinds, as in tools/map/build_map.py.
enum {
  MOTORWAY = 1, TRUNK, PRIMARY, SECONDARY, TERTIARY, MINOR, TRACK, TRAIL, RAIL,
  RIVER, STREAM, COAST, LAKE,
  PEAK = 20, HUT, WATER, STATION,
};

struct Level {
  uint8_t z, shift;
  uint32_t tx0, ty0, nx, ny, index;
};

static const uint8_t* g_map;
static size_t g_size;
static Level g_levels[4];
static int g_nlevels;

// Unaligned little-endian read from the mapped file.
template <typename T>
static T rd(const uint8_t* p) {
  T v;
  memcpy(&v, p, sizeof(v));
  return v;
}

// Maps the file (it sits in the ramdisk, so this costs no extra RAM), checks
// the HBMAP1 magic and reads the level table.
bool map_open(const char* path) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    klog("map: no %s", path);
    return false;
  }
  struct stat st;
  fstat(fd, &st);
  void* p = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (p == MAP_FAILED || st.st_size < 12 || memcmp(p, "HBMAP1", 6) != 0) {
    klog("map: %s is not a map", path);
    return false;
  }
  g_map = static_cast<const uint8_t*>(p);
  g_size = st.st_size;
  g_nlevels = int(rd<uint32_t>(g_map + 8));
  if (g_nlevels > 4) g_nlevels = 4;
  for (int i = 0; i < g_nlevels; ++i) {
    const uint8_t* d = g_map + 12 + 24 * i;
    Level& l = g_levels[i];
    l.z = d[0];
    l.shift = d[1];
    l.tx0 = rd<uint32_t>(d + 4);
    l.ty0 = rd<uint32_t>(d + 8);
    l.nx = rd<uint32_t>(d + 12);
    l.ny = rd<uint32_t>(d + 16);
    l.index = rd<uint32_t>(d + 20);
  }
  klog("map: %s, %zu KB, %d levels", path, g_size / 1024, g_nlevels);
  return true;
}

bool map_loaded() { return g_map != nullptr; }

// Web Mercator, as in build_map.py's project().
void map_from_lonlat(double lon, double lat, double* gx, double* gy) {
  *gx = (lon + 180.0) / 360.0 * kWorld;
  double s = sin(lat * M_PI / 180.0);
  *gy = (0.5 - log((1 + s) / (1 - s)) / (4 * M_PI)) * kWorld;
}

double map_units_per_metre(double gy) {
  double lat = atan(sinh(M_PI * (1 - 2 * gy / kWorld)));
  return kWorld / (40075016.686 * cos(lat));
}

// --- rasteriser -----------------------------------------------------------------

// Draws straight into a surface's pixels (gr_fill is far too slow for
// thousands of map segments and drops anything touching the edge).
struct Raster {
  GRSurface* s;
  int x0, y0, x1, y1;  // clip rectangle, exclusive max
  uint8_t r, g, b;
  int dash_on, dash_off, dash_pos;  // dash_on 0: solid

  void span(int x, int y, int n) {
    if (y < y0 || y >= y1) return;
    int xe = x + n;
    if (x < x0) x = x0;
    if (xe > x1) xe = x1;
    uint8_t* p = s->data + y * s->row_bytes + x * 4;
    for (; x < xe; ++x, p += 4) {
      p[0] = r;
      p[1] = g;
      p[2] = b;
    }
  }
  void vspan(int x, int y, int n) {
    if (x < x0 || x >= x1) return;
    int ye = y + n;
    if (y < y0) y = y0;
    if (ye > y1) ye = y1;
    uint8_t* p = s->data + y * s->row_bytes + x * 4;
    for (; y < ye; ++y, p += s->row_bytes) {
      p[0] = r;
      p[1] = g;
      p[2] = b;
    }
  }

  // Cohen-Sutherland against the clip rectangle grown by the pen.
  bool clip(double& ax, double& ay, double& bx, double& by, int pad) const {
    double lx = x0 - pad, ly = y0 - pad, hx = x1 + pad, hy = y1 + pad;
    auto code = [&](double x, double y) {
      return (x < lx) | (x > hx) << 1 | (y < ly) << 2 | (y > hy) << 3;
    };
    int ca = code(ax, ay), cb = code(bx, by);
    for (;;) {
      if (!(ca | cb)) return true;
      if (ca & cb) return false;
      int c = ca ? ca : cb;
      double x, y;
      if (c & 8) { x = ax + (bx - ax) * (hy - ay) / (by - ay); y = hy; }
      else if (c & 4) { x = ax + (bx - ax) * (ly - ay) / (by - ay); y = ly; }
      else if (c & 2) { y = ay + (by - ay) * (hx - ax) / (bx - ax); x = hx; }
      else { y = ay + (by - ay) * (lx - ax) / (bx - ax); x = lx; }
      if (c == ca) { ax = x; ay = y; ca = code(ax, ay); }
      else { bx = x; by = y; cb = code(bx, by); }
    }
  }

  // Line of width t: a span across the major axis at every step.
  void line(double fax, double fay, double fbx, double fby, int t) {
    if (!clip(fax, fay, fbx, fby, t)) return;
    int ax = int(lround(fax)), ay = int(lround(fay)), bx = int(lround(fbx)), by = int(lround(fby));
    int dx = abs(bx - ax), dy = abs(by - ay);
    int sx = ax < bx ? 1 : -1, sy = ay < by ? 1 : -1;
    int err = dx - dy, h = t / 2;
    bool flat = dx >= dy;
    for (;;) {
      bool ink = true;
      if (dash_on) {
        ink = dash_pos < dash_on;
        if (++dash_pos >= dash_on + dash_off) dash_pos = 0;
      }
      if (ink) {
        if (flat) vspan(ax, ay - h, t);
        else span(ax - h, ay, t);
      }
      if (ax == bx && ay == by) break;
      int e2 = 2 * err;
      if (e2 > -dy) { err -= dy; ax += sx; }
      if (e2 < dx) { err += dx; ay += sy; }
    }
  }
};

// LEB128 varint, as written by build_map.py; false if it runs past end.
static bool read_varint(const uint8_t*& p, const uint8_t* end, uint64_t* v) {
  uint64_t r = 0;
  for (int s = 0; p < end && s < 64; s += 7) {
    uint8_t b = *p++;
    r |= uint64_t(b & 0x7F) << s;
    if (!(b & 0x80)) {
      *v = r;
      return true;
    }
  }
  return false;
}

static int64_t unzigzag(uint64_t v) { return int64_t(v >> 1) ^ -int64_t(v & 1); }

struct Style {
  uint8_t r, g, b;
  float w;  // width in px at detail zoom
  int dash_on, dash_off;
};

// Colour, width (px at the most detailed zoom) and dash pattern per kind.
// Trails are bright pink dashes on purpose: they're what a hiker looks for.
static Style style_of(int kind) {
  switch (kind) {
    case MOTORWAY: return {235, 120, 60, 6, 0, 0};
    case TRUNK: return {235, 150, 70, 5, 0, 0};
    case PRIMARY: return {235, 190, 90, 5, 0, 0};
    case SECONDARY: return {225, 215, 120, 4, 0, 0};
    case TERTIARY: return {190, 190, 190, 3, 0, 0};
    case MINOR: return {120, 120, 120, 2, 0, 0};
    case TRACK: return {170, 125, 75, 3, 10, 5};
    case TRAIL: return {255, 70, 120, 3, 12, 7};
    case RAIL: return {210, 210, 230, 3, 0, 0};
    case RIVER: return {60, 130, 230, 4, 0, 0};
    case STREAM: return {50, 105, 190, 2, 0, 0};
    case COAST: return {80, 150, 240, 3, 0, 0};
    case LAKE: return {60, 130, 230, 2, 0, 0};
  }
  return {255, 255, 255, 1, 0, 0};
}

struct Label {
  int x0, y0, x1, y1;
};

// Draws text with a black halo, unless it would overlap an earlier label.
static void place_label(Label* placed, int* n, int max, int x, int y, int scale, const char* s,
                        int clip_top, int clip_bottom, int W) {
  int w = 10 * scale * int(strlen(s)), h = 18 * scale;
  if (x < 4 || x + w > W - 4 || y < clip_top + 2 || y + h > clip_bottom - 2) return;
  for (int i = 0; i < *n; ++i)
    if (x < placed[i].x1 && x + w > placed[i].x0 && y < placed[i].y1 && y + h > placed[i].y0) return;
  if (*n < max) placed[(*n)++] = {x - 6, y - 4, x + w + 6, y + h + 4};
  gr_color(0, 0, 0, 255);
  for (int d = 0; d < 4; ++d)
    gr_text_scaled(gr_sys_font(), x + (d & 1 ? 2 : -2), y + (d & 2 ? 2 : -2), s, scale);
  gr_color(255, 255, 255, 255);
  gr_text_scaled(gr_sys_font(), x, y, s, scale);
}

// Renders the view in two passes over the visible tiles of one level: lines
// first, then point symbols and labels on top. The level is picked from the
// zoom (z13 tiles up to 5 m/px, z11 up to 20, z9 beyond), so a frame never
// decodes more than a few dozen tiles.
void map_render(GRSurface* s, int top, int height, const MapView& v) {
  const int W = s->width;
  for (int y = top; y < top + height; ++y) memset(s->data + y * s->row_bytes, 0, W * 4);
  if (!g_map) return;

  // Pick the level: the most detailed one that isn't hopelessly dense here.
  int li = v.mpp <= 5 ? 0 : v.mpp <= 20 ? 1 : 2;
  if (li >= g_nlevels) li = g_nlevels - 1;
  const Level& L = g_levels[li];

  double upm = map_units_per_metre(v.cy);
  double upp = v.mpp * upm;  // global units per pixel
  double gx0 = v.cx - W / 2.0 * upp, gy0 = v.cy - height / 2.0 * upp;
  double gx1 = gx0 + W * upp, gy1 = gy0 + height * upp;
  int64_t tsize = int64_t(1) << (kWorldBits - L.z);
  int64_t tx_lo = int64_t(gx0) / tsize, tx_hi = int64_t(gx1) / tsize;
  int64_t ty_lo = int64_t(gy0) / tsize, ty_hi = int64_t(gy1) / tsize;
  if (tx_lo < L.tx0) tx_lo = L.tx0;
  if (ty_lo < L.ty0) ty_lo = L.ty0;
  if (tx_hi > int64_t(L.tx0 + L.nx) - 1) tx_hi = L.tx0 + L.nx - 1;
  if (ty_hi > int64_t(L.ty0 + L.ny) - 1) ty_hi = L.ty0 + L.ny - 1;

  // Lines get thinner as the map zooms out.
  float wscale = v.mpp <= 3 ? 1.0f : v.mpp <= 6 ? 0.8f : v.mpp <= 20 ? 0.65f : 0.5f;
  double k = double(1 << L.shift) / upp;  // tile-local unit -> px

  Raster R = {s, 0, top, W, top + height, 0, 0, 0, 0, 0, 0};
  GRSurface* old = gr_set_target(s);
  static Label placed[256];
  int nplaced = 0;

  for (int pass = 0; pass < 2; ++pass) {  // lines, then points and labels on top
    for (int64_t ty = ty_lo; ty <= ty_hi; ++ty) {
      for (int64_t tx = tx_lo; tx <= tx_hi; ++tx) {
        const uint8_t* ie = g_map + L.index + 8 * ((ty - L.ty0) * L.nx + (tx - L.tx0));
        uint32_t off = rd<uint32_t>(ie), len = rd<uint32_t>(ie + 4);
        if (off + uint64_t(len) > g_size) continue;
        const uint8_t* p = g_map + off;
        const uint8_t* end = p + len;
        double ox = (tx * tsize - gx0) / upp, oy = (ty * tsize - gy0) / upp + top;
        uint64_t count;
        if (!read_varint(p, end, &count)) continue;
        for (uint64_t f = 0; f < count && p < end; ++f) {
          int kind = *p++;
          if (kind >= PEAK) {
            uint64_t x, y, ele;
            if (!read_varint(p, end, &x) || !read_varint(p, end, &y) || !read_varint(p, end, &ele) ||
                p >= end)
              break;
            int nlen = *p++;
            if (p + nlen > end) break;
            char name[48];
            memcpy(name, p, nlen);
            name[nlen] = 0;
            p += nlen;
            if (pass != 1) continue;
            int px = int(ox + x * k), py = int(oy + y * k);
            if (px < 0 || px >= W || py < top || py >= top + height) continue;
            char lbl[64];
            if (kind == PEAK) {
              if (v.mpp > 20 && ele < 1200) continue;
              gr_color(0, 0, 0, 255);
              draw_triangle(px - 15, py + 11, px + 15, py + 11, px, py - 16);
              gr_color(200, 150, 90, 255);
              draw_triangle(px - 11, py + 8, px + 11, py + 8, px, py - 11);
              if (name[0] && v.mpp <= 20) snprintf(lbl, sizeof(lbl), "%s %d", name, int(ele));
              else if (ele) snprintf(lbl, sizeof(lbl), "%d", int(ele));
              else lbl[0] = 0;
              if (lbl[0]) place_label(placed, &nplaced, 256, px + 16, py - 18, 2, lbl, top, top + height, W);
            } else if (kind == HUT) {
              gr_color(0, 0, 0, 255);
              gr_fill(px - 12, py - 12, px + 12, py + 12);
              gr_color(240, 200, 60, 255);
              gr_fill(px - 9, py - 9, px + 9, py + 9);
              if (name[0] && v.mpp <= 8) place_label(placed, &nplaced, 256, px + 14, py - 16, 2, name, top, top + height, W);
            } else if (kind == WATER) {
              if (v.mpp > 8) continue;
              gr_color(0, 0, 0, 255);
              draw_ring(px, py, 0, 10);
              gr_color(80, 170, 255, 255);
              draw_ring(px, py, 0, 7);
            } else if (kind == STATION) {
              if (v.mpp > 20) continue;
              gr_color(0, 0, 0, 255);
              gr_fill(px - 10, py - 10, px + 10, py + 10);
              gr_color(230, 230, 255, 255);
              gr_fill(px - 7, py - 7, px + 7, py + 7);
              if (name[0] && v.mpp <= 6) place_label(placed, &nplaced, 256, px + 12, py - 16, 2, name, top, top + height, W);
            }
            continue;
          }
          uint64_t n;
          if (!read_varint(p, end, &n)) break;
          Style st = style_of(kind);
          int t = int(st.w * wscale + 0.5f);
          if (t < 1) t = 1;
          R.r = st.r;
          R.g = st.g;
          R.b = st.b;
          R.dash_on = st.dash_on;
          R.dash_off = st.dash_off;
          R.dash_pos = 0;
          int64_t lx = 0, ly = 0;
          double prev_x = 0, prev_y = 0;
          for (uint64_t i = 0; i < n; ++i) {
            uint64_t zx, zy;
            if (!read_varint(p, end, &zx) || !read_varint(p, end, &zy)) {
              p = end;
              break;
            }
            lx += unzigzag(zx);
            ly += unzigzag(zy);
            double px = ox + lx * k, py = oy + ly * k;
            if (i > 0 && pass == 0) R.line(prev_x, prev_y, px, py, t);
            prev_x = px;
            prev_y = py;
          }
        }
      }
    }
  }
  gr_set_target(old);
}
