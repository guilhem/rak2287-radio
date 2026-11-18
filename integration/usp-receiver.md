# Receive two LoRa profiles with the RAK2287

`usp-rx` uses Semtech USP to schedule reception on one RF1 packet engine. One
process owns the SPI device. RF0 supplies the concentrator clock. Each
configured profile gets an exclusive listening slot. A packet that arrives
while its profile is inactive can be missed. The receiver keeps both
external power amplifiers off and its command transport rejects transmission.

## Build and listen

On Linux with the RAK2287 attached by SPI, build the receiver and the MeshCore
observer from the repository root.

```sh
git submodule update --init --recursive
make usp-rx monitor
make check
```

Copy [the example profile file](rx-profiles.conf) and edit the settings to
match your nodes. Before starting the receiver, stop any gateway or observer
using the concentrator (on black1, `meshcore-rx-only.service`) and run the
board's reset helper. Run the receiver under a user with SPI access.

```bash
set -o pipefail
export MESHCORE_DATA_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/usp-meshcore-observer"
mkdir -p -m 700 "$MESHCORE_DATA_DIR"
build/usp-rx integration/rx-profiles.conf /dev/spidev0.0 |
  python3 integration/observe_profiles.py --monitor build/meshcore-rx-monitor \
    --meshcore meshcore --meshtastic meshtastic
```

`usp-rx` can also run on its own. It prints `RX` records containing the
active profile name, monotonic time in microseconds, RSSI in dBm, SNR in dB,
and the payload in hex. Each `SLOT` record describes the finished slot.
`dwell_us` spans listening, packet drains and rearms. `switch_us` measures the
tune call that armed the slot, including the check that the radio has
returned to RX. The first tune includes board setup. A slot can run longer
than its configured duration while a reception finishes. The MeshCore
observer prints `ROUTE` records
for forwarded and duplicate candidates. The Meshtastic observer prints the
clear packet header, including the sender, destination and packet ID. The
encrypted payload stays opaque.

## Configure the profiles

The file accepts one or two lines of settings. Blank lines and lines starting
with `#` are ignored. Profile names must be unique. Values are decimal, with
hexadecimal also available for the sync word. The observer arguments select
the profile names. Without `--meshtastic` it only interprets MeshCore packets.

| Fields | Meaning |
| --- | --- |
| `name frequency_hz bw_hz sf cr sync preamble` | Radio profile. Use the coding rate denominator (for example, `8` for CR 4/8). |
| `crc invert_iq implicit_len` | CRC and IQ inversion are `0` or `1`. Set the implicit length to `0` for explicit headers or set the fixed payload length in bytes. |
| `slot_ms extension_ms priority` | Nominal slot, maximum extension for an in-progress reception and USP priority (`0` highest, `4` lowest). Each profile gets its turn. Equal priorities start in file order. |

The example Meshtastic line targets the reported Paris MediumFast settings.
Change it to match your node's modem preset and frequency override. The example
slot durations are editable.

After stopping the test, check that the SPI device is free before restarting a
previous listener. On black1, `sudo systemctl start meshcore-rx-only.service`
restores the boot service.

## Validation

The simulated receiver covers a packet arriving just before the slot boundary,
an unfinished reception that reaches its extension limit, and a completed
packet drained in standby before the next profile. The observer test verifies
MeshCore duplicate and relay decisions and extracts a synthetic Meshtastic
header.

On black1, a 25-second receive-only run completed five MeshCore slots and
four Meshtastic slots. Retunes took 54.8–57.0 ms after initial board setup.
The median slot durations were 4.003 s and 1.002 s. No packets arrived during
that run. Over-the-air decoding with the USP backend requires a reference
transmitter.
