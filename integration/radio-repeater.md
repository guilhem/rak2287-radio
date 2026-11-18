# Native MeshCore radio repeater for RAK2287 EU868

The optional Linux executable connects MeshCoreRPI's `simple_repeater` to the
external C radio driver. It receives MeshCore EU Narrow on RF1 and sends the
firmware's serialized packets on RF0. The dispatcher handles flood paths,
duplicate suppression and its transmit queue. For receive-only monitoring, build the [stdin observer](native-monitor.md).

## Test with simulated radios

Initialize the pinned dependencies under `deps/` and run the tests from the
repository root:

```sh
git submodule update --init --recursive
make -j3 check
```

The tests use synthetic packets and ordinary temporary files. The same
repeater entry point, C++ backend and C RX/TX driver as the optional hardware
executable are linked to a HAL model. The test checks the bytes that reach
the RF0 FIFO and pass the model's TxDone, then injects another frame on
RF1. Both retransmitted frames have one additional two-byte hop matching
the repeater's public key. A copy of the first packet increments the
firmware's duplicate counter and leaves the count of transmissions at two.
Other cases cover a preamble on an already queued send, a lost preamble,
a terminal event, a rejected frame, an activity read failure, and the
dispatcher's forced send attempt after four seconds. The tests also cover
failure during transmit or return to RX, explicit activation and persistent
identity.

## Optional hardware executable

Build the executable from the repository root:

```sh
make -j3 radio-repeater
```

The executable requires a board prepared according to the driver instructions,
exclusive access to SPI and an antenna or suitable load. Launching it with the
following flag enables physical transmissions including firmware-generated
packets. The RF0 experimental transmit path requires independent RF validation.

```sh
mkdir -p -m 700 "$HOME/.local/state/meshcore-rak2287"
export MESHCORE_DATA_DIR="$HOME/.local/state/meshcore-rak2287"
build/meshcore-radio-repeater --enable-transmit --device /dev/spidev0.0
```

The state directory must belong to the process user and deny group/other
access. The program locks it and writes the standard 96-byte MeshCore
identity to `repeater.id` with private permissions. Keep the state directory
to retain the repeater's public identity. An unreadable or invalid existing
identity stops startup. Startup prints the public key for comparison with
observed route hashes. `RADIO_RX_READY` confirms that RF1 is listening.
The target uses the upstream forwarding and advert
timers. Its anonymous login and peer command callbacks are disabled.

The adapter requires 869.618 MHz, SF8, BW 62.5 kHz, CR 4/8, private sync 0x12,
explicit headers, normal IQ and payload CRC. The driver owns antenna switching
and the experimental PA settings. MeshCore schedules its randomized forwarding
delay and observes the dispatcher’s post-send airtime budget. Its transmit
accounting includes the synchronous driver call and restoration of reception. During continuous operation the target enforces
a minimum of nine times that duration as post-send silence. Stricter stored
factors are preserved. This spacing restarts with the process and does not
establish regulatory conformity.

### Waiting for RF1 before transmitting

When a packet becomes eligible for transmission, MeshCore calls
`Radio::isReceiving()` before removing it from its queue. The adapter reads
the live RF1 LoRa receiver IRQs. A detected preamble, a valid header, or a
terminal event waiting for `recvRaw` makes the dispatcher defer the queued
transmission. MeshCore retries its check after a randomized 120–360 ms.
A read error stops the repeater and closes the driver.

For EU Narrow a lone preamble is held for about 164 ms from its first
observation. A header gets a longer window for a complete packet. The
driver bounds stale indications and leaves terminal events for the receive
loop. A valid incoming packet goes through the normal MeshCore dispatcher.
The driver discards CRC/header errors. The adapter rejects valid PHY packets
with a mismatched coding rate or invalid MeshCore envelope.
This passive indication concerns the configured LoRa receiver. It provides
no RSSI energy scan or independent CAD measurement. Activity that has not
yet raised a monitored IRQ can still overlap a transmission.

MeshCore tries to send after four seconds of continuously observed activity.
Activity may also begin between its check and `startSendRaw`. The driver's
final guard can return `EAGAIN` in either case, before starting RF0.
The adapter returns the **same packet at its original priority** to MeshCore's
outbound queue with a new retry delay. The receive loop can then drain RF1.
The `busy_deferrals` counter counts these final-guard vetoes. Earlier
backoffs in `isReceiving()` leave the packet in the queue and do not
increment this counter.

`startSendRaw` uses the generic driver's no-reset RF0 handover. It reports
success after TxDone and verified RF1 rearming, including a successful cold
recovery after TxDone. Each RF0 attempt logs `RADIO_TX` with RF1 downtime,
TxDone, the RF1 rearm result and any recovery. A failed send stops the
process and closes the radio.
The adapter retries only an explicitly unstarted transmission. The final
`sent` count covers completed driver transactions. A peer must verify
physical reception.

## Run at boot on black1

Once the RF setup has been qualified for continuous operation, install the
service and the native binary from the repository root on black1:

```sh
sudo install -D -m 0755 build/meshcore-radio-repeater /usr/local/libexec/meshcore-radio-repeater
sudo install -m 0644 integration/meshcore-repeater.service /etc/systemd/system/meshcore-repeater.service
sudo systemctl daemon-reload
sudo systemctl disable meshcore-rx-only.service
sudo systemctl enable --now meshcore-repeater.service
journalctl -u meshcore-repeater.service -f
```

The unit stores the identity under `/var/lib/meshcore-repeater`. It stops
the receive-only service to take SPI ownership. On failure it starts
receive-only monitoring for the rest of the boot. Restore that mode as
the boot default with:

```sh
sudo systemctl disable --now meshcore-repeater.service
sudo systemctl enable --now meshcore-rx-only.service
```

## Hardware relay check

On black1, a one-frame test harness passed an over-the-air EU Narrow message
from RF1 to the native repeater. The test checked that MeshCore preserved the
body and added the public key prefix as a two-byte hop before allowing one
physical send. The 71-byte relay reached RF0 TxDone and verified RF1 rearm.
The send call, including both resets, took 1.965 s. The originating node's
app reported one repeater response, with the matching two-byte route
`6143`, RSSI −115 dBm and SNR −4.5 dB. The test unit exited and the
receive-only service recovered the SPI device.

A subsequent monitored run used the installed repeater binary with the generic
no-reset send path. It accepted 18 MeshCore packets, recognized four flood
duplicates, and completed 12 sends with TxDone and RF1 rearmed on every send.
All 12 completed without a recovery reset. RF1 was unavailable for 853–861 ms
for the five 55-byte group messages in the run. The companion showed the
repeater hop `B442` for two of the five test messages.

The installed systemd unit loaded the same identity and reached RF1 listening
on a separate start. An eight-second runtime limit injected for the test
stopped the repeater and triggered the receive-only service through
`OnFailure=`. The runtime limit was removed after verification.
