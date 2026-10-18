#include "hub.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "sys.h"

// firmware/os/inc/eventnums.h
static const uint32_t EVT_NO_FIRST_SENSOR_EVENT = 0x200;
static const uint32_t EVT_NO_SENSOR_CONFIG_EVENT = 0x300;

enum { CONFIG_CMD_DISABLE = 0, CONFIG_CMD_ENABLE = 1, CONFIG_CMD_FLUSH = 2 };

// Wire format written to /dev/nanohub to configure one sensor: rate in Hz x 1024
// (sensor_hz), latency in ns (how long the hub may batch before waking us).
struct ConfigCmd {
  uint32_t evtType;
  uint64_t latency;
  uint32_t rate;
  uint8_t sensorType;
  uint8_t cmd;
  uint16_t flags;
} __attribute__((packed));

// Packets read from /dev/nanohub: an EventHeader, then numSamples samples of
// one- or three-axis layout. The first sample's 4-byte slot holds this
// FirstSample instead of a time delta; later ones hold the delta to the
// previous sample.
struct FirstSample {
  uint8_t numSamples;
  uint8_t numFlushes;
  uint8_t highAccuracy : 1;
  uint8_t biasPresent : 1;
  uint8_t biasSample : 6;
  uint8_t pad;
};

struct OneAxisSample {
  uint32_t deltaTime;  // sample 0: FirstSample instead
  union {
    float fdata;
    uint32_t idata;
  };
} __attribute__((packed));

struct ThreeAxisSample {
  uint32_t deltaTime;
  float x, y, z;
} __attribute__((packed));

struct EventHeader {
  uint32_t evtType;
  uint64_t referenceTime;
  FirstSample first;  // overlays the first sample's deltaTime
} __attribute__((packed));

static int g_fd = -1;

// Packet counts per event type, for hub_log_stats().
static const int kMaxStats = 8;
static struct { uint32_t type; int n; } g_stats[kMaxStats];

static void count_packet(uint32_t type) {
  for (int i = 0; i < kMaxStats; ++i) {
    if (g_stats[i].n == 0) g_stats[i].type = type;
    if (g_stats[i].type == type) {
      g_stats[i].n++;
      return;
    }
  }
}

void hub_log_stats(const char* tag) {
  char buf[200];
  int len = 0;
  for (int i = 0; i < kMaxStats && g_stats[i].n; ++i)
    len += snprintf(buf + len, sizeof(buf) - len, " 0x%x:%d", g_stats[i].type, g_stats[i].n);
  buf[len] = 0;
  klog("hub packets %s:%s", tag, len ? buf : " none");
  memset(g_stats, 0, sizeof(g_stats));
}

bool hub_open() {
  g_fd = open("/dev/nanohub", O_RDWR | O_CLOEXEC | O_NONBLOCK);
  if (g_fd < 0) klog("open /dev/nanohub: %s", strerror(errno));
  return g_fd >= 0;
}

int hub_fd() { return g_fd; }

// Writes one ConfigCmd (enable, disable or flush) and logs it.
static bool send_config(uint8_t sens_type, uint8_t cmd, uint32_t rate, uint64_t latency) {
  if (g_fd < 0) return false;
  ConfigCmd c = {};
  c.evtType = EVT_NO_SENSOR_CONFIG_EVENT;
  c.latency = latency;
  c.rate = rate;
  c.sensorType = sens_type;
  c.cmd = cmd;
  ssize_t n = write(g_fd, &c, sizeof(c));
  if (n != (ssize_t)sizeof(c)) {
    klog("hub config type %u cmd %u: %s", sens_type, cmd, strerror(errno));
    return false;
  }
  klog("hub config type %u cmd %u rate %u latency %llums", sens_type, cmd, rate,
       (unsigned long long)(latency / 1000000));
  return true;
}

bool hub_config(uint8_t sens_type, bool enable, uint32_t rate_q10, uint64_t latency_ns) {
  return send_config(sens_type, enable ? CONFIG_CMD_ENABLE : CONFIG_CMD_DISABLE, rate_q10,
                     latency_ns);
}

bool hub_flush(uint8_t sens_type) { return send_config(sens_type, CONFIG_CMD_FLUSH, 0, 0); }

// Hub timestamps are on the hub's clock; we only trust their spacing and pin
// the newest sample of each packet to "now". Batches leave the hub as soon as
// the latency expires, so the error is at most one sample period.
static void parse(const uint8_t* buf, size_t len, const HubSampleCb& cb) {
  if (len < sizeof(EventHeader)) return;
  const EventHeader* h = reinterpret_cast<const EventHeader*>(buf);
  uint8_t sens = h->evtType - EVT_NO_FIRST_SENSOR_EVENT;
  bool three;
  if (h->evtType == EVT_NO_FIRST_SENSOR_EVENT + SENS_TYPE_GEO_MAG_ROT_VEC) three = true;
  else if (h->evtType == EVT_NO_FIRST_SENSOR_EVENT + SENS_TYPE_STEP_COUNT ||
           h->evtType == EVT_NO_FIRST_SENSOR_EVENT + SENS_TYPE_BARO) three = false;
  else return;  // logs, other sensors: not ours

  const uint8_t* p = buf + sizeof(h->evtType) + sizeof(h->referenceTime);
  size_t stride = three ? sizeof(ThreeAxisSample) : sizeof(OneAxisSample);
  int n = h->first.numSamples;
  int fit = (len - (p - buf)) / stride;
  if (n > fit) n = fit;
  if (n <= 0) return;

  // Hub-relative timestamps (sensorhal delta decoding: low bit set = raw ns,
  // otherwise the delta is in units of 512 ns).
  uint64_t ts[256];
  uint64_t t = h->referenceTime;
  for (int k = 0; k < n && k < 256; ++k) {
    if (k > 0) {
      uint32_t d;
      memcpy(&d, p + k * stride, sizeof(d));
      t += uint64_t(d) << ((d & 1) ? 0 : 9);
    }
    ts[k] = t;
  }

  double now_s = now_boot_ms() / 1000.0;
  for (int k = 0; k < n && k < 256; ++k) {
    HubSample s = {};
    s.sens_type = sens;
    s.t_boot_s = now_s - (ts[n - 1] - ts[k]) / 1e9;
    if (three) {
      ThreeAxisSample a;
      memcpy(&a, p + k * stride, sizeof(a));
      s.v[0] = a.x;
      s.v[1] = a.y;
      s.v[2] = a.z;
    } else {
      OneAxisSample a;
      memcpy(&a, p + k * stride, sizeof(a));
      s.i = a.idata;
      s.f = a.fdata;
    }
    cb(s);
  }
}

void hub_read(const HubSampleCb& cb) {
  uint8_t buf[512];
  for (;;) {
    ssize_t len = read(g_fd, buf, sizeof(buf));
    if (len <= 0) return;  // EAGAIN: drained
    if (len >= 4) {
      uint32_t type;
      memcpy(&type, buf, sizeof(type));
      count_packet(type);
    }
    parse(buf, len, cb);
  }
}
