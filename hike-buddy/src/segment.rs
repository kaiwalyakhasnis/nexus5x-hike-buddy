//! Seven-segment numerals drawn with gr_fill: crisp at any size, no font
//! files, and the watch-face look fits a hiking console.

use crate::gr::fill;
use std::ffi::{c_char, CStr};

//  aaa
// f   b
//  ggg
// e   c
//  ddd
const A: u32 = 1;
const B: u32 = 2;
const C: u32 = 4;
const D: u32 = 8;
const E: u32 = 16;
const F: u32 = 32;
const G: u32 = 64;

/// Segments lit per digit: bit 0 = a ... bit 6 = g (the usual 7-segment table).
const DIGIT: [u32; 10] = [0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f];

/// Lights the segments in mask m in a w x h cell with stroke t.
fn draw_segments(x: i32, y: i32, w: i32, h: i32, t: i32, m: u32) {
    let mid = y + (h - t) / 2;
    let gap = t / 4; // small gaps between segments read as "LCD"
    if m & A != 0 {
        fill(x + t, y, x + w - t, y + t);
    }
    if m & G != 0 {
        fill(x + t, mid, x + w - t, mid + t);
    }
    if m & D != 0 {
        fill(x + t, y + h - t, x + w - t, y + h);
    }
    if m & F != 0 {
        fill(x, y + t + gap, x + t, mid - gap);
    }
    if m & B != 0 {
        fill(x + w - t, y + t + gap, x + w, mid - gap);
    }
    if m & E != 0 {
        fill(x, mid + t + gap, x + t, y + h - t - gap);
    }
    if m & C != 0 {
        fill(x + w - t, mid + t + gap, x + w, y + h - t - gap);
    }
}

/// Horizontal space a glyph takes: '.' and ':' are narrow.
fn advance(c: u8, w: i32, t: i32) -> i32 {
    if c == b'.' || c == b':' {
        t * 2
    } else {
        w + t
    }
}

/// Width seg_text would draw `s` at.
pub fn measure(w: i32, t: i32, s: &[u8]) -> i32 {
    s.iter().map(|&c| advance(c, w, t)).sum()
}

/// Draws s (digits, '-', '.', ':', ' ') with its top-left corner at x,y;
/// w,h = one digit cell, t = stroke thickness. Returns the x just past the
/// last glyph.
pub fn text(mut x: i32, y: i32, w: i32, h: i32, t: i32, s: &[u8]) -> i32 {
    for &c in s {
        match c {
            b'0'..=b'9' => draw_segments(x, y, w, h, t, DIGIT[usize::from(c - b'0')]),
            b'-' => draw_segments(x, y, w, h, t, G),
            b'.' => fill(x, y + h - t, x + t, y + h),
            b':' => {
                fill(x, y + h / 3 - t / 2, x + t, y + h / 3 + t / 2);
                fill(x, y + 2 * h / 3 - t / 2, x + t, y + 2 * h / 3 + t / 2);
            }
            _ => {}
        }
        x += advance(c, w, t);
    }
    x
}

/// # Safety
/// `s` must be a valid NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn seg_text(x: i32, y: i32, w: i32, h: i32, t: i32, s: *const c_char) -> i32 {
    text(x, y, w, h, t, CStr::from_ptr(s).to_bytes())
}

/// # Safety
/// `s` must be a valid NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn seg_measure(w: i32, t: i32, s: *const c_char) -> i32 {
    measure(w, t, CStr::from_ptr(s).to_bytes())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn widths() {
        assert_eq!(measure(10, 2, b"12.5"), 12 + 12 + 4 + 12);
        assert_eq!(measure(10, 2, b""), 0);
    }
}
