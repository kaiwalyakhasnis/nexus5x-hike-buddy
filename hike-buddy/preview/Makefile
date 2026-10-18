# Host build of the screen for layout previews (no phone needed).
OUT ?= ../../out/preview
$(OUT)/preview: preview.cpp ../*.cpp ../*.h ../minui/*.cpp
	@mkdir -p $(OUT)
	g++ -O1 -std=c++14 -w -DOVERSCAN_PERCENT=0 -I../minui/include -I../minui -o $@ \
		preview.cpp ../sys.cpp ../segment.cpp ../hub.cpp ../draw.cpp ../altitude.cpp ../power.cpp ../map.cpp gps_stub.cpp ../minui/graphics.cpp ../minui/events.cpp
