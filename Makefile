# KBUS HTTP gateway
#
#   make                      native build (simulation only, unless sdk/include exists)
#   make CC=aarch64-linux-gnu-gcc REQUIRE_ADI=1     PFC build (done by tools/build-ipk.sh)
#
# The WAGO ADI/DAL backend is compiled when the SDK headers are present in
# $(SDK)/include (see tools/import-sdk-headers.sh). The WAGO libraries are not
# linked; they are loaded at runtime from the controller firmware.

CC          ?= gcc
SDK         ?= sdk
REQUIRE_ADI ?= 0
BUILD       ?= build
TARGET      ?= $(BUILD)/kbus-gateway

CFLAGS  ?= -O2 -g
# Required flags live in separate variables so `make CFLAGS=...` cannot drop them.
GW_CFLAGS  := -std=gnu11 -Wall -Wextra -pthread
GW_LDFLAGS := -pthread
GW_LDLIBS  := -ldl

SRCS := src/main.c src/log.c src/config.c src/gateway.c src/http_server.c src/wda.c \
        src/backend_sim.c src/backend_adi.c
OBJS := $(SRCS:src/%.c=$(BUILD)/%.o) $(BUILD)/web_asset.o   # web page built into the binary

ifneq ($(wildcard $(SDK)/include/dal/adi_application_interface.h),)
  CPPFLAGS += -DHAVE_WAGO_ADI
  # -idirafter: the compiler's own libc headers keep priority over the SDK's.
  SDK_DIRS := $(SDK)/include $(SDK)/include/OsLinux $(SDK)/include/dal \
              $(SDK)/include/glib-2.0 $(SDK)/lib/glib-2.0/include \
              $(SDK)/include/dbus-1.0 $(SDK)/lib/dbus-1.0/include
  CPPFLAGS += $(addprefix -idirafter ,$(wildcard $(SDK_DIRS)))
  ADI_STATE := with WAGO ADI/DAL backend
else ifeq ($(REQUIRE_ADI),1)
  $(error WAGO SDK headers not found in $(SDK)/include. Run tools/import-sdk-packages.sh \
          first (README 2.3), or build simulation-only with REQUIRE_ADI=0)
else
  ADI_STATE := SIMULATION ONLY (no sdk/include)
endif

.PHONY: all clean run-sim

all: $(TARGET)
	@echo "built $(TARGET): $(ADI_STATE)"

$(TARGET): $(OBJS)
	$(CC) $(GW_LDFLAGS) $(LDFLAGS) -o $@ $^ $(GW_LDLIBS) $(LDLIBS)

$(BUILD)/%.o: src/%.c src/*.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(GW_CFLAGS) $(CFLAGS) -c -o $@ $<

# web/index.html is embedded with .incbin (path relative to the project root)
$(BUILD)/web_asset.o: src/web_asset.S web/index.html | $(BUILD)
	$(CC) -c -o $@ $<

$(BUILD):
	mkdir -p $@

run-sim: $(TARGET)
	KBUS_SIMULATION=1 KBUS_PRIORITY=0 $(TARGET)

clean:
	rm -rf $(BUILD)
