# Optional RF0 LoRa transmission

The TX library lets a serialized application send one LoRa frame through RF0 and resume receiving on RF1. It is experimental on the RAK2287 EU868. The transaction has been exercised against a simulated two-radio board. Antenna routing, radiated power, modulation quality and reception by a peer require hardware measurement.

Build the ordinary RX library and explicitly add the TX archive.

```sh
make HAL=/path/to/sx1302_hal libsx1250_rx.a libsx1250_tx.a
```

Include `sx1250_tx.h` and link `libsx1250_tx.a`, then `libsx1250_rx.a`, then the stock Semtech HAL and its dependencies. After opening a LoRa RX profile, call `sx1250_tx_send(payload, length, timeout_ms)`. Supply the complete PHY bytes. The existing frequency, SF, bandwidth, coding rate, sync word, preamble, CRC and IQ polarity apply to the transmission. An implicit-header frame must use the configured fixed length. Choose a timeout long enough for the selected PHY and payload. The application owns airtime accounting and channel access.

Only a return value of zero confirms that the driver observed RF0 TxDone and rearmed RF1. On `EAGAIN`, read the pending RF1 event using `sx1250_rx_next()` before attempting to send again. While RF0 transmits, RF1 is unavailable. An in-progress preamble can be interrupted when switching to TX. An I/O error during a send can follow an actual transmission. If the receiver also returns `EIO`, close it, reset the board and reopen it. The timeout bounds the RF transmission and its IRQ wait. Board reset, setup and recovery require additional time.

RF0 first leaves the concentrator's I/Q mode while the radio interface uses its SPI clock. The driver keeps both external PA enables low and disables both LNAs during this transaction. For RF0 it uses the SX1262 reduced-output PA settings `02 02 00 01` and a signed SetTxParams value of `-9`. These values do not determine the power at the antenna connector. The stock EU868 reference table uses an external-PA bypass at its low settings. Before configuring RF0, the driver checks that its post-reset pin-mode register matches the working RF1 packet engine. The bypass path and power need independent confirmation on the specific board. Before enabling real TX, attach an appropriate load or antenna. [Semtech SX1261/SX1262 datasheet](https://www.semtech.com/products/wireless-rf/lora-connect/sx1262) · [Semtech EU868 reference gain table](https://github.com/Lora-net/sx1302_hal/blob/4b42025d1751e04632c0b04160e0d29dbbb222a5/packet_forwarder/global_conf.json.sx1250.EU868)

For a software-only integration, link the two driver archives against `libsx1250_fake_hal.a` in place of the hardware HAL. Set `FAKE_TX=1` and `IDLE=1`, then open a temporary ordinary file as the SPI path. The test header exposes `fake_inject_rx()` for a complete RF1 payload and `fake_last_tx()` to inspect the most recent RF0 TxDone bytes. The archive and test hooks are built only on request or by `make check`.
