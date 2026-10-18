// Golden test for draw.* and segment.*: draws thousands of random lines,
// rings, triangles and seven-segment strings (many partly or wholly off
// screen) into a 1080x1920 surface and prints FNV-1a hashes of the pixels,
// plus every polar()/seg_*() return value. Build against the old and the new
// implementation and diff the output; any pixel difference changes a hash.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../draw.h"
#include "../minui/include/minui/minui.h"
#include "../segment.h"

static uint32_t rng = 777;
static uint32_t next() {
  rng = rng * 1103515245u + 12345u;
  return rng >> 8;
}
static int range(int lo, int hi) { return lo + int(next() % uint32_t(hi - lo + 1)); }

static const int W = 1080, H = 1920;
static unsigned char pixels[W * H * 4];

static uint64_t hash() {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : pixels) h = (h ^ c) * 1099511628211ull;
  return h;
}

int main() {
  GRSurface s = {W, H, W * 4, 4, pixels};
  gr_set_target(&s);
  for (int i = 0; i < 4000; ++i) {
    gr_color(next() & 255, next() & 255, next() & 255, 255);
    switch (next() % 6) {
      case 0:
        draw_line(range(-300, W + 300), range(-300, H + 300), range(-300, W + 300),
                  range(-300, H + 300), range(1, 12));
        break;
      case 1: {
        int r_out = range(0, 400);
        draw_ring(range(-200, W + 200), range(-200, H + 200), range(0, r_out), r_out);
        break;
      }
      case 2:
        draw_triangle(range(-200, W + 200), range(-200, H + 200), range(-200, W + 200),
                      range(-200, H + 200), range(-200, W + 200), range(-200, H + 200));
        break;
      case 3: {
        int x, y;
        float r = float(range(0, 5000)) / 10.0f, deg = float(range(-7200, 7200)) / 10.0f;
        polar(range(0, W), range(0, H), r, deg, &x, &y);
        printf("polar %a %a -> %d %d\n", r, deg, x, y);
        break;
      }
      case 4: {
        static const char* kStr[] = {"0123456789", "-1234", "12:34", "3.14", " 7 -", "", "1567"};
        const char* str = kStr[next() % 7];
        int w = range(1, 150), h = range(2, 260), t = range(1, 30);
        int end = seg_text(range(-200, W), range(-200, H), w, h, t, str);
        printf("seg \"%s\" %d %d %d -> %d measure %d\n", str, w, h, t, end, seg_measure(w, t, str));
        break;
      }
      default:  // a big ring, as the compass draws
        draw_ring(W / 2, range(800, 1400), range(250, 290), range(291, 300));
    }
    if (i % 100 == 99) printf("hash after %d: %016llx\n", i + 1, (unsigned long long)hash());
  }
  return 0;
}
