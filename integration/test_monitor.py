"""Check the actual native router and duplicate table with synthetic protocol records."""
import os
from pathlib import Path
import hashlib
import subprocess
import sys
import tempfile
import unittest

BINARY = Path(sys.argv.pop(1)).resolve()



class MonitorTest(unittest.TestCase):
    def run_monitor(self, records):
        with tempfile.TemporaryDirectory() as state:
            run = subprocess.run(
                [BINARY], input=records, capture_output=True, check=True, timeout=10,
                env={**os.environ, "MESHCORE_DATA_DIR": state},
            )
        lines = run.stdout.decode().splitlines()
        stats = [line for line in lines if line.startswith("SUMMARY ")]
        self.assertEqual(len(stats), 1)
        return [line for line in lines if line.startswith("ROUTE ")], dict(
            token.split("=") for token in stats[0].split()[1:]
        )

    def test_synthetic_flood_replay(self):
        # Group frames with two-byte path hashes, no hops, and distinct dummy payloads.
        a = bytes.fromhex("1540aa0102") + bytes(range(16))
        b = bytes.fromhex("1540bb0304") + bytes(range(16))
        frames = [a, a, b, a]
        records = "".join("RX\t-61\t12\t%s\n" % f.hex() for f in frames)
        lines, stats = self.run_monitor(records.encode())
        self.assertEqual([line.split()[1] for line in lines], [
            "hash=" + hashlib.sha256(bytes([5]) + f[2:]).hexdigest()[:16] for f in frames
        ])
        self.assertEqual([line.split("duplicate=")[1] for line in lines],
                         ["0 repeat_requested=1", "1 repeat_requested=0",
                          "0 repeat_requested=1", "1 repeat_requested=0"])
        self.assertEqual(stats, dict(routed="4", flood="4", direct="0", flood_dups="2",
                                     direct_dups="0", repeat_requested="2", rejected="0",
                                     send_blocked="0", sent="0"))

    def test_malformed_records_and_resynchronization(self):
        bad = [b"TX\t-71\t2\t020012", b"RX\tinf\t2\t020012", b"RX\t-70\tnan\t020012",
               b"RX\t-70\t99\t020012", b"RX\t-70\t2\t", b"RX\t-70\t2\t0z",
               b"RX\t-70\t2\t120", b"RX\t-70\t2\t10", b"RX\t-70\t2\t150101",
               b"RX\t-70\t2\t15c0ff", b"RX\t-70\t2\t1540" + b"41" * 185,
               b"x" * 4099]
        # An ACK on a direct route, then an unterminated record at EOF.
        record = b"RX\t-77.5\t-2.25\t0e0001020304"
        lines, stats = self.run_monitor(b"\n".join([*bad, record, record, b"partial"]))
        self.assertEqual(len(lines), 2)
        self.assertIn("route=direct hops=0 rssi=-77.5 snr=-2.25", lines[0])
        self.assertEqual(stats["direct_dups"], "1")
        self.assertEqual(stats["rejected"], str(len(bad) + 1))
        self.assertEqual(stats["sent"], "0")


if __name__ == "__main__":
    unittest.main()
