//! The parts of minui (C++, minui/) that the Rust side draws with, called
//! through the extern "C" wrappers in minui_c.cpp.

#[cfg(not(test))]
extern "C" {
    fn hb_gr_fill(x1: i32, y1: i32, x2: i32, y2: i32);
}

/// Fills [x1, x2) x [y1, y2) with minui's current colour. minui skips the
/// whole rectangle if any corner is off the surface.
pub fn fill(x1: i32, y1: i32, x2: i32, y2: i32) {
    // SAFETY: plain values; minui draws into its current target.
    unsafe { hb_gr_fill(x1, y1, x2, y2) }
}

// Unit tests have no minui: record the fills instead.
#[cfg(test)]
pub static FILLS: std::sync::Mutex<Vec<(i32, i32, i32, i32)>> = std::sync::Mutex::new(Vec::new());

#[cfg(test)]
unsafe fn hb_gr_fill(x1: i32, y1: i32, x2: i32, y2: i32) {
    FILLS.lock().unwrap().push((x1, y1, x2, y2));
}
