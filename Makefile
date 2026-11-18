# Native Linux builds. The gitlinks under deps/ pin the tested upstream versions.
HAL := $(CURDIR)/deps/hal
MESHCORE := $(CURDIR)/deps/meshcore
CRYPTO := $(CURDIR)/deps/crypto
BUILD := $(CURDIR)/build
USP := $(CURDIR)/deps/usp
ADAPTER = $(MAKE) -C integration HAL="$(HAL)" MESHCORE="$(MESHCORE)" CRYPTO="$(CRYPTO)" BUILD="$(BUILD)"

.PHONY: all hal receiver monitor check radio-repeater usp-rx usp-scheduler bench-handover bench-handover-wall
all: receiver

hal:
	$(MAKE) -C "$(HAL)/libtools" libtinymt32.a
	$(MAKE) -C "$(HAL)/libloragw" libloragw.a

receiver: hal
	$(MAKE) -C driver HAL="$(HAL)" USP="$(USP)" all

monitor:
	$(ADAPTER) all

usp-scheduler:
	$(MAKE) -C usp_scheduler USP="$(USP)" all

usp-rx: receiver usp-scheduler
	mkdir -p "$(BUILD)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -Idriver -Iusp_scheduler \
	    $(LDFLAGS) -Wl,--gc-sections -o "$(BUILD)/usp-rx" integration/usp-rx.c \
	    usp_scheduler/libusp_scheduler.a driver/libsx1250_rx.a \
	    "$(HAL)/libloragw/libloragw.a" "$(HAL)/libtools/libtinymt32.a" -lrt -lm $(LDLIBS)

check: usp-rx
	$(MAKE) -C driver HAL="$(HAL)" USP="$(USP)" check
	$(MAKE) -C usp_scheduler USP="$(USP)" check
	$(ADAPTER) check
	python3 -B integration/test_observe_profiles.py "$(BUILD)/meshcore-rx-monitor" "$(BUILD)/usp-rx"

radio-repeater: hal
	$(ADAPTER) radio-repeater

bench-handover bench-handover-wall:
	$(MAKE) -C "$(HAL)/libloragw" inc/config.h
	$(MAKE) -C driver HAL="$(HAL)" USP="$(USP)" $@
