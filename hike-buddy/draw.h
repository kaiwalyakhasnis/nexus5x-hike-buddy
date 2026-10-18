// Small drawing helpers on top of minui's gr_fill(); all use the current gr_color.
// Implemented in Rust (src/draw.rs).
#pragma once

extern "C" {

// Line of thickness t (square brush).
void draw_line(int x0, int y0, int x1, int y1, int t);

// Ring between radii r_in and r_out, drawn as horizontal spans.
void draw_ring(int cx, int cy, int r_in, int r_out);

// Filled triangle.
void draw_triangle(int x0, int y0, int x1, int y1, int x2, int y2);

// Point at `deg` degrees clockwise from straight up, radius r around cx,cy.
void polar(int cx, int cy, float r, float deg, int* x, int* y);
}
