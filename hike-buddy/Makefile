# Static aarch64 build with a stock Linux cross compiler and glibc
# (Debian/Ubuntu: apt install g++-aarch64-linux-gnu). hike-buddy only uses
# plain Linux interfaces, so nothing Android is needed here.
#
# Mid-migration to Rust: src/ builds with cargo into a static library for
# the same glibc target, linked into the one static binary below.
CROSS    ?= aarch64-linux-gnu-
OUT      ?= ../out/hike-buddy
CXX      := $(CROSS)g++
CARGO    ?= $(shell command -v cargo || echo $(HOME)/.cargo/bin/cargo)
RTARGET  := aarch64-unknown-linux-gnu
RUSTLIB  := $(OUT)/rust/$(RTARGET)/release/libhike_buddy.a

CXXFLAGS := -O2 -std=c++14 -Wall -fno-exceptions -Iminui/include -Iminui -DOVERSCAN_PERCENT=0
LDFLAGS  := -static -s
LDLIBS   := -lm

SRCS := main.cpp sys.cpp minui_c.cpp hub.cpp power.cpp map.cpp gps.cpp minui/graphics.cpp minui/graphics_fbdev.cpp minui/events.cpp
OBJS := $(SRCS:%.cpp=$(OUT)/%.o)

$(OUT)/hike-buddy: $(OBJS) $(RUSTLIB)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS) $(RUSTLIB) $(LDLIBS)

# cargo tracks its own dependencies; make only needs to call it.
$(RUSTLIB): FORCE
	$(CARGO) build --quiet --release --target $(RTARGET) --target-dir $(OUT)/rust

$(OUT)/%.o: %.cpp $(wildcard *.h minui/*.h minui/include/minui/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf $(OUT)
.PHONY: clean FORCE
