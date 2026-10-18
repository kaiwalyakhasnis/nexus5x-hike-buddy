# Static aarch64 build with a stock Linux cross compiler and glibc
# (Debian/Ubuntu: apt install g++-aarch64-linux-gnu). hike-buddy only uses
# plain Linux interfaces, so nothing Android is needed here.
CROSS    ?= aarch64-linux-gnu-
OUT      ?= ../out/hike-buddy
CXX      := $(CROSS)g++

CXXFLAGS := -O2 -std=c++14 -Wall -fno-exceptions -Iminui/include -Iminui -DOVERSCAN_PERCENT=0
LDFLAGS  := -static -s
LDLIBS   := -lm

SRCS := main.cpp sys.cpp segment.cpp hub.cpp draw.cpp altitude.cpp power.cpp map.cpp gps.cpp minui/graphics.cpp minui/graphics_fbdev.cpp minui/events.cpp
OBJS := $(SRCS:%.cpp=$(OUT)/%.o)

$(OUT)/hike-buddy: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/%.o: %.cpp $(wildcard *.h minui/*.h minui/include/minui/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf $(OUT)
.PHONY: clean
