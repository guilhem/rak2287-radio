# Use the modem from a Mesh KISS client

The modem creates two virtual serial ports named `rf0` and `rf1`. Open one
client per device at 115200 baud with KISS port number `0`. The numbers in the
device paths indicate which physical radio receives on that port. Client
applications handle the mesh protocol.

## Set the radio profile

Send `SetRadio` with a profile for each port you want to receive on. Match the
frequency, bandwidth, spreading factor, coding rate, sync word, and preamble
to the intended peer. Reapply the settings after restarting the modem. Both
ports use explicit headers, CRC, and normal IQ.

| Commands | Purpose |
| --- | --- |
| `SetRadio`, `GetRadio` | Enable packet delivery with a profile or read back the profile. Frequency and bandwidth are in Hz, and coding rate is the denominator of `4/n`. [Supported ranges](../driver/sx1250_rx.h) |
| `SetSyncWord`, `SetPreamble`, `GetPhyExtra` | Set or read the remaining PHY settings. A zero preamble selects 32 symbols through SF8 and 16 symbols above SF8. |
| `SetTxPower`, `GetTxPower` | Set or read the nominal chip power from −9 to +22 dBm. The default is −9 dBm. Power at the antenna connector depends on the board. |
| `Data`, `RxMeta`, `SetSignalReport` | Exchange raw packet bytes and report RSSI in dBm and SNR in quarter dB. Signal reporting starts enabled. |

If a PHY change returns `TxBusy`, retry after active reception or transmission
has finished.

## Send through RF0

When the modem runs with `--mode receive-and-transmit`, each port can queue
one frame with its own settings. RF0 handles both ports' transmissions.
The modem defers a send for active reception and pauses both receivers
during transmission. A queued frame can expire while the radio stays busy.

Use `GetAirtime` to budget transmissions and `TXDELAY` to set the minimum
wait for each port. A successful `TxDone` means RF0 completed transmission
and both receivers resumed.

[MeshCore KISS specification](https://github.com/meshcore-dev/MeshCore/blob/75b04ef06def54b348400cb1b138afb6749ffd4f/docs/kiss_modem_protocol.md) · [Mesh KISS v2 extensions](https://github.com/ScotMesh/RepeaterTastic/blob/768a030998b8e3cac18e059bb6eb2a73ad4d1dc6/firmware/0001-kiss-modem-sync-word-preamble.patch) · [RF0 bench results](../driver/TX.md)
