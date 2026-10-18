// Seven-segment numerals drawn with gr_fill(): crisp at any size, no font
// files, and the watch-face look fits a hiking console.
#pragma once

// Draws s (digits, '-', '.', ':', ' ') with its top-left corner at x,y.
// w,h = one digit cell, t = stroke thickness. Uses the current gr_color.
// Returns the x just past the last glyph.
int seg_text(int x, int y, int w, int h, int t, const char* s);
int seg_measure(int w, int t, const char* s);
