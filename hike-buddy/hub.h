// Minimal client for the nanohub sensor hub (/dev/nanohub), speaking the same
// wire protocol as device/google/contexthub/sensorhal/hubconnection.cpp.
// Implemented in Rust (src/hub.rs); this is the C++ face of it.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <functional>

// Sensor types from contexthub firmware/os/inc/sensType.h
enum {
  SENS_TYPE_BARO = 10,             // BMP280, hPa
  SENS_TYPE_GEO_MAG_ROT_VEC = 20,  // accel + mag fused on the hub, no gyro
  SENS_TYPE_STEP_COUNT = 22,
};

// Same float math as the firmware's SENSOR_HZ(), so rates match its tables exactly.
constexpr uint32_t sensor_hz(float hz) { return (uint32_t)(hz * 1024.0f); }

struct HubSample {
  uint8_t sens_type;
  double t_boot_s;  // sample time on CLOCK_BOOTTIME, seconds
  float v[3];       // three-axis sensors
  float f;          // one-axis float sensors (pressure)
  uint32_t i;       // one-axis integer sensors (step count)
};

extern "C" {
typedef void (*HubSampleFn)(void* ctx, const HubSample* s);
bool hb_hub_open();
int hb_hub_fd();
bool hb_hub_config(uint8_t sens_type, bool enable, uint32_t rate_q10, uint64_t latency_ns);
bool hb_hub_flush(uint8_t sens_type);
void hb_hub_read(HubSampleFn cb, void* ctx);
void hb_hub_log_stats(const char* tag);
void hb_hub_test_feed(const uint8_t* buf, size_t len, HubSampleFn cb, void* ctx);
void hb_hub_test_use_fd(int fd);
}

using HubSampleCb = std::function<void(const HubSample&)>;

inline bool hub_open() { return hb_hub_open(); }
inline int hub_fd() { return hb_hub_fd(); }

// latency_ns > 0 lets the hub batch samples and wake us once per latency.
inline bool hub_config(uint8_t sens_type, bool enable, uint32_t rate_q10, uint64_t latency_ns) {
  return hb_hub_config(sens_type, enable, rate_q10, latency_ns);
}
inline bool hub_flush(uint8_t sens_type) { return hb_hub_flush(sens_type); }

// Reads what is pending and calls cb once per sample.
inline void hub_read(const HubSampleCb& cb) {
  hb_hub_read([](void* ctx, const HubSample* s) { (*static_cast<const HubSampleCb*>(ctx))(*s); },
              const_cast<HubSampleCb*>(&cb));
}

// Logs packets received per event type since the last call (diagnostics).
inline void hub_log_stats(const char* tag) { hb_hub_log_stats(tag); }

// Test hooks (tests/hub_golden.cpp): one packet through hub_read's path, and
// a stand-in fd for /dev/nanohub.
inline void hub_test_feed(const uint8_t* buf, size_t len, const HubSampleCb& cb) {
  hb_hub_test_feed(buf, len,
                   [](void* ctx, const HubSample* s) { (*static_cast<const HubSampleCb*>(ctx))(*s); },
                   const_cast<HubSampleCb*>(&cb));
}
inline void hub_test_use_fd(int fd) { hb_hub_test_use_fd(fd); }
