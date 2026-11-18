"""Exercise the production MeshCore radio target using simulated SX1250 hardware."""
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest

BINARY = Path(sys.argv.pop(1)).resolve()


class RepeaterTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.state = Path(tmp.name)
        self.device = self.state / "fake-spi"
        self.device.touch()
        self.env = {**os.environ, "MESHCORE_DATA_DIR": tmp.name, "IDLE": "1", "FAKE_TX": "1"}
        self.args = [BINARY, "--enable-transmit", "--device", self.device]

    def invoke(self, args=None, **env):
        return subprocess.run(
            self.args if args is None else args, env={**self.env, **env},
            text=True, capture_output=True, timeout=12,
        )

    def test_relay_then_receive_and_suppress_duplicate(self):
        keys = []
        for _ in range(2):
            result = self.invoke()
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            public = re.search(r"Repeater public key: ([0-9a-f]{64})", result.stdout)[1]
            self.assertIn("Post-send silence factor: 9\n", result.stdout)
            keys.append(public)
            frames = re.findall(r"FAKE RF0 TxDone path=(\w+) payload=(\w+)", result.stdout)
            self.assertEqual(frames, [(public[:4], "aa01020304050607"),
                                      (public[:4], "bb0908070605040302")])
            self.assertIn("received=3 flood=3 direct=0 flood_dups=1 direct_dups=0 "
                          "rejected=0 sent=2 flood_tx=2 direct_tx=0", result.stdout)
        self.assertEqual(keys[0], keys[1])
        key = self.state / "repeater.id"
        self.assertEqual(key.stat().st_mode & 0o777, 0o600)
        self.assertEqual(key.stat().st_size, 96)
        self.assertEqual(key.read_bytes()[:32].hex(), keys[0])

    def test_persisted_silence_factor(self):
        # The first field of the pinned native com_prefs file is a host float.
        prefs = self.state / "com_prefs"
        for value, expected in ((1.0, 9), (float("nan"), 9), (20.0, 20)):
            with self.subTest(value=value):
                prefs.write_bytes(struct.pack("=f", value))
                result = self.invoke()
                self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                self.assertIn(f"Post-send silence factor: {expected}\n", result.stdout)

    def test_receive_pending_when_send_starts(self):
        result = self.invoke(INJECT_BEFORE_TX="1")
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertIn("received=4 flood=4 direct=0 flood_dups=2 direct_dups=0 "
                      "rejected=0 sent=2 flood_tx=2 direct_tx=0 busy_drops=0", result.stdout)

    def test_ambiguous_transmit_failure_stops_and_releases_board(self):
        for fault in ("conflict", "rearm"):
            with self.subTest(fault=fault):
                result = self.invoke(FAKE_TX_FAULT=fault)
                self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
                self.assertIn("sx1250_tx_send", result.stderr)
                self.assertIn("HAL disconnected", result.stdout)
                self.assertIn("sent=0 flood_tx=0", result.stdout)

    def test_opt_in_required_and_corrupt_identity_preserved(self):
        result = self.invoke([BINARY, "--device", self.device])
        self.assertEqual(result.returncode, 2)
        self.assertNotIn("HAL connected", result.stdout)
        key = self.state / "repeater.id"
        key.write_bytes(b"bad")
        key.chmod(0o600)
        result = self.invoke()
        self.assertEqual(result.returncode, 1)
        self.assertNotIn("HAL connected", result.stdout)
        self.assertEqual(key.read_bytes(), b"bad")


if __name__ == "__main__":
    unittest.main()
