# Optional RF0 LoRa transmission

The TX library lets a serialized application send one LoRa frame through RF0 and resume receiving on RF1. It is experimental on the RAK2287 EU868. The transaction has been exercised against a simulated two-radio board. Antenna routing, radiated power, modulation quality and reception by a peer require hardware measurement.

Build the ordinary RX library and explicitly add the TX archive.

```sh
make HAL=/path/to/sx1302_hal libsx1250_rx.a libsx1250_tx.a
```

Include `sx1250_tx.h` and link `libsx1250_tx.a`, then `libsx1250_rx.a`, then the stock Semtech HAL and its dependencies. After opening a LoRa RX profile, call `sx1250_tx_send(payload, length, timeout_ms)`. Supply the complete PHY bytes. The existing frequency, SF, bandwidth, coding rate, sync word, preamble, CRC and IQ polarity apply to the transmission. An implicit-header frame must use the configured fixed length. Choose a timeout long enough for the selected PHY and payload. The application owns airtime accounting and channel access.

Call `sx1250_rx_activity()` before scheduling a transmission. A return of 1 means RF1 has detected a LoRa preamble, accepted an explicit header, or latched a terminal event for `sx1250_rx_next()`. Keep the outgoing packet queued and continue polling the receiver. A return of 0 permits a send attempt. A negative result reports an error through `errno`. The detector uses the configured LoRa profile. Other signals on the channel may go undetected.

The send call samples RF1 again immediately before putting it in standby. On `EAGAIN`, RF0 has not started. Service `sx1250_rx_next()` and retry the queued packet once activity clears. If a new preamble appears between the final SPI sample and the standby command, the handover can still interrupt it. A completed packet latched during that interval remains available to read.

Success confirms that RF0 reported TxDone and RF1 was rearmed. An I/O error can follow an actual transmission. If the receiver also returns `EIO`, close it, reset the board and reopen it. The timeout bounds the RF transmission and its IRQ wait. Board reset, setup and recovery require additional time.

RF0 first leaves the concentrator's I/Q mode while the radio interface uses its SPI clock. The driver keeps both external PA enables low and disables both LNAs during this transaction. For RF0 it uses the SX1262 reduced-output PA settings `02 02 00 01` and a signed SetTxParams value of `-9`. These values do not determine the power at the antenna connector. The stock EU868 reference table uses an external-PA bypass at its low settings. Before configuring RF0, the driver checks that its post-reset pin-mode register matches the working RF1 packet engine. The bypass path and power need independent confirmation on the specific board. Before enabling real TX, attach an appropriate load or antenna. [Semtech SX1261/SX1262 datasheet](https://www.semtech.com/products/wireless-rf/lora-connect/sx1262) · [Semtech EU868 reference gain table](https://github.com/Lora-net/sx1302_hal/blob/4b42025d1751e04632c0b04160e0d29dbbb222a5/packet_forwarder/global_conf.json.sx1250.EU868)

For a software-only integration, link the two driver archives against `libsx1250_fake_hal.a` in place of the hardware HAL. Set `FAKE_TX=1` and `IDLE=1`, then open a temporary ordinary file as the SPI path. The test header exposes `fake_inject_rx()` for a complete RF1 payload and `fake_last_tx()` to inspect the most recent RF0 TxDone bytes. The archive and test hooks are built only on request or by `make check`.

## Measure RF1 downtime without a radio

From the repository root with the submodules initialized, run:

```sh
make bench-handover
make bench-handover-wall
```

The first command compares the existing two-reset sequence with a test-only sequence that switches RF0 between the concentrator's I/Q mode and the LoRa packet engine. Both use the generic driver's send path and the pinned Semtech configuration and SPI routines. The benchmark replaces register I/O with a radio model and uses synthetic packets. The second command repeats the comparison with host monotonic time and real sleeps. Neither command needs a Raspberry Pi, a SPI device or root.

The timing starts when the application requests a send. RF1 becomes unavailable at its standby command and listening resumes at its next successful `SetRx`. The driver subsequently checks RF1 and returns to the application. The RF0 transmit interval runs from `SetTx` to the first poll that sees `TxDone`. The model supplies the on-air duration for the chosen payload. Linux scheduling adds variation to the wall-clock run.

For a 32-byte EU Narrow transmission (869.618 MHz, SF8, 62.5 kHz, CR 4/8), the deterministic run produces the following times in milliseconds.

| Interval | Two resets | No-reset candidate |
| --- | ---: | ---: |
| API entry to RF1 standby | 3 | 3 |
| RF1 standby to RF0 SetTx | 619 | 131 |
| RF0 SetTx to observed TxDone | 411 | 411 |
| Observed TxDone to RF1 SetRx | 574 | 96 |
| **RF1 unavailable** | **1,604** | **638** |
| Additional verification before the API returns | 11 | 11 |
| Total time in the send call | 1,618 | 652 |

The modeled packet takes 410.624 ms on air. Most of the 966 ms saved comes from removing two 520 ms reset sequences. Extra register transfers consume some of that saving. The measured handover excludes the board-opening reset and the candidate's one-time register capture.

For each sequence, the bench runs nine separate transmissions. In each trial, a 10-byte incoming frame starts at a different offset from the send request, from zero to two seconds in 250 ms steps. The model requires uninterrupted listening from the first symbol to the last. The two-reset sequence receives the probes at 1.75 and 2 s (2/9). The candidate receives all six probes starting at 0.75 s or later (6/9). These counts describe that schedule. Radios can sometimes acquire a packet partway through its preamble, which this model does not simulate.

The no-reset candidate records RF0 register values after the normal board-opening reset. It restores and checks the packet-mode registers before a send, then reconfigures RF0 as the concentrator clock before rearming RF1. The physical repeater still uses the two-reset sequence. Register readback on a model cannot establish that the clock, radio pins and analog front end have switched on silicon. Hardware qualification requires an antenna or RF load, a second receiver to observe the transmission, and an incoming packet after the handover.
