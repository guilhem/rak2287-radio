use std::process::Command;

const MODEM: &str = env!("CARGO_BIN_EXE_sx130x-kiss");

#[test]
fn help_and_version_work_without_a_board() {
    let help = Command::new(MODEM).arg("--help").output().unwrap();
    assert!(help.status.success(), "{help:?}");
    let text = String::from_utf8(help.stdout).unwrap();
    for expected in [
        "LoRa KISS modem for SX1302 SPI concentrators with two SX1250 radios",
        "Usage: sx130x-kiss [OPTIONS]",
        "--spi-device <PATH>",
        "[default: /dev/spidev0.0]",
        "--runtime-dir <DIR>",
        "$XDG_RUNTIME_DIR/sx130x-kiss or $HOME/.local/run/sx130x-kiss",
        "--rf0-path <PATH>",
        "--rf1-path <PATH>",
        "--rf0-group <GROUP>",
        "--rf1-group <GROUP>",
        "--mode <MODE>",
        "[default: receive-only]",
        "receive-only:",
        "Receive packets on both ports",
        "receive-and-transmit:",
        "Let KISS clients receive packets and send through RF0",
        "sx130x-kiss --spi-device /dev/spidev0.0",
        "--runtime-dir \"$HOME/.local/run/sx130x-kiss\" --mode receive-only",
    ] {
        assert!(text.contains(expected), "missing {expected:?} in {text}");
    }

    let version = Command::new(MODEM).arg("--version").output().unwrap();
    assert!(version.status.success(), "{version:?}");
    assert_eq!(
        String::from_utf8(version.stdout).unwrap().trim_end(),
        concat!("sx130x-kiss ", env!("CARGO_PKG_VERSION"))
    );
}

#[cfg(any(feature = "sim", feature = "fake-hal"))]
#[test]
fn kiss_over_linux_ptys() {
    let suite = if cfg!(feature = "fake-hal") {
        "DriverModemTest"
    } else {
        "ModemTest"
    };
    let status = Command::new("python3")
        .args(["-B", "kiss/tests/test_modem.py", MODEM, suite])
        .status()
        .expect("Python 3 is required for the PTY tests");
    assert!(status.success(), "{suite} exited with {status}");
}
