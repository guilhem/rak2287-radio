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
observed route hashes. The target uses the upstream forwarding and advert
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

`startSendRaw` reports success after TxDone and verified RF1 rearming.
An I/O or transmit timeout error stops the process and closes the radio.
The adapter retries only an explicitly unstarted transmission. The final
`sent` count covers completed driver transactions. A peer must verify
physical reception.
