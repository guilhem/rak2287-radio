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
Other cases cover reception pending at the instant of a send request,
failed send/return-to-RX, explicit activation and persistent identity.

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

The API has no channel-activity detector and the backend inherits
`Radio::isReceiving() == false`. An already latched terminal RX event can
defer a send. A preamble or an in-progress packet can be interrupted when
the driver switches to RF0. The firmware’s channel-busy backoff is therefore
inactive for this target.

`startSendRaw` reports success only after the driver observes TxDone and
rearms RF1. The next dispatcher cycle acknowledges completion. If an RX
event is pending before transmission, the adapter preserves it for
`recvRaw` and retries the unstarted send once. A second collision increments
`busy_drops` and leaves RF1 available for the incoming frame. An I/O or
timeout error stops the process and closes the radio. An ambiguous send
is never automatically repeated. The final `sent` counter represents
completed driver transactions. The peer must verify physical reception.
