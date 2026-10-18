// Golden test for the sensor hub client: packet parsing (random packets of
// every kind, truncated ones, sample counts past the end, both delta-time
// encodings), the config command bytes, and every log line.
//
// Build with -DOLD_CPP and the old hub.cpp #included (its statics are then
// reachable), or against the Rust library via its test entry points.
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const int64_t kNowMs = 123456789;  // fixed "now" for timestamps

#ifdef OLD_CPP
int64_t now_boot_ms() { return kNowMs; }
void klog(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  printf("klog: ");
  vprintf(fmt, ap);
  printf("\n");
  va_end(ap);
}
#include "hub.cpp"
static void feed(const uint8_t* buf, size_t len, const HubSampleCb& cb) {
  if (len >= 4) {
    uint32_t type;
    memcpy(&type, buf, sizeof(type));
    count_packet(type);
  }
  parse(buf, len, cb);
}
static void use_fd(int fd) { g_fd = fd; }
#else
#include "../hub.h"
extern "C" int64_t hb_now_boot_ms() { return kNowMs; }
extern "C" void hb_klog(const char* msg) { printf("klog: %s\n", msg); }
static void feed(const uint8_t* buf, size_t len, const HubSampleCb& cb) { hub_test_feed(buf, len, cb); }
static void use_fd(int fd) { hub_test_use_fd(fd); }
#endif

static uint32_t rng = 4242;
static uint32_t next() {
  rng = rng * 1103515245u + 12345u;
  return rng >> 8;
}

static void print_sample(const HubSample& s) {
  printf("  type %u t %a v %a %a %a f %a i %u\n", s.sens_type, s.t_boot_s, s.v[0], s.v[1], s.v[2],
         s.f, s.i);
}

int main() {
  static const uint32_t kTypes[] = {0x200 + 10, 0x200 + 20, 0x200 + 22, 0x200 + 1, 0x300, 0x12345678};
  uint8_t buf[512];
  for (int i = 0; i < 3000; ++i) {
    uint32_t type = kTypes[next() % 6];
    size_t len = next() % 2 ? 16 + (next() % 20) * 8 : next() % 512;  // often whole samples, sometimes not
    for (size_t k = 0; k < len; ++k) buf[k] = uint8_t(next());
    if (len >= 4) memcpy(buf, &type, 4);
    if (len >= 13) buf[12] = uint8_t(next() % 3 == 0 ? next() : next() % 40);  // numSamples
    // Deltas: mix of odd (raw ns) and even (x512) values, plausible floats.
    for (size_t k = 16; k + 16 <= len; k += 8) {
      uint32_t d = next() % 4 ? (next() % 200000) : (next() | 1);
      memcpy(buf + k, &d, 4);
      if (next() % 2) {
        float f = float(next() % 200000) / 100.0f - 50.0f;
        memcpy(buf + k + 4, &f, 4);
      }
    }
    printf("packet %d type 0x%x len %zu\n", i, type, len);
    feed(buf, len, print_sample);
    if (i % 250 == 249) hub_log_stats(i % 500 == 499 ? "while on" : "while off");
  }
  hub_log_stats("end");
  hub_log_stats("empty");

  // Config commands: the exact bytes written to the device.
  int p[2];
  if (pipe(p) != 0) return 1;
  use_fd(p[1]);
  struct { uint8_t type; bool enable; uint32_t rate; uint64_t latency; } kCfg[] = {
      {20, true, sensor_hz(12.5f), 0}, {20, false, 0, 0},
      {10, true, sensor_hz(0.1f), 300000000000ull}, {10, true, sensor_hz(1.0f), 0},
      {22, true, sensor_hz(1.0f / 60.0f), 300000000000ull}, {22, true, sensor_hz(1.0f / 10.0f), 0},
  };
  for (const auto& c : kCfg) {
    bool ok = hub_config(c.type, c.enable, c.rate, c.latency);
    uint8_t out[64];
    ssize_t n = read(p[0], out, sizeof(out));
    printf("config ok=%d bytes=%zd:", ok, n);
    for (ssize_t k = 0; k < n; ++k) printf(" %02x", out[k]);
    printf("\n");
  }
  printf("flush ok=%d\n", hub_flush(10));
  uint8_t out[64];
  ssize_t n = read(p[0], out, sizeof(out));
  printf("flush bytes=%zd:", n);
  for (ssize_t k = 0; k < n; ++k) printf(" %02x", out[k]);
  printf("\n");
  close(p[1]);  // writes now fail: the error path and its log line
  printf("config on closed fd ok=%d\n", hub_config(10, true, 1024, 0));
  return 0;
}
