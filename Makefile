# Native Linux builds. The gitlinks under deps/ pin the tested upstream versions.
HAL := $(CURDIR)/deps/hal
MESHCORE := $(CURDIR)/deps/meshcore
CRYPTO := $(CURDIR)/deps/crypto
BUILD := $(CURDIR)/build
ADAPTER = $(MAKE) -C integration HAL="$(HAL)" MESHCORE="$(MESHCORE)" CRYPTO="$(CRYPTO)" BUILD="$(BUILD)"

.PHONY: all hal receiver monitor check radio-repeater
all: receiver

hal:
	$(MAKE) -C "$(HAL)/libtools" libtinymt32.a
	$(MAKE) -C "$(HAL)/libloragw" libloragw.a

receiver: hal
	$(MAKE) -C driver HAL="$(HAL)" all

monitor:
	$(ADAPTER) all

check: receiver
	$(MAKE) -C driver HAL="$(HAL)" check
	$(ADAPTER) check

radio-repeater: hal
	$(ADAPTER) radio-repeater
