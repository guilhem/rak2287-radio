# Optional RF0 LoRa transmission

The TX library lets a serialized application send one LoRa frame through RF0 and resume receiving on RF1. The bench results below came from the RAK2287 EU868. The normal single-receiver send switches RF0 between packet mode and the concentrator clock without per-packet resets. It uses a read-only register capture taken after the reset at board open.

On black1, RF0 reported TxDone for 11- and 37-byte EU Narrow frames. The send calls took 1.465 s and 1.662 s, including both resets and the return to RX. The same process decoded 15 RF1 frames in the minute after the 37-byte transmission. The operator confirmed a test message arrived on a nearby MeshCore node. RF0 output power and occupied spectrum remain unmeasured.

Build the ordinary RX library and explicitly add the TX archive.

```sh
make HAL=/path/to/sx1302_hal libsx1250_rx.a libsx1250_tx.a
```

Include `sx1250_tx.h` and link `libsx1250_tx.a`, then `libsx1250_rx.a`, then the stock Semtech HAL and its dependencies. After opening a LoRa RX profile, call `sx1250_tx_send(payload, length, timeout_ms)`. Supply the complete PHY bytes. The existing frequency, SF, bandwidth, coding rate, sync word, preamble, CRC and IQ polarity apply to the transmission. An implicit-header frame must use the configured fixed length. Choose a timeout long enough for the selected PHY and payload. The application owns airtime accounting and channel access.

Call `sx1250_rx_activity()` before scheduling a transmission. A return of 1 means RF1 has detected a LoRa preamble, accepted an explicit header, or latched a terminal event for `sx1250_rx_next()`. Keep the outgoing packet queued and continue polling the receiver. A return of 0 permits a send attempt. A negative result reports an error through `errno`. The detector uses the configured LoRa profile. Other signals on the channel may go undetected.

The send call samples RF1 again immediately before putting it in standby. On `EAGAIN`, RF0 has not started. Service `sx1250_rx_next()` and retry the queued packet once activity clears. If a new preamble appears between the final SPI sample and the standby command, the handover can still interrupt it. A completed packet latched during that interval remains available to read.

Success confirms that RF0 reported TxDone and RF1 was rearmed. A failed transition triggers at most one cold recovery to RX. If TxDone was confirmed and cold recovery restored RF1, the send succeeds with the recovery recorded. Call `sx1250_tx_send_report()` to inspect the failed stage, cold recovery, TxDone, RF1 rearm and RF1 downtime at the SPI command boundaries. Only `EAGAIN` asks the application to defer and retry. An I/O error can follow an actual transmission. If the receiver also returns `EIO`, close it, reset the board and reopen it. The timeout bounds the RF transmission and its IRQ wait. Setup and recovery add their own delays.

RF0 first leaves the concentrator's I/Q mode while the radio interface uses its SPI clock. The driver keeps both external PA enables low and disables both LNAs during this transaction. RF0 uses the reduced-output PA settings `02 02 00 01`. `SetTxParams` defaults to −9 dBm. The dual-receiver API accepts signed nominal settings from −9 to +22 dBm. Output power at the antenna connector depends on the board's RF path. Attach an appropriate load or antenna for real TX. [Semtech SX1250 datasheet](https://mm.digikey.com/Volume0/opasdata/d220001/medias/docus/694/SX1250_rev1.2_Sept2019.pdf#page=15)

For a software-only integration, link the two driver archives against `libsx1250_fake_hal.a` in place of the hardware HAL. Set `FAKE_TX=1` and `IDLE=1`, then open a temporary ordinary file as the SPI path. The test header exposes `fake_inject_rx()` for a complete RF1 payload and `fake_last_tx()` to inspect the most recent RF0 TxDone bytes. The archive and test hooks are built only on request or by `make check`.

## Check one RF0 transmission and the return to RF1

Build from the repository root using the pinned submodules.

```sh
make hal
make -C driver HAL=../deps/hal sx1250-tx-probe
```

With an antenna attached, stop the current concentrator service and reset the
board. Supply one payload as hexadecimal bytes. This example sends a raw PHY
probe with MeshCore EU Narrow settings and listens on RF1 for ten seconds.

```sh
driver/sx1250-tx-probe --enable-transmit /dev/spidev0.0 010203 10000
```

`TX_OK` reports the TxDone IRQ, RF1 rearming, estimated airtime and total
send-call duration. Confirm reception of the same bytes using a separate
receiver. After rearming, the probe prints received packets in the same
tab-separated format as `sx1250-rx`. Set the optional listening duration to
`0` to exit after the send. A busy receiver produces `TX_DEFERRED` and exit code
3. Any ambiguous send reports `TX_UNCONFIRMED`. The probe makes one send call
per invocation.

## Measure both transitions with the probe

With the SPI device free and the board reset, run the dry probe first. It
configures RF0 for packet mode, restores the I/Q clock and rearms RF1 without
issuing SetTx. Keep the probe listening and send a fresh frame from a separate
EU Narrow node to verify that RF1 can receive again.

```sh
driver/sx1250-tx-probe --warm-dry-run /dev/spidev0.0 30000
```

For a paired measurement with an antenna or RF load attached, use
`--enable-cold-transmit` for the two-reset control and `--enable-transmit`
for the normal send. In each case pass the device path, one hexadecimal
payload and the receive duration in milliseconds. Use equal payload lengths
and confirm reception with a separate receiver. To isolate one transition,
use `--enable-warm-transmit=prepare` or `--enable-warm-transmit=restore`.
The explicit warm diagnostic treats a fallback as a failed trial.

Compare the measured interval from RF1 standby to RF1 SetRx in the
`HANDOFF` records. A successful dry run reports `tx_started=0` and
`rf1_rearmed=1`. The probe reports a failing stage and any recovery through
a restore reset. A potentially transmitted packet is never resent by the probe.

On black1, the dry probe completed twice with zero `SetTx` calls and no
recovery reset. The first run decoded 16 subsequent RF1 packets. With a
37-byte EU Narrow test frame, RF0 reported `TxDone` in both send modes. RF1
was unavailable for **687 ms without a reset** and **1,649 ms with both
resets**, measured at the SPI command boundaries. The warm send reported
no cold recovery. A separate MeshCore node confirmed receipt of the warm
test message over the air, and the same probe decoded a subsequent RF1 frame.

## Measure RF1 downtime without a radio

From the repository root with the submodules initialized, run:

```sh
make bench-handover
make bench-handover-wall
```

The first command compares the two-reset control with the normal send sequence that switches RF0 between the concentrator's I/Q mode and the LoRa packet engine. Both use the generic driver and the pinned Semtech configuration and SPI routines. The benchmark replaces register I/O with a radio model and uses synthetic packets. The second command repeats the comparison with host monotonic time and real sleeps. Neither command needs a Raspberry Pi, a SPI device or root.

The timing starts when the application requests a send. RF1 becomes unavailable at its standby command and listening resumes at its next successful `SetRx`. The driver subsequently checks RF1 and returns to the application. The RF0 transmit interval runs from `SetTx` to the first poll that sees `TxDone`. The model supplies the on-air duration for the chosen payload. Linux scheduling adds variation to the wall-clock run.

For a 32-byte EU Narrow transmission (869.618 MHz, SF8, 62.5 kHz, CR 4/8), the deterministic run produces the following times in milliseconds.

| Interval | Two resets | No per-packet reset |
| --- | ---: | ---: |
| API entry to RF1 standby | 3 | 4 |
| RF1 standby to RF0 SetTx | 619 | 131 |
| RF0 SetTx to observed TxDone | 411 | 411 |
| Observed TxDone to RF1 SetRx | 574 | 97 |
| **RF1 unavailable** | **1,604** | **639** |
| Additional verification before the API returns | 11 | 11 |
| Total time in the send call | 1,618 | 654 |

The modeled packet takes 410.624 ms on air. Most of the 965 ms saved comes from removing two 520 ms reset sequences. Extra register transfers and the clock-counter check consume some of that saving. The measured handover excludes the board-opening reset and its one-time register capture.

For each sequence, the bench runs nine separate transmissions. In each trial, a 10-byte incoming frame starts at a different offset from the send request, from zero to two seconds in 250 ms steps. The model requires uninterrupted listening from the first symbol to the last. The two-reset sequence receives the probes at 1.75 and 2 s (2/9). The normal sequence receives all six probes starting at 0.75 s or later (6/9). These counts describe that schedule. Radios can sometimes acquire a packet partway through its preamble, which this model does not simulate.
