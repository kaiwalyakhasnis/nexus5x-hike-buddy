#include "gps.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "sys.h"

// --- kernel IPC router ABI (include/uapi/linux/msm_ipc.h) ---
#define AF_MSM_IPC 27
#define MSM_IPC_ADDR_NAME 1
#define MSM_IPC_ADDR_ID 2

struct msm_ipc_addr {
  unsigned char addrtype;
  union {
    struct { uint32_t node_id, port_id; } port_addr;
    struct { uint32_t service, instance; } port_name;
  } addr;
};

struct sockaddr_msm_ipc {
  unsigned short family;
  msm_ipc_addr address;
  unsigned char reserved;
};

struct msm_ipc_server_info {
  uint32_t node_id, port_id, service, instance;
};

struct server_lookup_args {
  struct { uint32_t service, instance; } port_name;
  int num_entries_in_array;
  int num_entries_found;
  uint32_t lookup_mask;
  msm_ipc_server_info srv_info[1];
};

#define IPC_ROUTER_IOCTL_LOOKUP_SERVER _IOWR(0xC3, 2, struct sockaddr_msm_ipc)

// --- QMI (QCCI framing over the IPC router: no QMUX header) ---
enum { QMI_REQ = 0, QMI_RESP = 2, QMI_IND = 4 };

struct QmiHeader {
  uint8_t type;
  uint16_t txn;
  uint16_t msg_id;
  uint16_t len;
} __attribute__((packed));

static const uint32_t kSvcDms = 0x02;
static const uint32_t kSvcLoc = 0x10;

static const uint16_t DMS_SET_OPERATING_MODE = 0x002E;
static const uint8_t DMS_MODE_LOW_POWER = 1;  // RF off, not persistent

// The sequence below copies what LineageOS 15.1's Qualcomm location stack
// (libloc_api_v02 in system_server) sends, captured with strace on this phone.
static const uint16_t LOC_REG_EVENTS = 0x0021;
static const uint16_t LOC_START = 0x0022;
static const uint16_t LOC_STOP = 0x0023;
static const uint16_t LOC_IND_POSITION = 0x0024;
static const uint16_t LOC_IND_SV_INFO = 0x0025;
static const uint16_t LOC_IND_NMEA = 0x0026;
static const uint16_t LOC_IND_INJECT_TIME_REQ = 0x0028;
static const uint16_t LOC_IND_INJECT_XTRA_REQ = 0x0029;
static const uint16_t LOC_IND_ENGINE_STATE = 0x002B;
static const uint16_t LOC_INJECT_XTRA_DATA = 0x0035;
static const uint16_t LOC_INJECT_UTC_TIME = 0x0038;
static const uint16_t LOC_SET_OPERATION_MODE = 0x004A;

// Android's masks: idle = the modem's assistance requests (time, XTRA,
// position, ...); during a session also position, SV info, engine state.
static const uint64_t LOC_EVENTS_IDLE = 0x67EE78;
static const uint64_t LOC_EVENTS_SESSION = 0x67EEFB;
static const uint32_t LOC_OPERATION_MSB = 2;  // assisted by XTRA/time when available
static const uint32_t LOC_RECURRENCE_PERIODIC = 1;
static const uint32_t LOC_ACCURACY_HIGH = 3;
static const uint32_t LOC_ALTITUDE_ASSUMED_DISABLED = 2;
static const uint32_t LOC_SESSION_SUCCESS = 0;
static const uint8_t kSession = 1;
static const uint16_t kXtraPart = 1024;
static const uint32_t kTimeUncMs = 1500;  // clock set from the PC over telnet, 1 s resolution

static int g_fd = -1;
static bool g_have_loc = false, g_have_dms = false;
static msm_ipc_server_info g_loc, g_dms;
static uint16_t g_txn = 1;
static bool g_running = false;

static uint8_t* g_xtra;
static size_t g_xtra_len;
static int g_xtra_part;  // next part to send, 1-based; 0 = idle

bool gps_open() {
  g_fd = socket(AF_MSM_IPC, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (g_fd < 0) klog("gps: AF_MSM_IPC socket: %s", strerror(errno));
  return g_fd >= 0;
}

int gps_fd() { return g_fd; }
bool gps_connected() { return g_have_loc; }
bool gps_radio_low_power() { return g_have_dms; }
bool gps_running() { return g_running; }

// Asks the IPC router's name service where a QMI service (DMS, LOC) lives:
// returns its node/port once the modem has registered it.
static bool lookup(uint32_t service, msm_ipc_server_info* out) {
  server_lookup_args a = {};
  a.port_name.service = service;
  a.port_name.instance = 0;
  a.lookup_mask = 0;  // any instance / version
  a.num_entries_in_array = 1;
  if (ioctl(g_fd, IPC_ROUTER_IOCTL_LOOKUP_SERVER, &a) < 0 || a.num_entries_found < 1) return false;
  *out = a.srv_info[0];
  return true;
}

// Builds one QMI request body: TLVs of {u8 type, u16 length, value}, after
// room left for the QmiHeader (filled in when the request is sent).
struct Msg {
  uint8_t buf[256];
  size_t len = sizeof(QmiHeader);

  void tlv(uint8_t type, const void* v, uint16_t n) {
    buf[len] = type;
    memcpy(buf + len + 1, &n, 2);
    memcpy(buf + len + 3, v, n);
    len += 3 + n;
  }
  void u8(uint8_t type, uint8_t v) { tlv(type, &v, 1); }
  void u32(uint8_t type, uint32_t v) { tlv(type, &v, 4); }
  void u64(uint8_t type, uint64_t v) { tlv(type, &v, 8); }
};

// One request in flight per service, the next sent when its response
// arrives -- as Android's synchronous client does. The IPC router refuses
// (EAGAIN) a burst of unanswered messages to the same port.
struct Pending {
  msm_ipc_server_info srv;
  uint16_t msg_id;
  uint16_t len;
  uint8_t buf[sizeof(QmiHeader) + 64 + 1024];
};
static const int kQueue = 48;
static const int64_t kReplyTimeoutMs = 2000;
static Pending g_queue[kQueue];
static int g_qhead, g_qcount;
static bool g_in_flight;
static int64_t g_sent_ms;

// Sends the next queued request unless one is still waiting for its
// response (given up after kReplyTimeoutMs, so a lost reply can't stall it).
static void pump() {
  if (g_in_flight && now_mono_ms() - g_sent_ms < kReplyTimeoutMs) return;
  g_in_flight = false;
  while (g_qcount > 0) {
    Pending& p = g_queue[g_qhead];
    QmiHeader h = {QMI_REQ, g_txn++, p.msg_id, uint16_t(p.len - sizeof(QmiHeader))};
    memcpy(p.buf, &h, sizeof(h));
    sockaddr_msm_ipc to = {};
    to.family = AF_MSM_IPC;
    to.address.addrtype = MSM_IPC_ADDR_ID;
    to.address.addr.port_addr.node_id = p.srv.node_id;
    to.address.addr.port_addr.port_id = p.srv.port_id;
    if (sendto(g_fd, p.buf, p.len, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) < 0) {
      if (errno == EAGAIN) return;  // router busy: retry on the next message or timeout
      klog("gps: send 0x%04x: %s", p.msg_id, strerror(errno));
    } else {
      g_in_flight = true;
      g_sent_ms = now_mono_ms();
    }
    g_qhead = (g_qhead + 1) % kQueue;
    g_qcount--;
    if (g_in_flight) return;
  }
}

// Queues a request (TLV body without header) and sends it if the line is free.
static bool queue_req(const msm_ipc_server_info& srv, uint16_t msg_id, const uint8_t* body, size_t n) {
  if (g_qcount == kQueue || n > sizeof(Pending::buf) - sizeof(QmiHeader)) {
    klog("gps: queue full, dropped 0x%04x", msg_id);
    return false;
  }
  Pending& p = g_queue[(g_qhead + g_qcount) % kQueue];
  p.srv = srv;
  p.msg_id = msg_id;
  p.len = uint16_t(sizeof(QmiHeader) + n);
  memcpy(p.buf + sizeof(QmiHeader), body, n);
  g_qcount++;
  pump();
  return true;
}

static bool send_req(const msm_ipc_server_info& srv, uint16_t msg_id, Msg& m) {
  return queue_req(srv, msg_id, m.buf + sizeof(QmiHeader), m.len - sizeof(QmiHeader));
}

// Android's setup sequence, replayed byte for byte (captured with strace).
// Names from the order of loc_eng_reinit() in the HAL source where known.
struct InitReq {
  uint16_t msg_id;
  uint8_t len;
  uint8_t tlvs[48];
};
static const InitReq kInitSeq[] = {
    {LOC_REG_EVENTS, 11, {0x01, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {LOC_REG_EVENTS, 11, {0x01, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0}},  // start with no events
    {0x001E, 0, {}},  // get supported messages
    {0x0084, 14, {0x01, 4, 0, 0, 0, 0, 0, 0x02, 4, 0, 0, 0, 0, 0}},  // newer config message,
    {0x0084, 14, {0x01, 4, 0, 1, 0, 0, 0, 0x02, 4, 0, 2, 0, 0, 0}},  // meaning not documented
    {LOC_REG_EVENTS, 11, {0x01, 8, 0, 0x78, 0xEE, 0x67, 0, 0, 0, 0, 0}},  // LOC_EVENTS_IDLE
    {0x0088, 11, {0x10, 8, 0, 1, 0, 0, 0, 0, 0, 0, 0}},  // not documented either
    {0x003A, 7, {0x01, 4, 0, 1, 0, 0, 0}},  // engine lock: none
    {0x0054, 7, {0x12, 4, 0, 2, 0, 0, 0}},  // protocol config: SUPL version
    {0x0054, 7, {0x13, 4, 0, 0, 0, 0, 0}},  // protocol config: LPP
    {0x0056, 14, {0x10, 4, 0, 0, 0, 0, 0, 0x11, 4, 0, 1, 0, 0, 0}},  // sensor control config
    {0x0054, 7, {0x14, 4, 0, 0, 0, 0, 0}},  // protocol config: A-GLONASS
    // Sensor performance config: accel/gyro samples per batch and batches/s.
    {0x005A, 42, {0x10, 4, 0, 0, 0, 0, 0, 0x11, 4, 0, 5, 0, 2, 0, 0x12, 4, 0, 5, 0, 2, 0,
                  0x13, 4, 0, 0, 0, 0, 0, 0x14, 4, 0, 0x19, 0, 4, 0, 0x15, 4, 0, 0x19, 0, 4, 0}},
    {0x0085, 7, {0x01, 4, 0, 1, 0, 0, 0}},  // XTRA version check: auto
    // On "location on": unlock again and name Google's SUPL server (assistance
    // over mobile data; unused here, sent for fidelity).
    {0x003A, 7, {0x01, 4, 0, 1, 0, 0, 0}},
    {0x0042, 30, {0x01, 4, 0, 3, 0, 0, 0, 0x12, 20, 0, 's', 'u', 'p', 'l', '.', 'g', 'o', 'o', 'g',
                  'l', 'e', '.', 'c', 'o', 'm', ':', '7', '2', '7', '5'}},
};

// Which indications the modem should send us (LOC_EVENTS_*).
static void reg_events(uint64_t mask) {
  Msg m;
  m.u64(0x01, mask);
  send_req(g_loc, LOC_REG_EVENTS, m);
}

// Reads the whole XTRA file into memory (27-35 KB); injected later, on request.
bool gps_load_xtra(const char* path) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    klog("gps: no XTRA file %s", path);
    return false;
  }
  off_t n = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);
  if (n <= 0 || n > 512 * 1024) {
    close(fd);
    return false;
  }
  g_xtra = static_cast<uint8_t*>(malloc(n));
  g_xtra_len = g_xtra && read(fd, g_xtra, n) == n ? size_t(n) : 0;
  close(fd);
  klog("gps: XTRA %s, %zu bytes", path, g_xtra_len);
  return g_xtra_len > 0;
}

// One 1 KB part of QMI_LOC_INJECT_PREDICTED_ORBITS_DATA; the next goes on the response.
static void send_xtra_part() {
  uint16_t nparts = uint16_t((g_xtra_len + kXtraPart - 1) / kXtraPart);
  if (g_xtra_part < 1 || g_xtra_part > nparts) {
    g_xtra_part = 0;
    return;
  }
  size_t off = size_t(g_xtra_part - 1) * kXtraPart;
  uint16_t len = uint16_t(g_xtra_len - off < kXtraPart ? g_xtra_len - off : kXtraPart);
  static uint8_t buf[64 + kXtraPart];
  size_t p = 0;
  auto tlv = [&](uint8_t t, const void* v, uint16_t n) {
    buf[p] = t;
    memcpy(buf + p + 1, &n, 2);
    memcpy(buf + p + 3, v, n);
    p += 3 + n;
  };
  uint32_t total = uint32_t(g_xtra_len), format = 0;  // 0 = XTRA
  uint16_t part = uint16_t(g_xtra_part);
  tlv(0x01, &total, 4);
  tlv(0x02, &nparts, 2);
  tlv(0x03, &part, 2);
  buf[p] = 0x04;  // part data: u16 length, then the bytes
  uint16_t tl = uint16_t(2 + len);
  memcpy(buf + p + 1, &tl, 2);
  memcpy(buf + p + 3, &len, 2);
  memcpy(buf + p + 5, g_xtra + off, len);
  p += 5 + len;
  tlv(0x10, &format, 4);
  if (!queue_req(g_loc, LOC_INJECT_XTRA_DATA, buf, p)) g_xtra_part = 0;
}

// QMI_LOC_INJECT_UTC_TIME with the system clock, as Android does with NTP
// time. Skipped while the clock is unset (still near 1970).
void gps_inject_time() {
  if (!g_have_loc) return;
  timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1600000000) return;  // clock not set yet
  Msg m;
  m.u64(0x01, uint64_t(tv.tv_sec) * 1000 + tv.tv_usec / 1000);
  m.u32(0x02, kTimeUncMs);
  send_req(g_loc, LOC_INJECT_UTC_TIME, m);
  klog("gps: injected UTC time");
}

// Looks up DMS (puts the cellular radio in low power once found) and LOC.
// The first time LOC is found, replays Android's setup and injects the time.
bool gps_connect() {
  if (g_fd < 0) return false;
  if (!g_have_dms && lookup(kSvcDms, &g_dms)) {
    g_have_dms = true;
    Msg m;
    m.u8(0x01, DMS_MODE_LOW_POWER);
    send_req(g_dms, DMS_SET_OPERATING_MODE, m);
    klog("gps: DMS at %u:%u, radio -> low power", g_dms.node_id, g_dms.port_id);
  }
  if (!g_have_loc && lookup(kSvcLoc, &g_loc)) {
    g_have_loc = true;
    klog("gps: LOC at %u:%u", g_loc.node_id, g_loc.port_id);
    for (const auto& r : kInitSeq) {
      Msg m;
      memcpy(m.buf + m.len, r.tlvs, r.len);
      m.len += r.len;
      send_req(g_loc, r.msg_id, m);
    }
    gps_inject_time();
  }
  return g_have_loc;
}

// Starts a periodic session the way Android does: session events on, MSB
// mode (uses injected time/assistance), high accuracy, altitude not assumed.
void gps_start(uint32_t interval_ms) {
  if (!g_have_loc) return;
  reg_events(LOC_EVENTS_SESSION);
  Msg mode;
  mode.u32(0x01, LOC_OPERATION_MSB);
  send_req(g_loc, LOC_SET_OPERATION_MODE, mode);
  Msg m;
  m.u8(0x01, kSession);
  m.u32(0x10, LOC_RECURRENCE_PERIODIC);
  m.u32(0x11, LOC_ACCURACY_HIGH);
  m.u32(0x13, interval_ms);
  m.u32(0x15, LOC_ALTITUDE_ASSUMED_DISABLED);
  if (send_req(g_loc, LOC_START, m)) {
    g_running = true;
    klog("gps: start, every %u ms", interval_ms);
  }
}

// Stops the session and drops back to the idle event mask.
void gps_stop() {
  if (!g_have_loc || !g_running) return;
  Msg m;
  m.u8(0x01, kSession);
  send_req(g_loc, LOC_STOP, m);
  reg_events(LOC_EVENTS_IDLE);
  g_running = false;
  klog("gps: stop");
}

// Walks the TLVs of one message.
template <typename F>
static void each_tlv(const uint8_t* p, size_t n, F f) {
  while (n >= 3) {
    uint8_t type = p[0];
    uint16_t len;
    memcpy(&len, p + 1, 2);
    if (len > n - 3) return;
    f(type, p + 3, len);
    p += 3 + len;
    n -= 3 + len;
  }
}

// QMI_LOC_EVENT_POSITION_REPORT_IND. TLVs used: 0x01 session status (0 = fix,
// 1 = in progress, 3 = timeout), 0x10/0x11 lat/lon (double), 0x12 horizontal
// uncertainty (m), 0x1B altitude above sea level, 0x1C its uncertainty,
// 0x25 UTC time (ms), 0x2C satellites used.
static void parse_position(const uint8_t* p, size_t n, const GpsFixCb& on_fix) {
  GpsFix fix = {};
  uint32_t status = ~0u;
  bool have_lat = false, have_lon = false;
  each_tlv(p, n, [&](uint8_t t, const uint8_t* v, uint16_t len) {
    switch (t) {
      case 0x01: if (len >= 4) memcpy(&status, v, 4); break;
      case 0x10: if (len >= 8) { memcpy(&fix.lat, v, 8); have_lat = true; } break;
      case 0x11: if (len >= 8) { memcpy(&fix.lon, v, 8); have_lon = true; } break;
      case 0x12: if (len >= 4) memcpy(&fix.hunc_m, v, 4); break;
      case 0x1B: if (len >= 4) { memcpy(&fix.alt_msl_m, v, 4); fix.have_alt = true; } break;
      case 0x1C: if (len >= 4) memcpy(&fix.vunc_m, v, 4); break;
      case 0x25: if (len >= 8) memcpy(&fix.utc_ms, v, 8); break;
      case 0x2C: if (len >= 1) fix.sats_used = v[0]; break;
    }
  });
  fix.status = status;
  fix.valid = status == LOC_SESSION_SUCCESS && have_lat && have_lon;
  on_fix(fix);
}

// QMI_LOC_EVENT_GNSS_SV_INFO_IND: counts satellites listed and those with a
// usable signal (SNR > 10 dB-Hz).
static void parse_sv_info(const uint8_t* p, size_t n, const GpsSkyCb& on_sky) {
  GpsSky sky = {};
  each_tlv(p, n, [&](uint8_t t, const uint8_t* v, uint16_t len) {
    if (t != 0x10 || len < 1) return;
    // u8 count, then {u32 valid, u32 system, u16 id, u8 health, u32 status,
    // u8 nav, float elev, float az, float snr} packed = 28 bytes each.
    const size_t kSize = 28;
    int count = v[0];
    for (int i = 0; i < count && 1 + (i + 1) * kSize <= len; ++i) {
      float snr;
      memcpy(&snr, v + 1 + i * kSize + 24, 4);
      sky.in_view++;
      if (snr > 10.0f) sky.heard++;
    }
  });
  on_sky(sky);
}

// Drains the socket. Responses free the request line (and drive the XTRA
// parts); indications are the modem's requests for time/XTRA, results of our
// requests, engine state, positions and satellite lists.
void gps_read(const GpsFixCb& on_fix, const GpsSkyCb& on_sky) {
  uint8_t buf[2048];
  for (;;) {
    ssize_t n = recv(g_fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      pump();  // also retries after an EAGAIN or a lost response
      return;
    }
    if (n < (ssize_t)sizeof(QmiHeader)) {
      klog("gps: short message (%zd bytes)", n);
      continue;
    }
    QmiHeader h;
    memcpy(&h, buf, sizeof(h));
    const uint8_t* body = buf + sizeof(h);
    size_t len = n - sizeof(h);
    if (len > h.len) len = h.len;

    // Indications of requests carry TLV 0x01 u32 status (0 = success).
    uint32_t ind_status = ~0u;
    if (h.type == QMI_IND)
      each_tlv(body, len, [&](uint8_t t, const uint8_t* v, uint16_t l) {
        if (t == 0x01 && l >= 4) memcpy(&ind_status, v, 4);
      });

    if (h.type == QMI_RESP) {
      g_in_flight = false;
      // TLV 0x02 {u16 result, u16 error}: result 1 = failed. (This modem
      // puts a nonzero "error" in successful replies too, Android's included.)
      each_tlv(body, len, [&](uint8_t t, const uint8_t* v, uint16_t l) {
        uint16_t result, error;
        if (t != 0x02 || l < 4) return;
        memcpy(&result, v, 2);
        memcpy(&error, v + 2, 2);
        if (result != 0) klog("gps: 0x%04x refused, error %u", h.msg_id, error);
      });
      if (h.msg_id == LOC_INJECT_XTRA_DATA && g_xtra_part) {
        g_xtra_part++;
        send_xtra_part();
      }
      pump();
    } else if (h.type == QMI_IND && h.msg_id == LOC_IND_INJECT_TIME_REQ) {
      klog("gps: modem asks for time");
      gps_inject_time();
    } else if (h.type == QMI_IND && h.msg_id == LOC_IND_INJECT_XTRA_REQ) {
      if (g_xtra_len && !g_xtra_part) {
        klog("gps: modem asks for XTRA, injecting %zu bytes", g_xtra_len);
        g_xtra_part = 1;
        send_xtra_part();
      }
    } else if (h.type == QMI_IND && h.msg_id == LOC_INJECT_XTRA_DATA) {
      uint16_t nparts = uint16_t((g_xtra_len + kXtraPart - 1) / kXtraPart), part = 0;
      each_tlv(body, len, [&](uint8_t t, const uint8_t* v, uint16_t l) {
        if (t == 0x10 && l >= 2) memcpy(&part, v, 2);
      });
      // Part 0 is the verdict on the whole file. On this phone's modem
      // firmware today's files fail it with status 10 (version check),
      // exactly as they do under Android; time injection still helps.
      if (ind_status != 0) klog("gps: XTRA part %u rejected, status %u", part, ind_status);
      else if (part == nparts) klog("gps: XTRA accepted (%u parts)", nparts);
    } else if (h.type == QMI_IND && h.msg_id == LOC_INJECT_UTC_TIME) {
      klog("gps: time injection status %u", ind_status);
    } else if (h.type == QMI_IND && h.msg_id == LOC_IND_ENGINE_STATE) {
      klog("gps: engine %s", ind_status == 1 ? "on" : ind_status == 2 ? "off" : "?");
    } else if (h.type == QMI_IND && h.msg_id != LOC_IND_POSITION && h.msg_id != LOC_IND_SV_INFO &&
               h.msg_id != LOC_IND_NMEA && ind_status != 0 && ind_status != ~0u) {
      klog("gps: 0x%04x failed, status %u", h.msg_id, ind_status);
    } else if (h.type == QMI_IND && h.msg_id == LOC_IND_POSITION) {
      parse_position(body, len, on_fix);
    } else if (h.type == QMI_IND && h.msg_id == LOC_IND_SV_INFO) {
      parse_sv_info(body, len, on_sky);
    }
  }
}
