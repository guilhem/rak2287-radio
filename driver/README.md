# SX1250 packet radio for the RAK2287

Read LoRa packets on Linux directly from one or both radios of a RAK2287 SPI concentrator. An application gets the original packet bytes, RSSI and SNR through a C API. The command-line receiver listens on RF1. An optional transmit library sends on RF0 and restores reception. A GFSK receive example uses the same board interface.

The tested board is a RAK2287 in a Raspberry Pi 4 hotspot. In single-receiver mode, RF0 supplies the clock and RF1 exposes its demodulated packets through the SX1302 SPI multiplexer. On this board RF1 received an SF8, 62.5 kHz packet whose 53 bytes exactly matched a simultaneous I/Q recording decoded on the host. The packet driver runs without loading custom concentrator firmware.

## Receive a packet

You need a Linux C toolchain, Make, Git for the initial checkout, Python 3 and a C++ compiler for the tests, and access to the concentrator SPI device. Build against the upstream Semtech V2.1.0 HAL. From this directory:

```sh
git -C .. submodule update --init deps/hal deps/usp
HAL=$(cd ../deps/hal && pwd)
make -C "$HAL/libtools" libtinymt32.a
make -C "$HAL/libloragw" libloragw.a
make HAL="$HAL"
make check HAL="$HAL"
```

Give the receiver exclusive access. Stop any gateway software using the concentrator and run your board's external reset helper. On the tested Blackspot, use `./reset_lgw.sh start` from the gateway tools directory (BCM25). Use the reset pin and helper for your own board. [Blackspot reset helper](https://github.com/cyberjunky/MNTD-LoRa-Repeater/blob/280f1a80418d7bc8b9154b7ed8337fcb18fbeb28/install/bin/reset_lgw.sh) With an EU Narrow sender nearby, run:

```sh
./sx1250-rx --device /dev/spidev0.0 --frequency 869618000 --bandwidth 62500 \
    --sf 8 --cr 8 --sync 0x12 --preamble 16
```

Every accepted packet produces one line on stdout. The four tab-separated fields are the literal `RX`, RSSI in dBm, SNR in dB, and the hex payload. The command writes diagnostics to stderr. An idle channel produces no packet lines. Ctrl+C releases the radio. Run the board reset helper before restarting the gateway.

## Use the C library

Include `sx1250_rx.h` and link `libsx1250_rx.a`, `libloragw.a`, `libtinymt32.a`, `-lrt` and `-lm`, in that order. Configure the center frequency in Hz, bandwidth in Hz, spreading factor, coding-rate denominator, sync word, preamble length, CRC requirement and IQ polarity. Explicit LoRa headers supply the received coding rate and CRC flag. For implicit headers, use `--implicit-length N` or set `implicit_length` (1–255 bytes). The implicit mode takes CR and CRC from the configured profile and checks the fixed-length register against the requested length.

```c
#include "sx1250_rx.h"

int receive_once(struct sx1250_rx_packet *packet) {
    struct sx1250_rx_config radio = {
        .frequency_hz = 869618000, .bandwidth_hz = 62500, .spreading_factor = 8,
        .coding_rate = 8, .sync_word = 0x12, .preamble_symbols = 16, .require_crc = true
    };
    if (sx1250_rx_open("/dev/spidev0.0", &radio) < 0)
        return -1;
    int received = sx1250_rx_next(packet, 1000);
    return sx1250_rx_close() < 0 ? -1 : received;
}
```

A positive result supplies the payload, byte count, packet RSSI, SNR, CRC-presence flag and received coding rate. Zero means the wait expired. A negative result means the call failed. Single-packet RX holds the completed FIFO until the application reads it. The library rearms on each terminal event. A slow caller or a blocked output pipe can miss subsequent transmissions during that interval. Use one owner and serialize access. The library uses the process-global Semtech HAL. An advisory device lock coordinates other users of this library. Existing gateway applications require a separate stop/reset/start transition. [API and supported values](sx1250_rx.h)

## Send a packet and resume reception

Link the optional `libsx1250_tx.a` before the RX library and call `sx1250_tx_send()` on an open receiver. Success means RF0 reported TxDone and the driver rearmed RF1. Physical operation requires an antenna or suitable load. [Build, failure handling and hardware limits](TX.md)

## Receive on both radios

Open one session with `sx1250_dual_open()`, passing a profile for RF0
and a profile for RF1. Poll each chain with `sx1250_dual_next()` and
close the session with `sx1250_dual_close()`. Both packet engines listen
independently. One process owns SPI and serializes the calls.
Use `sx1250_dual_reconfigure()` to retune one receiver while the other
listens. `EAGAIN` means the selected chain is receiving or has a packet
pending. Poll the packet before retrying.

Use `sx1250_dual_send()` to transmit at the board's default power. To
select a specific level, check it with `sx1250_tx_power_supported()` and
pass it to `sx1250_dual_send_with_power()` along with the profile and frame.
The supported levels are nominal chip settings from −9 to +22 dBm.
The driver defers when either radio has a pending reception. A completed
send reports RF0 TxDone and verifies that both receivers returned to
their current profiles. Retry only an explicit `EAGAIN`. The calling
application owns transmit scheduling and airtime accounting.

A Raspberry Pi trial received 62.5 kHz and 250 kHz LoRa traffic in parallel.
The dual-receiver transmit handoff and recovery have been exercised in
simulation. [Dual RX API](sx1250_dual.h) · [Dual TX API](sx1250_tx.h)

## Choose a hardware path

| Application | Interface and coverage |
| --- | --- |
| Single-channel LoRa packet RX | The C library and CLI control RF1. SF8/62.5 kHz has on-air evidence. Other configurable widths and SFs need over-the-air qualification for their intended use. |
| GFSK packet RX experiment | Build `make gfsk_rx HAL="$HAL"`, reset the idle board, and run `./gfsk_rx 5`. The example checks the GFSK packet type, reads back sync/CRC settings and listens for five seconds. Its source gives the matching sender parameters. [Example](examples/gfsk_rx.c) |
| Multichannel gateway or LoRaWAN forwarding | Return the board to its ordinary concentrator software. Semtech's HAL and packet forwarder continue to own this mode. [Semtech HAL](https://github.com/Lora-net/sx1302_hal/tree/4b42025d1751e04632c0b04160e0d29dbbb222a5) |
| I/Q sampling and capture RAM | Use the concentrator capture path in a separate session. Sustained I/Q sampling has different SPI throughput requirements from packet FIFO reads. |
| Transmission | The optional library sends on RF0 using the open LoRa profile. The half-duplex handoff is tested with a simulated HAL. |

Supported center frequencies fall inside the stock HAL's image-calibration intervals of 430–440, 470–510, 779–787, 863–870 and 902–928 MHz, excluding the endpoints. Retuning into a different interval recalibrates that radio. Use the RF band of the installed board variant and apply local rules in the application. [Semtech calibration](https://github.com/Lora-net/sx1302_hal/blob/4b42025d1751e04632c0b04160e0d29dbbb222a5/libloragw/src/loragw_sx1250.c#L92-L133) · [RAK2287 hardware](https://docs.rakwireless.com/product-categories/wislink/rak2287/datasheet/)

## Source and licenses

The new driver files use BSD-3-Clause. The Semtech HAL remains a separate dependency. Preserve its notices when redistributing linked binaries. The optional GFSK example follows the Semtech SWSD003 receive profile and SX126x command definitions. [Reference example](https://github.com/Lora-net/SWSD003) · [SX126x driver notice](licenses/Semtech-sx126x-driver.txt) This directory can be distributed independently of the MeshCore adapter, local captures and device configuration. [Driver license](LICENSE) · [HAL notices](licenses/Semtech-sx1302-HAL.txt)
