USP ?= ../deps/usp
RAC = $(USP)/smtc_rac_lib
RADIO = $(RAC)/smtc_radio_interface
DRIVER = ../driver
CPPFLAGS += -I$(DRIVER) -I$(RAC)/smtc_rac_api -I$(RAC)/smtc_rac \
	-I$(RAC)/smtc_core_hal_interface -I$(RAC)/radio_planner/src \
	-I$(RADIO)/smtc_ral/src -I$(RADIO)/smtc_ralf/src \
	-I$(RADIO)/radio_drivers/sx126x_driver/src \
	-DUSE_SMTC_RAC=1 -DSX126X -DSMTC_RADIO_PLANNER_TRACE=0 \
	-DRAC_CORE_LOG_CONFIG_ENABLE=0
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -ffunction-sections -fdata-sections

all: libusp_scheduler.a

libusp_scheduler.a: scheduler_combined.o
	$(AR) rcs $@ $^

# Keep the upstream lock-only engine and discard the unused RF transaction APIs.
scheduler_combined.o: usp_scheduler.o smtc_rac.o radio_planner.o duty_cycle.o
	$(CC) -r -nostdlib -Wl,--gc-sections,-u,usp_rx_run -o $@ $^

usp_scheduler.o: usp_scheduler.c usp_scheduler.h rac_radio.h $(DRIVER)/sx1250_rx.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Werror -c -o $@ $<

smtc_rac.o: $(RAC)/smtc_rac/smtc_rac.c rac_radio.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -include rac_radio.h -c -o $@ $<

radio_planner.o: $(RAC)/radio_planner/src/radio_planner.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

duty_cycle.o: $(RAC)/radio_planner/src/duty_cycle.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

test_scheduler: test_scheduler.c libusp_scheduler.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Werror -o $@ $^ $(LDFLAGS) $(LDLIBS) \
		-Wl,--wrap=clock_gettime,--wrap=nanosleep

check: test_scheduler
	./test_scheduler

clean:
	$(RM) *.o libusp_scheduler.a test_scheduler

.PHONY: all check clean
