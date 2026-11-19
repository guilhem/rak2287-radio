# Native Linux builds. The gitlinks under deps/ pin the tested upstream versions.
HAL := $(CURDIR)/deps/hal
MESHCORE := $(CURDIR)/deps/meshcore
CRYPTO := $(CURDIR)/deps/crypto
BUILD := $(CURDIR)/build
USP := $(CURDIR)/deps/usp
ADAPTER = $(MAKE) -C integration HAL="$(HAL)" MESHCORE="$(MESHCORE)" CRYPTO="$(CRYPTO)" BUILD="$(BUILD)"
HAL_LIBS := $(HAL)/libloragw/libloragw.a $(HAL)/libtools/libtinymt32.a
KISS_FLAGS := -std=c11 -Wall -Wextra -Werror -Idriver -pthread

.PHONY: all hal kiss kiss-sim kiss-check receiver monitor check radio-repeater usp-rx usp-scheduler bench-handover bench-handover-wall
all: kiss

hal:
	$(MAKE) -C "$(HAL)/libtools" libtinymt32.a
	$(MAKE) -C "$(HAL)/libloragw" libloragw.a

$(BUILD):
	mkdir -p "$@"

$(BUILD)/rak-kiss.o: kiss/rak-kiss.c driver/sx1250_dual.h driver/sx1250_rx.h driver/sx1250_tx.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KISS_FLAGS) -c "$<" -o "$@"

kiss: hal $(BUILD)/rak-kiss.o
	$(MAKE) -C driver HAL="$(HAL)" USP="$(USP)" libsx1250_tx.a libsx1250_rx.a
	$(CC) $(LDFLAGS) -pthread -Wl,--gc-sections -o "$(BUILD)/rak-kiss" "$(BUILD)/rak-kiss.o" \
	    driver/libsx1250_tx.a driver/libsx1250_rx.a $(HAL_LIBS) -lrt -lm $(LDLIBS)

$(BUILD)/rak-kiss-sim: $(BUILD)/rak-kiss.o kiss/tests/radio_scenario.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KISS_FLAGS) $(LDFLAGS) -o "$@" $^ -lm $(LDLIBS)

kiss-sim: $(BUILD)/rak-kiss-sim

kiss-check: kiss-sim
	python3 -B kiss/tests/test_modem.py "$(BUILD)/rak-kiss-sim"

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

check: usp-rx kiss-check
	$(MAKE) -C driver HAL="$(HAL)" USP="$(USP)" check
	$(MAKE) -C usp_scheduler USP="$(USP)" check
	$(ADAPTER) check
	python3 -B integration/test_observe_profiles.py "$(BUILD)/meshcore-rx-monitor" "$(BUILD)/usp-rx"

radio-repeater: hal
	$(ADAPTER) radio-repeater

bench-handover bench-handover-wall:
	$(MAKE) -C "$(HAL)/libloragw" inc/config.h
	$(MAKE) -C driver HAL="$(HAL)" USP="$(USP)" $@
