# Source and license notices

The original driver and Linux integration code in this repository use the [BSD 3-Clause License](LICENSE). Dependency checkouts are pinned by Git submodules. Distributors of linked binaries should include the notices for the code they bundle.

| Component | Origin and notice |
| --- | --- |
| SX1302 HAL and TinyMT | [Semtech V2.1.0 distribution notices](https://github.com/Lora-net/sx1302_hal/blob/4b42025d1751e04632c0b04160e0d29dbbb222a5/LICENSE.TXT), including TinyMT and the packet forwarder's Parson notice |
| Native MeshCore simple repeater | [MeshCoreRPI, MIT](https://github.com/dabeani/MeshCoreRPI/blob/5f7def6f14fbd0562a81d71c18d93739b65e029c/license.txt), including the Raspberry Pi port attribution |
| Ed25519 code linked by the repeater | [Orson Peters, zlib](https://github.com/dabeani/MeshCoreRPI/blob/5f7def6f14fbd0562a81d71c18d93739b65e029c/lib/ed25519/license.txt) |
| Arduino Crypto | [Southern Storm Software, MIT](https://github.com/rweather/arduinolibs/blob/37a76b8f7516568e1c575b6dc9268da1ccaac6b6/libraries/LICENSE.txt) and the notices in the linked source files |
| SX126x command definitions referenced by the driver | [Semtech Clear BSD notice](driver/licenses/Semtech-sx126x-driver.txt) |
