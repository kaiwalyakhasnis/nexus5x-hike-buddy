// Minimal client for the nanohub sensor hub (/dev/nanohub), speaking the same
// wire protocol as device/google/contexthub/sensorhal/hubconnection.cpp.
#pragma once

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

bool hub_open();
int hub_fd();

// latency_ns > 0 lets the hub batch samples and wake us once per latency.
bool hub_config(uint8_t sens_type, bool enable, uint32_t rate_q10, uint64_t latency_ns);
bool hub_flush(uint8_t sens_type);

struct HubSample {
  uint8_t sens_type;
  double t_boot_s;  // sample time on CLOCK_BOOTTIME, seconds
  float v[3];       // three-axis sensors
  float f;          // one-axis float sensors (pressure)
  uint32_t i;       // one-axis integer sensors (step count)
};

// Reads what is pending and calls cb once per sample.
using HubSampleCb = std::function<void(const HubSample&)>;
void hub_read(const HubSampleCb& cb);

// Logs packets received per event type since the last call (diagnostics).
void hub_log_stats(const char* tag);
