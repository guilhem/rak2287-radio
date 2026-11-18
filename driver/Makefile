# Build the pinned Semtech V2.1.0 HAL libraries first.
HAL ?= ../sx1302_hal
CPPFLAGS += -I$(HAL)/libloragw/inc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror
HAL_LIBS = $(HAL)/libloragw/libloragw.a $(HAL)/libtools/libtinymt32.a -lrt -lm
OBJS = sx1250_rx.o sx1250_board.o

all: sx1250-rx libsx1250_rx.a
# Explicit opt-in. The ordinary receiver does not link the TX implementation.
libsx1250_tx.a: sx1250_tx.o
	$(AR) rcs $@ $^
libsx1250_fake_hal.a: tests/fake_hal.o
	$(AR) rcs $@ $^
sx1250_tx.o: sx1250_tx.h sx1250_io.h sx1250_lora.h sx1250_rx_internal.h sx1250_rx.h
tests/fake_hal.o: tests/fake_hal.h
libsx1250_rx.a: $(OBJS)
	$(AR) rcs $@ $^
sx1250-rx: sx1250-rx.o libsx1250_rx.a
	$(CC) $(LDFLAGS) -o $@ $^ $(HAL_LIBS) $(LDLIBS)
gfsk_rx: examples/gfsk_rx.c sx1250_board.o sx1250_io.h sx1250_packet.h sx1250_rx.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< sx1250_board.o $(HAL_LIBS) $(LDLIBS)
$(OBJS): sx1250_io.h sx1250_board.h
sx1250_rx.o: sx1250_packet.h sx1250_lora.h sx1250_rx_internal.h
sx1250-rx.o sx1250_rx.o: sx1250_rx.h
check: libsx1250_rx.a libsx1250_tx.a libsx1250_fake_hal.a
	CC="$(CC)" CXX="$(CXX)" HAL="$(abspath $(HAL))" python3 test_rx.py
clean:
	$(RM) $(OBJS) sx1250-rx.o sx1250_tx.o tests/fake_hal.o libsx1250_rx.a libsx1250_tx.a libsx1250_fake_hal.a sx1250-rx gfsk_rx
.PHONY: all check clean

# Offline comparison: the benchmark links register models and no physical SPI ioctl.
.PHONY: bench-handover bench-handover-wall
bench-handover bench-handover-wall:
	CC="$(CC)" python3 tests/bench_handover.py --hal "$(abspath $(HAL))" $(if $(filter bench-handover-wall,$@),--wall-clock)
