#include "draw.h"

#include <math.h>
#include <stdlib.h>

#include "minui/minui.h"

void draw_line(int x0, int y0, int x1, int y1, int t) {
  int dx = abs(x1 - x0), dy = abs(y1 - y0);
  int steps = dx > dy ? dx : dy;
  if (steps == 0) steps = 1;
  int h = t / 2;
  // Brush every max(1, t/2) pixels: enough overlap for a solid stroke.
  int stride = h > 1 ? h : 1;
  for (int s = 0; s <= steps; s += stride) {
    int x = x0 + (x1 - x0) * s / steps;
    int y = y0 + (y1 - y0) * s / steps;
    gr_fill(x - h, y - h, x - h + t, y - h + t);
  }
  gr_fill(x1 - h, y1 - h, x1 - h + t, y1 - h + t);
}

void draw_ring(int cx, int cy, int r_in, int r_out) {
  for (int dy = -r_out; dy <= r_out; ++dy) {
    int xo = int(sqrtf(float(r_out * r_out - dy * dy)));
    if (abs(dy) >= r_in) {
      gr_fill(cx - xo, cy + dy, cx + xo + 1, cy + dy + 1);
    } else {
      int xi = int(sqrtf(float(r_in * r_in - dy * dy)));
      gr_fill(cx - xo, cy + dy, cx - xi, cy + dy + 1);
      gr_fill(cx + xi + 1, cy + dy, cx + xo + 1, cy + dy + 1);
    }
  }
}

void draw_triangle(int x0, int y0, int x1, int y1, int x2, int y2) {
  // Sort by y, then fill scanlines between the long edge and the two short ones.
  if (y1 < y0) { int t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
  if (y2 < y0) { int t = x0; x0 = x2; x2 = t; t = y0; y0 = y2; y2 = t; }
  if (y2 < y1) { int t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; }
  if (y2 == y0) return;
  for (int y = y0; y <= y2; ++y) {
    int xa = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
    int xb = y < y1 ? (y1 == y0 ? x1 : x0 + (x1 - x0) * (y - y0) / (y1 - y0))
                    : (y2 == y1 ? x1 : x1 + (x2 - x1) * (y - y1) / (y2 - y1));
    if (xa > xb) { int t = xa; xa = xb; xb = t; }
    gr_fill(xa, y, xb + 1, y + 1);
  }
}

void polar(int cx, int cy, float r, float deg, int* x, int* y) {
  float a = deg * float(M_PI) / 180.0f;
  *x = cx + int(lroundf(r * sinf(a)));
  *y = cy - int(lroundf(r * cosf(a)));
}
