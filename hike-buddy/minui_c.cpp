// extern "C" entry points into minui (C++ linkage) for the Rust side (src/gr.rs).
#include "minui/minui.h"

extern "C" void hb_gr_fill(int x1, int y1, int x2, int y2) { gr_fill(x1, y1, x2, y2); }
