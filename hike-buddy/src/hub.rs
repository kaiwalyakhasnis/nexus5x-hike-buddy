//! Minimal client for the nanohub sensor hub (/dev/nanohub), speaking the
//! same wire protocol as device/google/contexthub/sensorhal/hubconnection.cpp.
//! Ported from hub.cpp; tests/hub_golden.cpp checks it decodes every packet,
//! writes every config byte and logs every line exactly as the C++ did.

use crate::sys::{klog, last_os_error_text, now_boot_ms};
use std::ffi::{c_char, c_int, c_void, CStr};
use std::sync::atomic::{AtomicI32, Ordering};
use std::sync::Mutex;

// firmware/os/inc/eventnums.h
const EVT_NO_FIRST_SENSOR_EVENT: u32 = 0x200;
const EVT_NO_SENSOR_CONFIG_EVENT: u32 = 0x300;

const CONFIG_CMD_DISABLE: u8 = 0;
const CONFIG_CMD_ENABLE: u8 = 1;
const CONFIG_CMD_FLUSH: u8 = 2;

// Sensor types from contexthub firmware/os/inc/sensType.h (also in hub.h).
pub const SENS_TYPE_BARO: u8 = 10; // BMP280, hPa
pub const SENS_TYPE_GEO_MAG_ROT_VEC: u8 = 20; // accel + mag fused on the hub, no gyro
pub const SENS_TYPE_STEP_COUNT: u8 = 22;

// Packet layout read from /dev/nanohub: u32 event type, u64 reference time,
// then samples. Sample 0's 4-byte time slot holds the FirstSample header
// (numSamples is its first byte); later ones hold the delta to the previous.
const HEADER_LEN: usize = 16; // evtType + referenceTime + FirstSample
const SAMPLES_AT: usize = 12; // evtType + referenceTime
const ONE_AXIS_LEN: usize = 8; // u32 deltaTime, f32/u32 value
const THREE_AXIS_LEN: usize = 16; // u32 deltaTime, f32 x, y, z

/// One sample, as hub.h's struct HubSample (same C layout).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct HubSample {
    pub sens_type: u8,
    pub t_boot_s: f64, // sample time on CLOCK_BOOTTIME, seconds
    pub v: [f32; 3],   // three-axis sensors
    pub f: f32,        // one-axis float sensors (pressure)
    pub i: u32,        // one-axis integer sensors (step count)
}

static FD: AtomicI32 = AtomicI32::new(-1);

/// Packet counts per event type, for log_stats(); first 8 types seen.
const MAX_STATS: usize = 8;
static STATS: Mutex<[(u32, i32); MAX_STATS]> = Mutex::new([(0, 0); MAX_STATS]);

fn rd_u32(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(b[at..at + 4].try_into().unwrap())
}

fn rd_f32(b: &[u8], at: usize) -> f32 {
    f32::from_le_bytes(b[at..at + 4].try_into().unwrap())
}

fn count_packet(evt_type: u32) {
    let mut stats = STATS.lock().unwrap();
    for s in stats.iter_mut() {
        if s.1 == 0 {
            s.0 = evt_type;
        }
        if s.0 == evt_type {
            s.1 += 1;
            return;
        }
    }
}

/// Logs packets received per event type since the last call (diagnostics).
pub fn log_stats(tag: &str) {
    let mut stats = STATS.lock().unwrap();
    let list: String = stats
        .iter()
        .take_while(|s| s.1 != 0)
        .map(|s| format!(" 0x{:x}:{}", s.0, s.1))
        .collect();
    klog(&format!(
        "hub packets {tag}:{}",
        if list.is_empty() { " none" } else { &list }
    ));
    *stats = [(0, 0); MAX_STATS];
}

pub fn open() -> bool {
    // SAFETY: valid NUL-terminated path.
    let fd = unsafe {
        libc::open(
            c"/dev/nanohub".as_ptr(),
            libc::O_RDWR | libc::O_CLOEXEC | libc::O_NONBLOCK,
        )
    };
    if fd < 0 {
        klog(&format!("open /dev/nanohub: {}", last_os_error_text()));
    }
    FD.store(fd, Ordering::Relaxed);
    fd >= 0
}

pub fn fd() -> i32 {
    FD.load(Ordering::Relaxed)
}

/// The 20-byte packed ConfigCmd: rate in Hz x 1024 (sensor_hz), latency in
/// ns (how long the hub may batch before waking us), flags 0.
fn config_bytes(sens_type: u8, cmd: u8, rate: u32, latency: u64) -> [u8; 20] {
    let mut c = [0u8; 20];
    c[0..4].copy_from_slice(&EVT_NO_SENSOR_CONFIG_EVENT.to_le_bytes());
    c[4..12].copy_from_slice(&latency.to_le_bytes());
    c[12..16].copy_from_slice(&rate.to_le_bytes());
    c[16] = sens_type;
    c[17] = cmd;
    c
}

/// Writes one config command (enable, disable or flush) and logs it.
fn send_config(sens_type: u8, cmd: u8, rate: u32, latency: u64) -> bool {
    let fd = fd();
    if fd < 0 {
        return false;
    }
    let c = config_bytes(sens_type, cmd, rate, latency);
    // SAFETY: c is a valid buffer of c.len() bytes.
    let n = unsafe { libc::write(fd, c.as_ptr().cast(), c.len()) };
    if n != c.len() as isize {
        klog(&format!(
            "hub config type {sens_type} cmd {cmd}: {}",
            last_os_error_text()
        ));
        return false;
    }
    klog(&format!(
        "hub config type {sens_type} cmd {cmd} rate {rate} latency {}ms",
        latency / 1_000_000
    ));
    true
}

/// latency_ns > 0 lets the hub batch samples and wake us once per latency.
pub fn config(sens_type: u8, enable: bool, rate_q10: u32, latency_ns: u64) -> bool {
    let cmd = if enable {
        CONFIG_CMD_ENABLE
    } else {
        CONFIG_CMD_DISABLE
    };
    send_config(sens_type, cmd, rate_q10, latency_ns)
}

pub fn flush(sens_type: u8) -> bool {
    send_config(sens_type, CONFIG_CMD_FLUSH, 0, 0)
}

/// Decodes one packet. Hub timestamps are on the hub's clock; only their
/// spacing is trusted, and the newest sample of each packet is pinned to
/// `now_s`. Batches leave the hub as soon as the latency expires, so the
/// error is at most one sample period.
pub fn parse(buf: &[u8], now_s: f64, cb: &mut dyn FnMut(&HubSample)) {
    if buf.len() < HEADER_LEN {
        return;
    }
    let evt_type = rd_u32(buf, 0);
    let sens = evt_type.wrapping_sub(EVT_NO_FIRST_SENSOR_EVENT) as u8;
    let three = if evt_type == EVT_NO_FIRST_SENSOR_EVENT + u32::from(SENS_TYPE_GEO_MAG_ROT_VEC) {
        true
    } else if evt_type == EVT_NO_FIRST_SENSOR_EVENT + u32::from(SENS_TYPE_STEP_COUNT)
        || evt_type == EVT_NO_FIRST_SENSOR_EVENT + u32::from(SENS_TYPE_BARO)
    {
        false
    } else {
        return; // logs, other sensors: not ours
    };

    let stride = if three { THREE_AXIS_LEN } else { ONE_AXIS_LEN };
    let fit = (buf.len() - SAMPLES_AT) / stride;
    let n = usize::from(buf[SAMPLES_AT]).min(fit);
    if n == 0 {
        return;
    }

    // Hub-relative timestamps (sensorhal delta decoding: low bit set = raw
    // ns, otherwise the delta is in units of 512 ns).
    let mut ts = [0u64; 256];
    let mut t = u64::from_le_bytes(buf[4..12].try_into().unwrap());
    for (k, slot) in ts.iter_mut().enumerate().take(n) {
        if k > 0 {
            let d = rd_u32(buf, SAMPLES_AT + k * stride);
            t = t.wrapping_add(u64::from(d) << if d & 1 != 0 { 0 } else { 9 });
        }
        *slot = t;
    }

    for k in 0..n {
        let at = SAMPLES_AT + k * stride;
        let mut s = HubSample {
            sens_type: sens,
            t_boot_s: now_s - ts[n - 1].wrapping_sub(ts[k]) as f64 / 1e9,
            ..Default::default()
        };
        if three {
            s.v = [
                rd_f32(buf, at + 4),
                rd_f32(buf, at + 8),
                rd_f32(buf, at + 12),
            ];
        } else {
            s.i = rd_u32(buf, at + 4);
            s.f = rd_f32(buf, at + 4);
        }
        cb(&s);
    }
}

/// What read() does with each packet: count it, decode it.
fn feed(buf: &[u8], cb: &mut dyn FnMut(&HubSample)) {
    if buf.len() >= 4 {
        count_packet(rd_u32(buf, 0));
    }
    parse(buf, now_boot_ms() as f64 / 1000.0, cb);
}

/// Reads what is pending (the fd is non-blocking) and calls cb per sample.
pub fn read(cb: &mut dyn FnMut(&HubSample)) {
    let mut buf = [0u8; 512];
    loop {
        // SAFETY: buf is valid for buf.len() bytes.
        let n = unsafe { libc::read(fd(), buf.as_mut_ptr().cast(), buf.len()) };
        if n <= 0 {
            return; // EAGAIN: drained
        }
        feed(&buf[..n as usize], cb);
    }
}

// --- C interface (hub.h) ---------------------------------------------------------

pub type SampleFn = extern "C" fn(ctx: *mut c_void, s: *const HubSample);

/// Safety contract for every function here: pointers are valid for the
/// call (buf for len bytes, tag NUL-terminated), and `cb` may be called with
/// `ctx` until the call returns. Single-threaded use (the UI thread).
#[allow(clippy::missing_safety_doc)]
pub mod ffi {
    use super::*;

    #[no_mangle]
    pub extern "C" fn hb_hub_open() -> bool {
        open()
    }

    #[no_mangle]
    pub extern "C" fn hb_hub_fd() -> c_int {
        fd()
    }

    #[no_mangle]
    pub extern "C" fn hb_hub_config(
        sens: u8,
        enable: bool,
        rate_q10: u32,
        latency_ns: u64,
    ) -> bool {
        config(sens, enable, rate_q10, latency_ns)
    }

    #[no_mangle]
    pub extern "C" fn hb_hub_flush(sens: u8) -> bool {
        flush(sens)
    }

    #[no_mangle]
    pub extern "C" fn hb_hub_read(cb: SampleFn, ctx: *mut c_void) {
        read(&mut |s| cb(ctx, s));
    }

    #[no_mangle]
    pub unsafe extern "C" fn hb_hub_log_stats(tag: *const c_char) {
        log_stats(&CStr::from_ptr(tag).to_string_lossy());
    }

    /// Test hook (tests/hub_golden.cpp): one packet through read()'s path.
    #[no_mangle]
    pub unsafe extern "C" fn hb_hub_test_feed(
        buf: *const u8,
        len: usize,
        cb: SampleFn,
        ctx: *mut c_void,
    ) {
        feed(std::slice::from_raw_parts(buf, len), &mut |s| cb(ctx, s));
    }

    /// Test hook: use this fd instead of /dev/nanohub.
    #[no_mangle]
    pub extern "C" fn hb_hub_test_use_fd(fd: c_int) {
        FD.store(fd, Ordering::Relaxed);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn packet(evt: u32, ref_t: u64, samples: &[(u32, [f32; 3])], three: bool) -> Vec<u8> {
        let mut b = evt.to_le_bytes().to_vec();
        b.extend_from_slice(&ref_t.to_le_bytes());
        for (k, (d, v)) in samples.iter().enumerate() {
            if k == 0 {
                b.extend_from_slice(&[samples.len() as u8, 0, 0, 0]);
            } else {
                b.extend_from_slice(&d.to_le_bytes());
            }
            for x in v.iter().take(if three { 3 } else { 1 }) {
                b.extend_from_slice(&x.to_le_bytes());
            }
        }
        b
    }

    #[test]
    fn decodes_pressure_with_timestamps() {
        // deltas: 1953125 * 512 ns = 1 s (even: x512), and 3 (odd: raw ns)
        let b = packet(
            0x20a,
            0,
            &[
                (0, [1000.0; 3]),
                (1953125 & !1, [1001.0; 3]),
                (3, [1002.0; 3]),
            ],
            false,
        );
        let mut got = vec![];
        parse(&b, 100.0, &mut |s| got.push(*s));
        assert_eq!(got.len(), 3);
        assert_eq!(got[2].t_boot_s, 100.0);
        assert!((got[0].t_boot_s - (100.0 - (1953124.0 * 512.0 + 3.0) / 1e9)).abs() < 1e-12);
        assert_eq!((got[0].sens_type, got[1].f), (SENS_TYPE_BARO, 1001.0));
    }

    #[test]
    fn ignores_other_types_and_short_packets() {
        let mut n = 0;
        parse(&packet(0x201, 0, &[(0, [1.0; 3])], false), 0.0, &mut |_| {
            n += 1
        });
        parse(&[0u8; 15], 0.0, &mut |_| n += 1);
        assert_eq!(n, 0);
    }

    #[test]
    fn config_layout() {
        let c = config_bytes(22, 1, 17, 300_000_000_000);
        assert_eq!(&c[0..4], &[0, 3, 0, 0]);
        assert_eq!(&c[16..20], &[22, 1, 0, 0]);
    }
}
