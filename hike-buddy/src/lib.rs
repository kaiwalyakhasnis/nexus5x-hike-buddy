//! hike-buddy, being migrated from C++ to Rust module by module. Everything
//! here is called from the C++ side through `extern "C"` functions.
//!
//! Constraint: this runs on a 3.10 kernel. Avoid std APIs that need newer
//! syscalls (HashMap's random seed uses getrandom, fs metadata uses statx):
//! the calls fall back fine but the kernel logs a register dump for each.

pub mod altitude;
pub mod draw;
mod gr;
pub mod segment;

/// Version string of the Rust part, NUL-terminated, for the startup log.
#[no_mangle]
pub extern "C" fn hb_rust_version() -> *const core::ffi::c_char {
    concat!("hike-buddy rust ", env!("CARGO_PKG_VERSION"), "\0")
        .as_ptr()
        .cast()
}
