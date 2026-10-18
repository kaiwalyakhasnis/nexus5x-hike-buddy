// Golden test for Altitude: drives it through a long deterministic sequence
// and prints every observable value with exact float bits (%a). Used to
// check that a reimplementation behaves identically:
//   build once against the old implementation, once against the new, diff.
//
// The C++ -> Rust port (src/altitude.rs) matched byte for byte on the PC, and
// on the phone once the C++ was built with -ffp-contract=off: GCC otherwise
// fuses multiply-adds on ARM, which moves results by up to 64 ulp (0.06 mm).
//   cargo build --release --target-dir ../out/golden/rust
//   g++ -O2 tests/altitude_golden.cpp ../out/golden/rust/release/libhike_buddy.a -lpthread -ldl
#include <stdint.h>
#include <stdio.h>

#include "../altitude.h"

static uint32_t rng = 12345;
static uint32_t next() {  // LCG: deterministic on every platform
  rng = rng * 1103515245u + 12345u;
  return rng >> 8;
}
static float uniform(float lo, float hi) { return lo + (hi - lo) * float(next() % 100000) / 100000.0f; }

static void dump(const Altitude& a, const char* tag) {
  printf("[%s] valid=%d cal=%d", tag, a.valid(), a.calibrated());
  if (a.valid()) printf(" live=%a", a.live_m());
  float up, down;
  a.totals(&up, &down);
  printf(" up=%a down=%a\n", up, down);
  for (int ago = 0; ago <= Altitude::kMinutes + 1; ++ago) {
    float m;
    if (a.minute(ago, &m)) printf("  %d=%a", ago, m);
  }
  printf("\n");
}

int main() {
  Altitude a;
  dump(a, "empty");
  a.calibrate(100, 5);  // no data yet: ignored
  dump(a, "cal-before-data");

  double t = 1000.0;
  float hpa = 1005.0f;
  for (int step = 0; step < 3000; ++step) {
    int r = int(next() % 1000);
    if (r < 700) {                      // normal sample, random walk
      hpa += uniform(-0.3f, 0.3f);
      t += uniform(0.5f, 20.0f);
      a.add_pressure(t, hpa);
    } else if (r < 740) {               // implausible readings
      a.add_pressure(t, (next() & 1) ? 50.0f : 1300.0f);
    } else if (r < 780) {               // late sample from an earlier minute
      a.add_pressure(t - uniform(60.0f, 600.0f), hpa + 1.0f);
    } else if (r < 800) {               // gap: minutes to hours without data
      t += uniform(60.0f, 4.0f * 3600.0f);
    } else if (r < 830) {               // a climb / descent
      for (int k = 0; k < 30; ++k) {
        hpa += (step & 1) ? -0.12f : 0.12f;
        t += 10.0;
        a.add_pressure(t, hpa);
      }
    } else if (r < 860) {               // GPS calibration, various uncertainties
      static const float kUnc[] = {-1.0f, 0.0f, 3.0f, 10.0f, 15.0f, 15.5f, 40.0f};
      a.calibrate(uniform(-50.0f, 900.0f), kUnc[next() % 7]);
    } else if (r < 862) {               // the same time again
      a.add_pressure(t, hpa);
    }
    if (step % 37 == 0) {
      char tag[32];
      snprintf(tag, sizeof(tag), "step %d", step);
      dump(a, tag);
    }
  }
  dump(a, "end");
  return 0;
}
