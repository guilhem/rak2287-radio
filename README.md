# RAK2287 radio

Use the two SX1250 radios on a RAK2287 EU868 SPI concentrator from Linux. The C library receives LoRa packets on RF1. Its optional half-duplex transmit path sends a packet on RF0 and returns the board to RF1 reception. Applications supply their own packet bytes and radio profile. The MeshCore adapter is one such application.

**RF1 reception has been demonstrated on hardware at 62.5 kHz. RF0 transmission and the handoff back to RF1 have been exercised in simulation.** Antenna-port routing, conducted power, spectrum and over-the-air delivery on RF0 still require board measurements.

## Run the repeater in simulation

On Linux, install Git, GNU Make, a C compiler, a C++17 compiler, Python 3 and `patch`. The test runs without a concentrator, GPIO access or root privileges.

```sh
git clone --recurse-submodules https://github.com/guilhem/rak2287-radio.git
cd rak2287-radio
make -j2 check
```

The test feeds synthetic EU Narrow packets into a simulated RF1 FIFO. The MeshCore repeater decides which packets to forward and appends its path hash. The production C driver writes the forwarded bytes to the simulated RF0 FIFO, waits for the transmit-complete interrupt, and rearms RF1. The scenario checks a second reception and duplicate suppression. Separate tests exercise timeout and recovery failures.

## Choose an interface

| Goal | Entry point |
| --- | --- |
| Receive packets from a physical board | `make receiver` builds the RX-only CLI. It writes payload bytes in hexadecimal plus RSSI and SNR to stdout. [Receiver guide](driver/README.md) |
| Integrate your own application | Open a LoRa profile with `sx1250_rx_open()`. Use `sx1250_rx_next()` for reception. Link the optional transmit API to call `sx1250_tx_send()`. [RX API](driver/sx1250_rx.h) · [TX API and error handling](driver/sx1250_tx.h) |
| Observe MeshCore routing without transmitting | `make monitor` builds the stdin observer. It accepts packets from the RX-only CLI and reports forwarding decisions. [MeshCore adapter](integration/native-monitor.md) |
| Build the experimental RF0/RF1 MeshCore repeater | `make radio-repeater` builds the physical backend. Running it requires a separate transmit opt-in. The EU Narrow profile is 869.618 MHz, SF8, 62.5 kHz, CR 4/8 and private sync 0x12. [Repeater guide and validation](integration/radio-repeater.md) |
| Use the standard multichannel gateway again | Stop the packet driver, reset the board using the helper for your hardware, then start the gateway software. [Hardware and ownership](driver/README.md) |

The board and HAL have a single owner during a packet-radio session. During a transmission RF1 cannot receive. A successful transmit call means the driver observed RF0 `TxDone` and rearmed RF1. Delivery to another node needs a separate receiver or protocol acknowledgement.

The code uses BSD-3-Clause. Upstream source and binary notices are listed separately. [License](LICENSE) · [Third-party notices](NOTICE.md)
