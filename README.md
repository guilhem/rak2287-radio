# RAK2287 KISS modem

Turn a RAK2287 SPI concentrator into a LoRa packet modem for Linux KISS clients. The `rf0` and `rf1` serial ports listen on separate LoRa profiles and share the RF0 transmitter. Clients handle network protocols and apply local rules for frequency, power and airtime.

## Try the simulator

From the repository root on Linux, use GNU Make, a C compiler and Python 3.

```sh
make -j2 kiss-check
```

Look for `OK` after the codec and modem tests exercise framing, independent profiles and shared transmission. To exercise the production driver through a KISS port on a simulated board, initialize `deps/hal` and `deps/usp` and run `make kiss-driver-check`. The full `make check` includes this test.

## Start the modem

Use the RAK2287 SPI variant for your frequency band. On a Linux host with Git, GNU Make and a C compiler, run the modem as a user with access to the SPI device. Stop any existing concentrator software and run the board's reset helper before launching the modem. [Board setup and C API](driver/README.md)

```sh
git submodule update --init deps/hal deps/usp
make -j2 kiss
modem_dir="${XDG_RUNTIME_DIR:-$HOME/.local/run}/rak-kiss"
install -d -m 700 "$modem_dir"
./build/rak-kiss "$modem_dir" /dev/spidev0.0
```

Substitute your host's SPI device if needed. `RAK_KISS_READY` prints both serial paths. Use one client per path, running as the modem user, with KISS port number 0 on each device. Run `kisstool info --dev` with either printed path to query the modem. [RepeaterTastic's `kisstool`](https://github.com/ScotMesh/RepeaterTastic/blob/768a030998b8e3cac18e059bb6eb2a73ad4d1dc6/docs/hardware.md#check-the-modem)

## Configure a port

Reapply the radio settings after a modem restart. The modem uses explicit LoRa headers, CRC and normal IQ. A PHY change can return `TxBusy` while reception or transmission is pending.

| Commands | Effect |
| --- | --- |
| `SetRadio`, `GetRadio` | Set or read frequency and bandwidth in Hz, spreading factor and coding rate denominator. Send `SetRadio` to enable packet delivery on the port. [Supported ranges](driver/sx1250_rx.h) |
| `SetSyncWord`, `SetPreamble`, `GetPhyExtra` | Set or read the LoRa sync word and preamble length. A zero preamble selects 32 symbols through SF8 and 16 above SF8. |
| `SetTxPower`, `GetTxPower` | Set or read signed nominal chip power from −9 to +22 dBm. The default is −9 dBm. Connector output depends on the board RF path. |
| `Data`, `RxMeta`, `SetSignalReport` | Carry raw packets and optional RSSI in dBm and SNR in quarter dB. Signal reports are enabled by default. |

See the command formats in [MeshCore KISS](https://github.com/meshcore-dev/MeshCore/blob/75b04ef06def54b348400cb1b138afb6749ffd4f/docs/kiss_modem_protocol.md) and the [Mesh KISS v2 extensions](https://github.com/ScotMesh/RepeaterTastic/blob/768a030998b8e3cac18e059bb6eb2a73ad4d1dc6/firmware/0001-kiss-modem-sync-word-preamble.patch).

## Shared transmission

The start command above enables reception. With an antenna or suitable load connected, append `--enable-transmit` to allow sending. Each port can queue one frame with its current profile and power setting. The scheduler alternates between waiting ports, defers for active reception, and pauses both receivers during TX.

Use `GetAirtime` and `TxDone` to schedule sends. A successful `TxDone` confirms RF0 completed transmission and both receivers rearmed. A queued send can expire while the radio stays busy. `TXDELAY` sets the minimum per-port wait before transmission.

The RF0 driver sent packets over the air with MeshCore EU Narrow. The KISS modem, power selection and band changes were exercised in simulation. [RF0 bench results](driver/TX.md)

[License](LICENSE) · [Third-party notices](NOTICE.md)
