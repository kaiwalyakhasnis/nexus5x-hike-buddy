# Host build of the screen for layout previews (no phone needed).
OUT ?= ../../out/preview
CARGO ?= $(shell command -v cargo || echo $(HOME)/.cargo/bin/cargo)
RUSTLIB := $(OUT)/rust/release/libhike_buddy.a

$(OUT)/preview: preview.cpp ../*.cpp ../*.h ../minui/*.cpp $(RUSTLIB)
	@mkdir -p $(OUT)
	g++ -O1 -std=c++14 -w -DOVERSCAN_PERCENT=0 -I../minui/include -I../minui -o $@ \
		preview.cpp ../sys.cpp ../minui_c.cpp ../power.cpp ../map.cpp gps_stub.cpp ../minui/graphics.cpp ../minui/events.cpp \
		$(RUSTLIB) -lpthread -ldl

# The Rust part, built for the PC.
$(RUSTLIB): FORCE
	cd .. && $(CARGO) build --quiet --release --target-dir $(abspath $(OUT))/rust
.PHONY: FORCE
