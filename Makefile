# Native Linux builds. The gitlinks under deps/ pin the tested upstream versions.
HAL := $(CURDIR)/deps/hal
MESHCORE := $(CURDIR)/deps/meshcore
CRYPTO := $(CURDIR)/deps/crypto
BUILD := $(CURDIR)/build
USP := $(CURDIR)/deps/usp
ADAPTER = $(MAKE) -C integration HAL="$(HAL)" MESHCORE="$(MESHCORE)" CRYPTO="$(CRYPTO)" BUILD="$(BUILD)"
HAL_LIBS := $(HAL)/libloragw/libloragw.a $(HAL)/libtools/libtinymt32.a
KISS_FLAGS := -std=c11 -Wall -Wextra -Werror -Idriver -Ikiss -pthread
KISS_OBJS := $(BUILD)/rak-kiss.o $(BUILD)/kiss-codec.o
DRIVER_LIBS := driver/libsx1250_tx.a driver/libsx1250_rx.a

.PHONY: all hal kiss kiss-sim kiss-check kiss-driver-check receiver monitor check radio-repeater usp-rx usp-scheduler bench-handover bench-handover-wall
all: kiss

hal:
	$(MAKE) -C "$(HAL)/libtools" libtinymt32.a
	$(MAKE) -C "$(HAL)/libloragw" libloragw.a

$(BUILD):
	mkdir -p "$@"

$(BUILD)/rak-kiss.o: kiss/rak-kiss.c kiss/codec.h driver/sx1250_dual.h driver/sx1250_rx.h driver/sx1250_tx.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KISS_FLAGS) -c "$<" -o "$@"

$(BUILD)/kiss-codec.o: kiss/codec.c kiss/codec.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KISS_FLAGS) -c "$<" -o "$@"

kiss: hal $(KISS_OBJS)
	$(MAKE) -C driver HAL="$(HAL)" USP="$(USP)" libsx1250_tx.a libsx1250_rx.a
	$(CC) $(LDFLAGS) -pthread -Wl,--gc-sections -o "$(BUILD)/rak-kiss" \
	    $(KISS_OBJS) $(DRIVER_LIBS) $(HAL_LIBS) -lrt -lm $(LDLIBS)

$(BUILD)/rak-kiss-sim: $(KISS_OBJS) kiss/tests/radio_scenario.c driver/sx1250_lora.c \
                      driver/sx1250_lora.h driver/sx1250_board.h driver/sx1250_rx.h driver/sx1250_tx.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KISS_FLAGS) $(LDFLAGS) -o "$@" $(filter %.o %.c,$^) -lm $(LDLIBS)

kiss-sim: $(BUILD)/rak-kiss-sim

$(BUILD)/kiss-codec-test: kiss/tests/test_codec.c $(BUILD)/kiss-codec.o kiss/codec.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KISS_FLAGS) $(LDFLAGS) -UNDEBUG -o "$@" $(filter %.o %.c,$^) $(LDLIBS)

kiss-check: kiss-sim $(BUILD)/kiss-codec-test
	"$(BUILD)/kiss-codec-test"
	python3 -B kiss/tests/test_modem.py "$(BUILD)/rak-kiss-sim"

kiss-driver-check: $(KISS_OBJS)
	$(MAKE) -C "$(HAL)/libloragw" inc/config.h
	$(MAKE) -C driver HAL="$(HAL)" USP="$(USP)" libsx1250_tx.a libsx1250_rx.a libsx1250_fake_hal.a
	$(CC) $(LDFLAGS) -pthread -Wl,--gc-sections -o "$(BUILD)/rak-kiss-driver-sim" \
	    $(KISS_OBJS) $(DRIVER_LIBS) driver/libsx1250_fake_hal.a -lrt -lm $(LDLIBS)
	python3 -B kiss/tests/test_modem.py "$(BUILD)/rak-kiss-driver-sim" DriverModemTest

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
	$(MAKE) kiss-driver-check
	$(MAKE) -C usp_scheduler USP="$(USP)" check
	$(ADAPTER) check
	python3 -B integration/test_observe_profiles.py "$(BUILD)/meshcore-rx-monitor" "$(BUILD)/usp-rx"

radio-repeater: hal
	$(ADAPTER) radio-repeater

bench-handover bench-handover-wall:
	$(MAKE) -C "$(HAL)/libloragw" inc/config.h
	$(MAKE) -C driver HAL="$(HAL)" USP="$(USP)" $@
