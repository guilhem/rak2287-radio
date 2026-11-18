# Feed a Linux MeshCore repeater from a receive-only radio

The monitor passes received LoRa payloads to the Linux port of MeshCore's
`simple_repeater/MyMesh`, dispatcher and duplicate table. It prints the router's
decision for each packet. It discards forwarding decisions before they enter
the transmit queue. Its radio interface also rejects every send request, including
packets generated locally by the firmware. Only the separate capture process
opens the physical radio.

## Build and run

Use Linux with a C compiler, C++17 compiler and standard library (including
floating-point `from_chars`), GNU make, patch and Git. From the repository root:

```sh
git submodule update --init --recursive
make -j3 monitor
export MESHCORE_DATA_DIR="$HOME/.local/state/meshcore-observer"
mkdir -p -m 700 "$MESHCORE_DATA_DIR"
printf 'RX\t-77.5\t-2.25\t0e0001020304\n' | build/meshcore-rx-monitor
```

The example processes one direct ACK. For live input, pipe the TSV output of
the capture process into `meshcore-rx-monitor` in the same environment.
Fresh state uses the port's default routing preferences. Use a dedicated
directory. The observer has a fresh random identity per process and starts
with an empty duplicate table. It does not advertise or accept remote
administration. A dedicated state directory avoids loading another node's
preferences, ACL or region settings.

Input records have four tab-separated fields, terminated by a newline.
`RX`, RSSI in dBm, SNR in dB and continuous hexadecimal payload bytes.
The adapter checks lengths, the firmware's MeshCore version/path encoding
and the range of the firmware's signed quarter-dB SNR. The producer selects
frequency, bandwidth, sync word, coding rate and CRC policy. TSV carries
no physical-layer verification fields. The EU Narrow profile is used for
the firmware's airtime estimate. The observer processes incoming packets
immediately so a finite capture has a deterministic input order.

Each `ROUTE` line records the firmware's packet hash, original route and
hop count, radio metadata, and whether its duplicate counter increased during
processing. `repeat_requested=1` means the router returned a forwarding
action. Protocol handlers can also create separate replies. The `SUMMARY`
line counts rejected records, returned forwarding actions and any calls that
reached the disabled send method. Every run ends with `sent=0`.
A router classification alone does not authenticate a message or decrypt its
channel contents.

## Test the observer with synthetic frames

```sh
python3 integration/test_monitor.py build/meshcore-rx-monitor
```

The tests feed four synthetic frames into a clean native process.
The firmware identifies four group-text floods, counts two duplicates and
requests two repeats. Additional records exercise direct duplicate handling,
invalid TSV, incomplete frames, oversized input and recovery at the next
newline. These tests run without hardware.

The build compiles the pinned native Arduino compatibility layer and repeater
sources directly. The target-specific patch selects the firmware's unsupported
response for its SPI diagnostics command. Neither a physical radio backend nor
the native port's TCP console is linked. The build does not modify either
checkout.

See the [optional radio repeater](radio-repeater.md) for the bidirectional driver integration.

Sources: [MeshCoreRPI](https://github.com/dabeani/MeshCoreRPI/tree/5f7def6f14fbd0562a81d71c18d93739b65e029c)
and [Arduino Crypto](https://github.com/rweather/arduinolibs/tree/37a76b8f7516568e1c575b6dc9268da1ccaac6b6).
