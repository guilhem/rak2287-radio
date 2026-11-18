# Absolute paths to the two pinned source checkouts and the output directory.
MESHCORE ?= $(CURDIR)/MeshCoreRPI
CRYPTO ?= $(CURDIR)/arduinolibs
BUILD ?= $(CURDIR)/build
RPI_REV := 5f7def6f14fbd0562a81d71c18d93739b65e029c
CRYPTO_REV := 37a76b8f7516568e1c575b6dc9268da1ccaac6b6

NATIVE := $(MESHCORE)/host/rpi_native_cli/include
APP := $(MESHCORE)/examples/simple_repeater
LIB := $(CRYPTO)/libraries/Crypto
ED := $(MESHCORE)/lib/ed25519
CPPFLAGS += -I. -I$(NATIVE) -I$(APP) -I$(MESHCORE)/src -I$(MESHCORE)/src/helpers -I$(LIB) -I$(ED)
CPPFLAGS += -DMESHCORE_EXTERNAL_RX=1 -DARDUINO=1 -DRP2040_PLATFORM=1 -DMESH_PACKET_LOGGING=0 -DMESH_DEBUG=0 -DMAX_NEIGHBOURS=100
CXXFLAGS += -std=gnu++17 -O2 -ffunction-sections -fdata-sections -MMD -MP
CFLAGS += -O2 -ffunction-sections -fdata-sections -MMD -MP
CORE := Packet Dispatcher Mesh Utils Identity
HELPERS := StaticPoolPacketManager CommonCLI ClientACL RegionMap TxtDataHelpers AdvertDataHelpers IdentityStore TransportKeyStore
CRYPTO_OBJS := AES128 AESCommon BlockCipher Crypto SHA256 SHA512 Ed25519 Curve25519 BigNumberUtil Hash
ED_OBJS := fe ge sc keypair key_exchange sha512 sign
OBJECTS := $(addprefix $(BUILD)/,$(addsuffix .o,$(CORE) $(HELPERS) $(CRYPTO_OBJS) $(ED_OBJS) MyMesh external_radio))

.PHONY: all check-revisions
all: $(BUILD)/meshcore-rx-monitor

check-revisions:
	@test "$$(git -C "$(MESHCORE)" rev-parse HEAD)" = "$(RPI_REV)"
	@test "$$(git -C "$(CRYPTO)" rev-parse HEAD)" = "$(CRYPTO_REV)"

$(OBJECTS) $(BUILD)/monitor.o $(BUILD)/repeater.o: | $(BUILD) check-revisions
$(BUILD):
	mkdir -p "$@"

# The upstream Linux-specific CLI diagnostic assumes direct SPI access.
# Build its existing "not supported" branch for this external radio.
$(BUILD)/MyMesh.cpp: $(APP)/MyMesh.cpp stdin-target.patch | $(BUILD)
	patch --silent -o "$@" "$<" stdin-target.patch

vpath %.cpp . $(BUILD) $(MESHCORE)/src $(MESHCORE)/src/helpers $(LIB)
vpath %.c $(ED)
$(BUILD)/%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c "$<" -o "$@"
$(BUILD)/%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"
$(BUILD)/meshcore-rx-monitor: $(OBJECTS) $(BUILD)/monitor.o
	$(CXX) $(LDFLAGS) -Wl,--gc-sections $^ -o "$@"

-include $(OBJECTS:.o=.d) $(BUILD)/monitor.d $(BUILD)/repeater.d

# The normal observer has no dependency on the radio driver or its transmit code.
DRIVER ?= $(abspath ../driver)
HAL ?= $(abspath ../sx1302_hal)
RADIO_OBJECTS := $(addprefix $(BUILD)/,sx1250_rx.o sx1250_tx.o sx1250_board.o)
REPEATER_OBJECTS := $(filter-out $(BUILD)/CommonCLI.o,$(OBJECTS)) $(BUILD)/RepeaterCLI.o $(BUILD)/repeater.o
# The native preference loader clamps even stricter persisted budgets to 9x.
# Keep the loaded value for the external radio target's validation.
$(BUILD)/RepeaterCLI.cpp: $(MESHCORE)/src/helpers/CommonCLI.cpp repeater-prefs.patch | $(BUILD)
	patch --silent -o "$@" "$<" repeater-prefs.patch
$(BUILD)/RepeaterCLI.o: | $(BUILD) check-revisions
HAL_LIBS := $(HAL)/libloragw/libloragw.a $(HAL)/libtools/libtinymt32.a
.PHONY: radio-repeater simulated-repeater
radio-repeater: $(BUILD)/meshcore-radio-repeater
simulated-repeater: $(BUILD)/meshcore-sim-repeater
vpath %.c $(DRIVER) $(DRIVER)/tests tests $(ED)
$(BUILD)/repeater.o $(RADIO_OBJECTS) $(BUILD)/fake_hal.o $(BUILD)/radio_scenario.o: CPPFLAGS += -I$(DRIVER) -I$(HAL)/libloragw/inc
$(RADIO_OBJECTS) $(BUILD)/fake_hal.o $(BUILD)/radio_scenario.o: | $(BUILD)
$(BUILD)/meshcore-radio-repeater: $(REPEATER_OBJECTS) $(RADIO_OBJECTS) $(HAL_LIBS)
	$(CXX) $(LDFLAGS) -Wl,--gc-sections $^ -lrt -lm -o "$@"
$(BUILD)/meshcore-sim-repeater: $(REPEATER_OBJECTS) $(RADIO_OBJECTS) $(BUILD)/fake_hal.o $(BUILD)/radio_scenario.o
	$(CXX) $(LDFLAGS) -Wl,--gc-sections -Wl,--wrap=sx1250_rx_next,--wrap=sx1250_rx_activity,--wrap=sx1250_tx_send $^ -o "$@"
-include $(BUILD)/RepeaterCLI.d $(RADIO_OBJECTS:.o=.d) $(BUILD)/fake_hal.d $(BUILD)/radio_scenario.d

$(BUILD)/repeater_queue_test.o: tests/repeater_queue_test.cpp repeater_queue.h | $(BUILD) check-revisions
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -c "$<" -o "$@"
$(BUILD)/repeater-queue-test: $(BUILD)/repeater_queue_test.o $(BUILD)/StaticPoolPacketManager.o $(BUILD)/Packet.o
	$(CXX) $(LDFLAGS) -Wl,--gc-sections $^ -o "$@"
-include $(BUILD)/repeater_queue_test.d
.PHONY: check
check: $(BUILD)/repeater-queue-test $(BUILD)/meshcore-rx-monitor simulated-repeater
	"$(BUILD)/repeater-queue-test"
	python3 -B test_monitor.py "$(BUILD)/meshcore-rx-monitor"
	python3 -B test_repeater.py "$(BUILD)/meshcore-sim-repeater"
