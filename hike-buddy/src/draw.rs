//! Small drawing helpers on top of minui's gr_fill; all use the current
//! colour. Integer maths as in the C++ original (division truncates toward
//! zero in both), so the pixels are identical (tests/draw_golden.cpp).

use crate::gr::fill;

/// Line of thickness t (square brush).
#[no_mangle]
pub extern "C" fn draw_line(x0: i32, y0: i32, x1: i32, y1: i32, t: i32) {
    let (dx, dy) = ((x1 - x0).abs(), (y1 - y0).abs());
    let steps = if dx > dy { dx } else { dy }.max(1);
    let h = t / 2;
    // Brush every max(1, t/2) pixels: enough overlap for a solid stroke.
    let stride = if h > 1 { h } else { 1 };
    let mut s = 0;
    while s <= steps {
        let x = x0 + (x1 - x0) * s / steps;
        let y = y0 + (y1 - y0) * s / steps;
        fill(x - h, y - h, x - h + t, y - h + t);
        s += stride;
    }
    fill(x1 - h, y1 - h, x1 - h + t, y1 - h + t);
}

/// Ring between radii r_in and r_out (a disc if r_in is 0), drawn as
/// horizontal spans.
#[no_mangle]
pub extern "C" fn draw_ring(cx: i32, cy: i32, r_in: i32, r_out: i32) {
    for dy in -r_out..=r_out {
        let xo = ((r_out * r_out - dy * dy) as f32).sqrt() as i32;
        if dy.abs() >= r_in {
            fill(cx - xo, cy + dy, cx + xo + 1, cy + dy + 1);
        } else {
            let xi = ((r_in * r_in - dy * dy) as f32).sqrt() as i32;
            fill(cx - xo, cy + dy, cx - xi, cy + dy + 1);
            fill(cx + xi + 1, cy + dy, cx + xo + 1, cy + dy + 1);
        }
    }
}

/// Filled triangle: sorted by y, then scanlines between the long edge and
/// the two short ones.
#[no_mangle]
pub extern "C" fn draw_triangle(x0: i32, y0: i32, x1: i32, y1: i32, x2: i32, y2: i32) {
    let (mut x0, mut y0, mut x1, mut y1, mut x2, mut y2) = (x0, y0, x1, y1, x2, y2);
    if y1 < y0 {
        std::mem::swap(&mut x0, &mut x1);
        std::mem::swap(&mut y0, &mut y1);
    }
    if y2 < y0 {
        std::mem::swap(&mut x0, &mut x2);
        std::mem::swap(&mut y0, &mut y2);
    }
    if y2 < y1 {
        std::mem::swap(&mut x1, &mut x2);
        std::mem::swap(&mut y1, &mut y2);
    }
    if y2 == y0 {
        return;
    }
    for y in y0..=y2 {
        let mut xa = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
        let mut xb = if y < y1 {
            if y1 == y0 {
                x1
            } else {
                x0 + (x1 - x0) * (y - y0) / (y1 - y0)
            }
        } else if y2 == y1 {
            x1
        } else {
            x1 + (x2 - x1) * (y - y1) / (y2 - y1)
        };
        if xa > xb {
            std::mem::swap(&mut xa, &mut xb);
        }
        fill(xa, y, xb + 1, y + 1);
    }
}

/// Point at `deg` degrees clockwise from straight up, radius r around cx,cy.
pub fn polar_xy(cx: i32, cy: i32, r: f32, deg: f32) -> (i32, i32) {
    let a = deg * std::f32::consts::PI / 180.0;
    // round() is lroundf: halves away from zero.
    (
        cx + (r * a.sin()).round() as i32,
        cy - (r * a.cos()).round() as i32,
    )
}

/// C entry point for polar_xy.
///
/// # Safety
/// `x` and `y` must be valid pointers to write to.
#[no_mangle]
pub unsafe extern "C" fn polar(cx: i32, cy: i32, r: f32, deg: f32, x: *mut i32, y: *mut i32) {
    let (px, py) = polar_xy(cx, cy, r, deg);
    *x = px;
    *y = py;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn polar_points() {
        assert_eq!(polar_xy(100, 100, 10.0, 0.0), (100, 90)); // up
        assert_eq!(polar_xy(100, 100, 10.0, 90.0), (110, 100)); // right
        assert_eq!(polar_xy(100, 100, 10.0, 180.0), (100, 110));
    }
}
