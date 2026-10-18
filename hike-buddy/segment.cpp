#include "segment.h"

#include "minui/minui.h"

//  aaa
// f   b
//  ggg
// e   c
//  ddd
// Segments lit per digit: bit 0 = a ... bit 6 = g (the usual 7-segment table).
static const unsigned char kDigit[10] = {
  0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f,
};
enum { A = 1, B = 2, C = 4, D = 8, E = 16, F = 32, G = 64 };

// Lights the segments in mask m in a w x h cell with stroke t.
static void draw_segments(int x, int y, int w, int h, int t, unsigned m) {
  int mid = y + (h - t) / 2;
  int gap = t / 4;  // small gaps between segments read as "LCD"
  if (m & A) gr_fill(x + t, y, x + w - t, y + t);
  if (m & G) gr_fill(x + t, mid, x + w - t, mid + t);
  if (m & D) gr_fill(x + t, y + h - t, x + w - t, y + h);
  if (m & F) gr_fill(x, y + t + gap, x + t, mid - gap);
  if (m & B) gr_fill(x + w - t, y + t + gap, x + w, mid - gap);
  if (m & E) gr_fill(x, mid + t + gap, x + t, y + h - t - gap);
  if (m & C) gr_fill(x + w - t, mid + t + gap, x + w, y + h - t - gap);
}

// Horizontal space a glyph takes: '.' and ':' are narrow.
static int advance(char c, int w, int t) {
  return (c == '.' || c == ':') ? t * 2 : w + t;
}

int seg_measure(int w, int t, const char* s) {
  int x = 0;
  for (; *s; ++s) x += advance(*s, w, t);
  return x;
}

int seg_text(int x, int y, int w, int h, int t, const char* s) {
  for (; *s; ++s) {
    char c = *s;
    if (c >= '0' && c <= '9') {
      draw_segments(x, y, w, h, t, kDigit[c - '0']);
    } else if (c == '-') {
      draw_segments(x, y, w, h, t, G);
    } else if (c == '.') {
      gr_fill(x, y + h - t, x + t, y + h);
    } else if (c == ':') {
      gr_fill(x, y + h / 3 - t / 2, x + t, y + h / 3 + t / 2);
      gr_fill(x, y + 2 * h / 3 - t / 2, x + t, y + 2 * h / 3 + t / 2);
    }
    x += advance(c, w, t);
  }
  return x;
}
