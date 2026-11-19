# RAK2287 KISS modem

Use a RAK2287 SPI concentrator as a LoRa packet modem on Linux. Two virtual serial ports let KISS clients set their own LoRa parameters and listen in parallel. Both ports share the transmitter on RF0. The modem carries raw LoRa bytes and signal measurements. Host applications handle network protocols and apply local rules for frequency, power and airtime.

## Try it without a concentrator

From this checkout on Linux, use GNU Make, a C compiler and Python 3.

```sh
make -j2 kiss-check
```

The tests configure two serial ports on a simulated modem, receive raw packets with RSSI and SNR, and submit overlapping send requests. The simulated radio handles the sends in sequence with the power selected on each port when it queued the frame. The test runner reports `OK` on success.

## Listen with a RAK2287

Connect a RAK2287 SPI variant that matches the frequency band you plan to use to a Linux host with SPI enabled. Give your user access to the SPI device and run the board reset helper for your hardware after stopping the process that currently owns the concentrator. [Board and SPI setup](driver/README.md)

```sh
git submodule update --init deps/hal deps/usp
make -j2 kiss
install -d -m 700 "$XDG_RUNTIME_DIR/rak-kiss"
./build/rak-kiss "$XDG_RUNTIME_DIR/rak-kiss" /dev/spidev0.0
```

Use the SPI device for your host and a dedicated absolute runtime directory. When `RAK_KISS_READY` prints, open `rf0` or `rf1` inside that directory with a local KISS client running as the same user. Use one client per port, with KISS port number 0 on each serial device. `kisstool info --dev "$XDG_RUNTIME_DIR/rak-kiss/rf0"` reports `Mesh KISS v2` and the current settings. Send `SetRadio` on a port to activate packet delivery. The command above runs in receive-only mode. [RepeaterTastic's `kisstool`](https://github.com/ScotMesh/RepeaterTastic/blob/768a030998b8e3cac18e059bb6eb2a73ad4d1dc6/docs/hardware.md#check-the-modem)

## Configure a port

The modem uses the LoRa radio commands from MeshCore KISS with the Mesh KISS v2 additions from ScotMesh. An accepted setting applies to that receive chain and to sends on behalf of the same port. Packets use an explicit LoRa header, CRC and normal IQ. A receive in progress on the selected chain or an RF0 transmission can defer a profile change with `TxBusy`. Read the applied settings back with the corresponding `Get` command. Reapply the desired profiles when the service restarts.

| Commands | Effect |
| --- | --- |
| `SetRadio` / `GetRadio` | Center frequency and bandwidth in Hz, spreading factor and coding rate denominator. The [driver's ranges](driver/sx1250_rx.h) apply. |
| `SetSyncWord`, `SetPreamble`, `GetPhyExtra` | Sync word and preamble length in symbols. A zero preamble selects 32 symbols through SF8 and 16 above SF8. |
| `SetTxPower` / `GetTxPower` | Select and read back a nominal chip setting from −9 to +22 dBm on this port. The modem saves it with each new Data request. Values outside this range return `InvalidParam`. |
| `Data`, `SetSignalReport`, `RxMeta` | Raw LoRa packets and optional RX metadata. By default each forwarded packet includes RSSI in dBm and SNR in quarter dB. |
| `Data`, `TxDone` | Queue one send per port. A successful `TxDone` means RF0 completed transmission and both receive chains rearmed. |

`IsChannelBusy` reports observed packet reception on either chain or an RF0 handoff in progress. `Ping`, `GetVersion`, `GetDeviceName`, `GetStats` and `GetAirtime` support modem queries. The serial `TXDELAY` command sets the minimum wait before a request can start. [MeshCore modem commands](https://github.com/meshcore-dev/MeshCore/blob/75b04ef06def54b348400cb1b138afb6749ffd4f/docs/kiss_modem_protocol.md) · [Mesh KISS v2 additions](https://github.com/ScotMesh/RepeaterTastic/blob/768a030998b8e3cac18e059bb6eb2a73ad4d1dc6/firmware/0001-kiss-modem-sync-word-preamble.patch)

Both hardware ports start at the nominal −9 dBm setting (signed byte `0xf7`). `GetTxPower` reports the selected chip setting. Connector power depends on the board RF path. The external power amplifier stays off in packet mode.

## Shared transmission

With an antenna or suitable load connected, `--enable-transmit` allows Data requests to use RF0. The modem rotates between pending ports and waits for both receive chains to be idle. Both chains pause during TX. Read-only KISS queries continue to work on both ports. A pending request can expire while the radio stays busy. Applications can use `GetAirtime` and `TxDone` to schedule sends. Treat a missing or failed `TxDone` as unconfirmed delivery. On shutdown, the modem waits for any active RF0 handoff to finish before releasing the board.

The underlying transmitter was tested over the air with MeshCore EU Narrow. The KISS service, additional power settings and changes between calibration bands were exercised in simulation. [RF0 bench results](driver/TX.md) · [Driver development and board ownership](driver/README.md)

[License](LICENSE) · [Third-party notices](NOTICE.md)
