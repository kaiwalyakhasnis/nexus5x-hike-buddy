// Preview stand-in for gps.cpp: pretends the modem is up.
#include "../gps.h"
bool gps_open() { return false; }
int gps_fd() { return -1; }
bool gps_connect() { return true; }
bool gps_connected() { return true; }
bool gps_radio_low_power() { return true; }
void gps_start(uint32_t) {}
void gps_stop() {}
bool gps_running() { return false; }
void gps_read(const GpsFixCb&, const GpsSkyCb&) {}
bool gps_load_xtra(const char*) { return false; }
void gps_inject_time() {}
