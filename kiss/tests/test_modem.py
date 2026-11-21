"""Exercise the modem's Linux PTYs with a simulated driver or HAL."""

from collections import deque
from contextlib import contextmanager, nullcontext
from pathlib import Path
from select import select
import grp
import os
import stat
import struct
import subprocess
import sys
import tempfile
import time
import tty
import unittest

MODEM = Path(sys.argv.pop(1)).resolve()
os.environ.pop("NOTIFY_SOCKET", None)
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
    def __init__(self, directory, log, port_paths):
        self.directory = directory
        self.paths = tuple(path if path is not None else directory / f"rf{rf}"
                           for rf, path in enumerate(port_paths))
        self.gate = directory / "release-tx"
        self.log_file = log

    def port(self, rf):
        return Port(self.paths[rf])

    def wait_for_tx_start(self):
        deadline = time.monotonic() + 5
        while b"SET_TX " not in os.pread(self.log_file.fileno(), 8192, 0):
            if time.monotonic() >= deadline:
                raise TimeoutError("Modem never entered simulated TX")
            time.sleep(0.01)

    @property
    def tx_calls(self):
        return [line for line in self.log.splitlines() if line.startswith("SET_TX ")]


class ModemTestCase(unittest.TestCase):
    @contextmanager
    def modem(self, *, mode="receive-and-transmit", hold_tx=False, hold_in_flight=False,
              directory=None, use_default_directory=False, device="/simulated-spi",
              port_paths=(None, None), extra_args=(), **env):
        runtime = nullcontext(directory) if directory is not None else tempfile.TemporaryDirectory()
        with runtime as dirname, tempfile.TemporaryFile() as log:
            trial = Trial(Path(dirname), log, port_paths)
            if hold_tx:
                env["FAKE_TX_GATE"] = str(trial.gate)
            if hold_in_flight:
                env["FAKE_TX_IN_FLIGHT"] = str(trial.gate)
            args = [MODEM]
            if not use_default_directory:
                args.extend(("--runtime-dir", trial.directory))
            if device is not None:
                args.extend(("--spi-device", device))
            if mode is not None:
                args.extend(("--mode", mode))
            for rf, path in enumerate(port_paths):
                if path is not None:
                    args.extend((f"--rf{rf}-path", path))
            args.extend(extra_args)
            with subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT,
                                  env={**os.environ, **env}) as process:
                trial.process = process
                try:
                    deadline = time.monotonic() + 5
                    while b"SX130X_KISS_READY" not in os.pread(log.fileno(), 8192, 0):
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
                for path in trial.paths:
                    self.assertFalse(path.is_symlink())

    def configure(self, port, profile, sync, preamble):
        self.assertEqual(port.command(0x09, profile), OK)
        self.assertEqual(port.command(0x1B, bytes([sync])), OK)
        self.assertEqual(port.command(0x1C, struct.pack("<H", preamble)), OK)


class ModemTest(ModemTestCase):
    def test_default_directory_is_private_locked_and_ready_in_receive_mode(self):
        with tempfile.TemporaryDirectory() as name:
            home = Path(name)
            xdg = home / "runtime"
            xdg.mkdir(mode=0o700)
            for runtime_base, parent in ((str(xdg), xdg), ("", home / ".local/run")):
                directory = parent / "sx130x-kiss"
                settings = {"XDG_RUNTIME_DIR": runtime_base, "HOME": name}
                with self.subTest(runtime_base=runtime_base):
                    with self.modem(mode=None, directory=directory, use_default_directory=True,
                                    device=None, **settings) as trial:
                        self.assertEqual(directory.stat().st_mode & 0o777, 0o700)
                        self.assertIn(b" tx_enabled=0\n", os.pread(trial.log_file.fileno(), 8192, 0))
                        competing = subprocess.run([MODEM], env={**os.environ, **settings},
                                                   capture_output=True, timeout=5)
                        self.assertNotEqual(competing.returncode, 0)
                        self.assertIn(os.fsencode(directory), competing.stderr)
                        self.assertIn(b"Another sx130x-kiss process", competing.stderr)
                        with trial.port(0) as port:
                            self.assertEqual(port.command(0x17), PONG)

    def test_default_directory_does_not_adopt_a_public_directory_or_symlink(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name) / "sx130x-kiss"
            directory.mkdir()
            directory.chmod(0o755)
            settings = {**os.environ, "XDG_RUNTIME_DIR": name}
            result = subprocess.run([MODEM], env=settings, capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(os.fsencode(directory), result.stderr)
            self.assertIn(b"mode 0700", result.stderr)
            self.assertEqual(directory.stat().st_mode & 0o777, 0o755)
            directory.rmdir()

            target = Path(name) / "target"
            target.mkdir(mode=0o700)
            directory.symlink_to(target, target_is_directory=True)
            result = subprocess.run([MODEM], env=settings, capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(os.fsencode(directory), result.stderr)
            self.assertTrue(directory.is_symlink())
            self.assertEqual(list(target.iterdir()), [])

    def test_invalid_options_leave_directories_uncreated(self):
        with tempfile.TemporaryDirectory() as name:
            settings = {**os.environ, "XDG_RUNTIME_DIR": name}
            for arguments, diagnostic in (
                (("--runtime-dir", Path(name) / "ports", "--mode", "transmit"), b"invalid value"),
                (("--runtime-dir", "relative/ports"), b"must be an absolute path"),
                (("--rf0-path", "relative/rf0"), b"must be an absolute path"),
                (("--rf1-path", name + "/bad/"), b"ending in a filename"),
                (("--rf0-group", "__sx130x_missing_group__"), b"cannot resolve Unix group"),
            ):
                with self.subTest(arguments=arguments):
                    result = subprocess.run([MODEM, *arguments], cwd=name, env=settings,
                                            capture_output=True, timeout=5)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(diagnostic, result.stderr)
                    self.assertEqual(result.stdout, b"")
                    self.assertEqual(list(Path(name).iterdir()), [])

    def test_explicit_paths_share_a_directory_alias_with_independent_access(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            actual = root / "actual"
            directory = actual / "ports"
            directory.mkdir(parents=True)
            directory.chmod(0o755)
            alias = root / "alias"
            alias.symlink_to(actual, target_is_directory=True)
            paths = (directory / "meshcore", alias / "ports/meshtastic")
            try:
                group = grp.getgrgid(os.getegid())
            except KeyError:
                group = None
            group_args = ("--rf0-group", group.gr_name) if group is not None else ()
            with self.modem(directory=root, use_default_directory=True, port_paths=paths,
                            extra_args=group_args, HOME="", XDG_RUNTIME_DIR="") as trial:
                self.assertEqual(stat.S_IMODE(paths[0].stat().st_mode), 0o660 if group else 0o600)
                if group is not None:
                    self.assertEqual(paths[0].stat().st_gid, group.gr_gid)
                self.assertEqual(stat.S_IMODE(paths[1].stat().st_mode), 0o600)
                expected = b"SX130X_KISS_READY rf0=%s rf1=%s tx_enabled=1\n" % (
                    os.fsencode(paths[0]), os.fsencode(paths[1]),
                )
                self.assertIn(expected, os.pread(trial.log_file.fileno(), 8192, 0))
                with trial.port(0) as rf0, trial.port(1) as rf1:
                    self.assertEqual(rf0.command(0x17), PONG)
                    self.assertEqual(rf1.command(0x17), PONG)

    def test_custom_and_fallback_parent_directories_stay_locked(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            custom = root / "custom"
            custom.mkdir(mode=0o700)
            runtime = root / "runtime"
            with self.modem(directory=runtime, port_paths=(custom / "meshcore", None)) as trial:
                for parent in (custom, runtime):
                    result = subprocess.run(
                        [MODEM, "--rf0-path", parent / "other0", "--rf1-path", parent / "other1"],
                        capture_output=True, timeout=5,
                    )
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(b"Another sx130x-kiss process", result.stderr)
                with trial.port(0) as rf0, trial.port(1) as rf1:
                    self.assertEqual(rf0.command(0x17), PONG)
                    self.assertEqual(rf1.command(0x17), PONG)

    def test_duplicate_aliased_paths_and_links_to_live_ptys_are_refused(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            actual = root / "actual"
            directory = actual / "ports"
            directory.mkdir(parents=True)
            alias = root / "alias"
            alias.symlink_to(actual, target_is_directory=True)
            path = directory / "shared"
            run = [MODEM, "--rf0-path", path, "--rf1-path"]
            result = subprocess.run([*run, alias / "ports/shared"], capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"require distinct port paths", result.stderr)
            self.assertEqual(list(directory.iterdir()), [])

            master, slave = os.openpty()
            target = os.ttyname(slave)
            occupied = directory / "occupied"
            try:
                occupied.symlink_to(target)
                result = subprocess.run([*run, occupied], capture_output=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b"the port path is occupied", result.stderr)
                self.assertEqual(os.readlink(occupied), target)
                self.assertFalse(path.is_symlink())
            finally:
                os.close(slave)
                os.close(master)

    def test_failed_service_notification_cleans_up_both_port_links(self):
        with tempfile.TemporaryDirectory() as name:
            result = subprocess.run(
                [MODEM, "--runtime-dir", name], env={**os.environ, "NOTIFY_SOCKET": "relative"},
                capture_output=True, timeout=5,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"cannot notify systemd", result.stderr)
            self.assertEqual(list(Path(name).iterdir()), [])

    def test_runtime_directory_recovers_stale_ports_and_excludes_a_second_owner(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            master, slave = os.openpty()
            try:
                tty.setraw(slave)
                reused = os.ttyname(slave)
                (directory / "rf0").symlink_to(reused)
                (directory / "rf1").symlink_to("/dev/pts/2147483647")
                with self.modem(directory=directory) as trial, trial.port(0) as port:
                    self.assertNotEqual(os.readlink(directory / "rf0"), reused)
                    os.write(slave, b"x")
                    self.assertTrue(select([master], [], [], 5)[0])
                    self.assertEqual(os.read(master, 1), b"x")
                    competing = subprocess.run(
                        [MODEM, "--runtime-dir", directory, "--spi-device", "/simulated-spi"],
                        capture_output=True, timeout=5,
                    )
                    self.assertNotEqual(competing.returncode, 0)
                    self.assertEqual(port.command(0x17), PONG)
            finally:
                os.close(slave)
                os.close(master)

    def test_partial_startup_cleans_ports_and_preserves_existing_files(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            occupied = directory / "rf1"
            occupied.write_text("reserved")
            result = subprocess.run(
                [MODEM, "--runtime-dir", directory, "--spi-device", "/simulated-spi"],
                capture_output=True, timeout=5,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((directory / "rf0").is_symlink())
            self.assertEqual(occupied.read_text(), "reserved")

    def test_readiness_preserves_linux_path_bytes(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name) / "custom" / os.fsdecode(b"radio-\xff")
            with self.modem(directory=directory, device=None) as trial:
                self.assertEqual(directory.stat().st_mode & 0o777, 0o700)
                expected = (b"SX130X_KISS_READY rf0=" + os.fsencode(directory / "rf0") +
                            b" rf1=" + os.fsencode(directory / "rf1") + b" tx_enabled=1\n")
                self.assertIn(expected, os.pread(trial.log_file.fileno(), 8192, 0))

    def test_independent_phy_configuration_and_received_frames(self):
        with self.modem(FAKE_RX="1") as trial, trial.port(0) as rf0, trial.port(1) as rf1:
            self.assertEqual(rf0.command(0x17), PONG)
            self.assertEqual(rf1.command(0x11), VERSION)
            self.assertEqual(rf0.command(0x16), b"\x06\x96SX130x RF0")
            self.assertEqual(rf1.command(0x16), b"\x06\x96SX130x RF1")
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
            bad_bandwidth = struct.pack("<IIBB", 869300000, 123456, 9, 5)
            self.assertEqual(port.command(0x09, bad_bandwidth), b"\x06\xf1\x02")
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
            for power in (-10, 23):
                self.assertEqual(rf0.command(0x0A, struct.pack("b", power)), b"\x06\xf1\x02")
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

    def test_back_to_back_transmissions_at_915_and_923_mhz(self):
        profile_915 = struct.pack("<IIBB", 915300000, 125000, 10, 5)
        profile_923 = struct.pack("<IIBB", 923100000, 250000, 9, 8)
        with self.modem(hold_tx=True, FAKE_LONG_TX="1") as trial, trial.port(0) as rf0, trial.port(1) as rf1:
            # Either modeled airtime times nine exceeds the queue deadline.
            self.configure(rf0, profile_915, 0x12, 128)
            self.configure(rf1, profile_923, 0x2B, 256)
            self.assertEqual(rf0.command(0x0A, b"\x16"), OK)
            self.assertEqual(rf0.command(0x0C), b"\x06\x8c\x16")
            rf0.send(b"\x00\x3a")
            rf1.send(b"\x00\x4b")
            self.assertEqual(rf0.command(0x17), PONG)
            self.assertEqual(rf1.command(0x17), PONG)
            trial.gate.touch()
            self.assertEqual(rf0.event(), TX_OK)
            self.assertEqual(rf1.event(), TX_OK)
            self.assertEqual(rf0.command(0x0B), b"\x06\x8b" + profile_915)
            self.assertEqual(rf1.command(0x0B), b"\x06\x8b" + profile_923)
        self.assertCountEqual(trial.tx_calls, [
            "SET_TX hz=915300000 bw=125000 sf=10 cr=5 sync=12 preamble=128 power_dbm=22 payload=3a",
            "SET_TX hz=923100000 bw=250000 sf=9 cr=8 sync=2b preamble=256 power_dbm=-9 payload=4b",
        ])

    def test_receive_only_never_transmits(self):
        with self.modem(mode="receive-only") as trial, trial.port(0) as port:
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

    def test_shutdown_waits_for_active_transmission_and_discards_queue(self):
        with self.modem(hold_in_flight=True) as trial, trial.port(0) as rf0, trial.port(1) as rf1:
            try:
                self.configure(rf0, PARIS, 0x2B, 16)
                self.configure(rf1, NARROW, 0x12, 32)
                rf0.send(b"\x00\x22")
                trial.wait_for_tx_start()
                rf1.send(b"\x00\x11")
                self.assertEqual(rf1.command(0x17), PONG)
                trial.process.terminate()
                with self.assertRaises(subprocess.TimeoutExpired):
                    trial.process.wait(timeout=0.1)
            finally:
                trial.gate.touch()
            self.assertEqual(trial.process.wait(timeout=5), 0)
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

    def test_partial_frames_are_isolated_by_port(self):
        with self.modem() as trial, trial.port(0) as rf0, trial.port(1) as rf1:
            rf0.write(FEND + b"\x06\x17" + FEND + b"\x06")
            self.assertEqual(rf0.event(), PONG)
            self.assertEqual(rf1.command(0x11), VERSION)
            rf0.write(b"\x17" + FEND)
            self.assertEqual(rf0.event(), PONG)


class DriverModemTest(ModemTestCase):
    def test_profiles_airtime_and_transmission_through_production_driver(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            device = directory / "spi"
            device.touch()
            radio = dict(IDLE="1", FAKE_DUAL_RX="1", FAKE_DUAL_TX="1", FAKE_SINGLE_TX="1")
            with self.modem(directory=directory, device=device, **radio) as trial, \
                    trial.port(0) as rf0, trial.port(1) as rf1:
                self.configure(rf0, PARIS, 0x2B, 16)
                self.configure(rf1, NARROW, 0x12, 16)
                invalid = struct.pack("<IIBB", 869618000, 123456, 8, 8)
                self.assertEqual(rf1.command(0x09, invalid), b"\x06\xf1\x02")
                self.assertEqual(rf1.command(0x0B), b"\x06\x8b" + NARROW)
                self.assertEqual(rf1.command(0x0F, b"\x20"), b"\x06\x8f" + struct.pack("<I", 411))

                rf1.send(b"\x00" + b"\xc0\xdb" * 16)
                self.assertEqual(rf1.event(), TX_OK)
                self.assertEqual(rf0.command(0x0B), b"\x06\x8b" + PARIS)
                self.assertEqual(rf1.command(0x0B), b"\x06\x8b" + NARROW)
                self.assertEqual(rf0.command(0x0E), b"\x06\x8e\x00")


if __name__ == "__main__":
    unittest.main(defaultTest="ModemTest")
