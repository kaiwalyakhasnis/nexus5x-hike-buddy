//! The C++ helpers in sys.cpp that the Rust side still uses: the boot clock
//! and the kernel log. (sys.cpp moves to Rust with main.)

use std::ffi::{c_char, CString};

#[cfg(not(test))]
extern "C" {
    fn hb_now_boot_ms() -> i64;
    fn hb_klog(msg: *const c_char);
}

/// CLOCK_BOOTTIME in ms: keeps counting while the phone is suspended.
pub fn now_boot_ms() -> i64 {
    // SAFETY: no arguments.
    unsafe { hb_now_boot_ms() }
}

/// One line to the kernel log, prefixed "hike-buddy: " (dmesg).
pub fn klog(msg: &str) {
    if let Ok(c) = CString::new(msg) {
        // SAFETY: c is a valid NUL-terminated string for the call.
        unsafe { hb_klog(c.as_ptr()) }
    }
}

/// strerror() text for the current errno, as the C++ side logs it.
pub fn last_os_error_text() -> String {
    let errno = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
    // SAFETY: strerror returns a valid C string (static or thread-local).
    unsafe { std::ffi::CStr::from_ptr(libc::strerror(errno)) }
        .to_string_lossy()
        .into_owned()
}

#[cfg(test)]
unsafe fn hb_now_boot_ms() -> i64 {
    1_000_000
}

#[cfg(test)]
unsafe fn hb_klog(_msg: *const c_char) {}
