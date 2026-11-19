"""Exercise the modem's Linux PTYs against an instrumented radio."""

from collections import deque
from contextlib import contextmanager, nullcontext
from pathlib import Path
from select import select
import os
import struct
import subprocess
import sys
import tempfile
import time
import tty
import unittest

MODEM = Path(sys.argv.pop(1)).resolve()
FEND = b"\xc0"
OK = b"\x06\xf0"
PONG = b"\x06\x97"
VERSION = b"\x06\x91\x02\x00"
TX_OK = b"\x06\xf8\x01"
TX_FAILED = b"\x06\xf8\x00"
TX_BUSY = b"\x06\xf1\x07"
PARIS = struct.pack("<IIBB", 869300000, 250000, 9, 5)
NARROW = struct.pack("<IIBB", 869618000, 62500, 8, 8)


def unescape(wire):
    decoded = bytearray()
    data = iter(wire)
    for byte in data:
        if byte == 0xDB:
            escape = next(data, None)
            if escape == 0xDC:
                byte = 0xC0
            elif escape == 0xDD:
                byte = 0xDB
            else:
                raise ValueError(f"Invalid escape in modem frame {wire.hex()}")
        decoded.append(byte)
    return bytes(decoded)


class Port:
    def __init__(self, path):
        self.path = path
        self.buffer = b""
        self.events = deque()

    def __enter__(self):
        self.fd = os.open(self.path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(self.fd)
        return self

    def __exit__(self, *exc):
        os.close(self.fd)

    def write(self, data):
        deadline = time.monotonic() + 5
        while data:
            left = max(0, deadline - time.monotonic())
            if not select([], [self.fd], [], left)[1]:
                raise TimeoutError(f"Modem stopped reading {self.path}")
            data = data[os.write(self.fd, data):]

    def send(self, frame):
        self.write(FEND + frame.replace(b"\xdb", b"\xdb\xdd").replace(FEND, b"\xdb\xdc") + FEND)

    def receive(self, deadline):
        while True:
            if FEND in self.buffer:
                wire, self.buffer = self.buffer.split(FEND, 1)
                if wire:
                    return unescape(wire)
                continue
            left = deadline - time.monotonic()
            if left <= 0 or not select([self.fd], [], [], left)[0]:
                raise TimeoutError(f"No modem response on {self.path}")
            data = os.read(self.fd, 4096)
            if not data:
                raise EOFError(f"Modem closed {self.path}")
            self.buffer += data

    def command(self, opcode, payload=b"", *, timeout=5):
        self.send(bytes([0x06, opcode]) + payload)
        deadline = time.monotonic() + timeout
        while True:
            frame = self.receive(deadline)
            if frame[0] == 0x00 or frame[:2] in (b"\x06\xf8", b"\x06\xf9"):
                self.events.append(frame)
            else:
                return frame

    def event(self):
        return self.events.popleft() if self.events else self.receive(time.monotonic() + 5)


class Trial:
    def __init__(self, directory, log):
        self.directory = directory
        self.gate = directory / "release-tx"
        self.log_file = log

    def port(self, rf):
        return Port(self.directory / f"rf{rf}")

    def wait_for_tx_start(self):
        deadline = time.monotonic() + 5
        while b"SET_TX " not in os.pread(self.log_file.fileno(), 8192, 0):
            if time.monotonic() >= deadline:
                raise TimeoutError("Modem never entered simulated TX")
            time.sleep(0.01)

    @property
    def tx_calls(self):
        return [line for line in self.log.splitlines() if line.startswith("SET_TX ")]


class ModemTest(unittest.TestCase):
    @contextmanager
    def modem(self, *, tx=True, hold_tx=False, hold_in_flight=False, directory=None, **env):
        runtime = nullcontext(directory) if directory is not None else tempfile.TemporaryDirectory()
        with runtime as dirname, tempfile.TemporaryFile() as log:
            trial = Trial(Path(dirname), log)
            if hold_tx:
                env["FAKE_TX_GATE"] = str(trial.gate)
            if hold_in_flight:
                env["FAKE_TX_IN_FLIGHT"] = str(trial.gate)
            args = [MODEM, trial.directory, "/simulated-spi"]
            if tx:
                args.append("--enable-transmit")
            with subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT,
                                  env={**os.environ, **env}) as process:
                try:
                    deadline = time.monotonic() + 5
                    while b"RAK_KISS_READY" not in os.pread(log.fileno(), 8192, 0):
                        if process.poll() is not None or time.monotonic() >= deadline:
                            output = os.pread(log.fileno(), 8192, 0).decode(errors="replace")
                            self.fail(f"Modem did not report readiness\n{output}")
                        time.sleep(0.01)
                    yield trial
                finally:
                    if process.poll() is None:
                        process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                        raise
                    finally:
                        log.seek(0)
                        trial.log = log.read().decode(errors="replace")
                self.assertEqual(process.returncode, 0, trial.log)
                self.assertFalse((trial.directory / "rf0").is_symlink())
                self.assertFalse((trial.directory / "rf1").is_symlink())

    def test_runtime_directory_recovers_stale_ports_and_excludes_a_second_owner(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            for rf in (0, 1):
                (directory / f"rf{rf}").symlink_to(f"/dev/pts/{2147483646 + rf}")
            with self.modem(directory=directory) as trial, trial.port(0) as port:
                competing = subprocess.run([MODEM, directory, "/simulated-spi"],
                                           capture_output=True, timeout=5)
                self.assertNotEqual(competing.returncode, 0)
                self.assertEqual(port.command(0x17), PONG)

    def configure(self, port, profile, sync, preamble):
        self.assertEqual(port.command(0x09, profile), OK)
        self.assertEqual(port.command(0x1B, bytes([sync])), OK)
        self.assertEqual(port.command(0x1C, struct.pack("<H", preamble)), OK)

    def test_independent_phy_configuration_and_received_frames(self):
        with self.modem(FAKE_RX="1") as trial, trial.port(0) as rf0, trial.port(1) as rf1:
            self.assertEqual(rf0.command(0x17), PONG)
            self.assertEqual(rf1.command(0x11), VERSION)
            self.configure(rf0, PARIS, 0x2B, 16)
            self.configure(rf1, NARROW, 0x12, 24)
            self.assertEqual(rf0.command(0x0B), b"\x06\x8b" + PARIS)
            self.assertEqual(rf1.command(0x0B), b"\x06\x8b" + NARROW)
            self.assertEqual(rf0.command(0x1D), b"\x06\x9d\x2b\x10\x00")
            self.assertEqual(rf1.command(0x1D), b"\x06\x9d\x12\x18\x00")
            self.assertEqual(rf1.command(0x1C, b"\x00\x00"), OK)
            self.assertEqual(rf1.command(0x1D), b"\x06\x9d\x12\x20\x00")
            for rf, port, snr, rssi in ((0, rf0, 17, -95), (1, rf1, -14, -96)):
                self.assertEqual(port.event(), b"\x00\x15\xc0\xdb" + bytes([rf]))
                self.assertEqual(port.event(), b"\x06\xf9" + struct.pack("bb", snr, rssi))

    def test_invalid_and_failed_configuration_preserve_readback(self):
        with self.modem(FAKE_RECONFIG_ERROR="1") as trial, trial.port(0) as port:
            self.configure(port, PARIS, 0x2B, 16)
            self.assertEqual(port.command(0x09, PARIS[:-1]), b"\x06\xf1\x01")
            bad_freq = struct.pack("<IIBB", 0xFFFFFFFF, 250000, 9, 5)
            self.assertEqual(port.command(0x09, bad_freq), b"\x06\xf1\x02")
            fault = struct.pack("<IIBB", 868800000, 250000, 9, 5)
            self.assertEqual(port.command(0x09, fault), b"\x06\xf1\x03")
            self.assertEqual(port.command(0x0B), b"\x06\x8b" + PARIS)

    def test_both_ports_share_rf0_using_their_own_profiles(self):
        payload = b"\x22\xc0\xdb" * 85
        with self.modem(hold_tx=True) as trial, trial.port(0) as rf0, trial.port(1) as rf1:
            self.configure(rf0, PARIS, 0x2B, 16)
            self.configure(rf1, NARROW, 0x12, 32)
            rf0.send(b"\x00" + payload)
            rf1.send(b"\x00\x11")
            self.assertEqual(rf0.command(0x17), PONG)
            self.assertEqual(rf1.command(0x17), PONG)
            trial.gate.touch()
            self.assertEqual(rf0.event(), TX_OK)
            self.assertEqual(rf1.event(), TX_OK)
            self.assertEqual(rf0.command(0x0B), b"\x06\x8b" + PARIS)
            self.assertEqual(rf1.command(0x0B), b"\x06\x8b" + NARROW)
        self.assertCountEqual(trial.tx_calls, [
            f"SET_TX hz=869300000 bw=250000 sf=9 cr=5 sync=2b preamble=16 power_dbm=-9 payload={payload.hex()}",
            "SET_TX hz=869618000 bw=62500 sf=8 cr=8 sync=12 preamble=32 power_dbm=-9 payload=11",
        ])

    def test_transmit_power_is_per_port_and_fixed_at_submission(self):
        with self.modem(hold_tx=True) as trial, trial.port(0) as rf0, trial.port(1) as rf1:
            self.assertEqual(rf0.command(0x0C), b"\x06\x8c\xf7")
            self.assertEqual(rf1.command(0x0C), b"\x06\x8c\xf7")
            self.assertEqual(rf0.command(0x0A, b"\x16"), b"\x06\xf1\x02")
            self.assertEqual(rf0.command(0x0C), b"\x06\x8c\xf7")
            self.assertEqual(rf1.command(0x0C), b"\x06\x8c\xf7")

            self.configure(rf0, PARIS, 0x2B, 16)
            self.configure(rf1, NARROW, 0x12, 32)
            self.assertEqual(rf0.command(0x0A, b"\x08"), OK)
            self.assertEqual(rf1.command(0x0A, b"\x0e"), OK)
            rf0.send(b"\x00\x22")
            rf1.send(b"\x00\x11")
            self.assertEqual(rf0.command(0x0C), b"\x06\x8c\x08")
            self.assertEqual(rf1.command(0x0C), b"\x06\x8c\x0e")
            self.assertEqual(rf0.command(0x0A, b"\xf7"), OK)
            self.assertEqual(rf0.command(0x0C), b"\x06\x8c\xf7")
            self.assertEqual(rf1.command(0x0C), b"\x06\x8c\x0e")

            trial.gate.touch()
            self.assertEqual(rf0.event(), TX_OK)
            self.assertEqual(rf1.event(), TX_OK)
            rf0.send(b"\x00\x33")
            self.assertEqual(rf0.event(), TX_OK)
        self.assertCountEqual(trial.tx_calls, [
            "SET_TX hz=869300000 bw=250000 sf=9 cr=5 sync=2b preamble=16 power_dbm=8 payload=22",
            "SET_TX hz=869618000 bw=62500 sf=8 cr=8 sync=12 preamble=32 power_dbm=14 payload=11",
            "SET_TX hz=869300000 bw=250000 sf=9 cr=5 sync=2b preamble=16 power_dbm=-9 payload=33",
        ])

    def test_receive_only_never_transmits(self):
        with self.modem(tx=False) as trial, trial.port(0) as port:
            self.configure(port, PARIS, 0x2B, 16)
            port.send(b"\x00\x22")
            self.assertEqual(port.event(), TX_FAILED)
        self.assertEqual(trial.tx_calls, [])

    def test_other_port_answers_control_queries_during_transmission(self):
        with self.modem(hold_in_flight=True) as trial, trial.port(0) as rf0, trial.port(1) as rf1:
            self.configure(rf0, PARIS, 0x2B, 16)
            self.configure(rf1, NARROW, 0x12, 32)
            rf0.send(b"\x00\x22")
            try:
                trial.wait_for_tx_start()
                self.assertEqual(rf1.command(0x0B, timeout=1), b"\x06\x8b" + NARROW)
                self.assertEqual(rf1.command(0x0E, timeout=1), b"\x06\x8e\x01")
            finally:
                trial.gate.touch()
            self.assertEqual(rf0.event(), TX_OK)
        self.assertEqual(len(trial.tx_calls), 1)

    def test_reopened_port_does_not_receive_previous_connection_tx_done(self):
        with self.modem(hold_in_flight=True) as trial, trial.port(1) as rf1:
            try:
                with trial.port(0) as sender:
                    self.configure(sender, PARIS, 0x2B, 16)
                    sender.send(b"\x00\x22")
                    trial.wait_for_tx_start()
                # A round trip through the main poll loop lets it handle RF0's hangup.
                self.assertEqual(rf1.command(0x17), PONG)
                with trial.port(0) as reopened:
                    self.assertEqual(reopened.command(0x17), PONG)
                    trial.gate.touch()
                    deadline = time.monotonic() + 3
                    status = rf1.command(0x0E, timeout=1)
                    while status == b"\x06\x8e\x01":
                        self.assertLess(time.monotonic(), deadline, "Simulated TX never finished")
                        time.sleep(0.01)
                        status = rf1.command(0x0E, timeout=1)
                    self.assertEqual(status, b"\x06\x8e\x00")
                    self.assertEqual(reopened.command(0x17), PONG)
                    self.assertFalse(reopened.events, "The original client no longer owns this PTY")
            finally:
                trial.gate.touch()
        self.assertEqual(len(trial.tx_calls), 1)

    def test_overlong_airtime_rejects_tx_without_closing_ports(self):
        slow = struct.pack("<IIBB", 869500000, 125000, 12, 8)
        with self.modem(FAKE_LONG_TX="1") as trial, trial.port(0) as rf0, trial.port(1) as rf1:
            self.configure(rf0, slow, 0x12, 8192)
            self.configure(rf1, NARROW, 0x12, 32)
            rf0.send(b"\x00\x22")
            self.assertEqual(rf0.event(), TX_FAILED)
            self.assertEqual(rf0.command(0x17), PONG)
            self.assertEqual(rf1.command(0x17), PONG)
        self.assertEqual(trial.tx_calls, [])

    def test_busy_and_ambiguous_transmits_are_not_replayed(self):
        with self.modem(hold_tx=True) as trial, trial.port(1) as port:
            self.configure(port, NARROW, 0x12, 32)
            port.send(b"\x00\x11")
            port.send(b"\x00\x22")
            self.assertEqual(port.event(), TX_BUSY)
            self.assertEqual(port.event(), TX_FAILED)
        self.assertEqual(trial.tx_calls, [])
        with self.modem(FAKE_AMBIGUOUS="1") as trial, trial.port(1) as port:
            self.configure(port, NARROW, 0x12, 32)
            port.send(b"\x00\x11")
            self.assertEqual(port.event(), TX_FAILED)
            self.assertEqual(port.command(0x17), PONG)
        self.assertEqual(len(trial.tx_calls), 1)

    def test_parser_recovers_after_corrupt_and_oversized_frames(self):
        with self.modem() as trial, trial.port(0) as port:
            port.write(FEND + b"\x06\x17\xdb\x01" + FEND)
            port.write(FEND + b"\x06\x17" + b"x" * 512 + FEND)
            self.assertEqual(port.command(0x17), PONG)
            self.assertEqual(port.command(0x11), VERSION)


if __name__ == "__main__":
    unittest.main()
