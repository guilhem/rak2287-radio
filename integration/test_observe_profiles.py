import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    mesh = "1540aabbccddeeffaabbccddeeff"
    meshtastic = struct.pack("<IIIBBBB", 0xffffffff, 0x12345678, 0x456789ab, 3, 8, 0, 1).hex()
    # Include an unknown profile and a short Meshtastic frame.
    lines = (
        f"RX\tother\t1\t-60\t12\t{mesh}\n"
        f"RX\tmeshcore\t2\t-60\t12\t{mesh}\n"
        "RX\tmeshtastic\t3\t-70\t4\t42\n"
        f"RX\tmeshtastic\t4\t-70\t4\t{meshtastic}\n"
        f"RX\tmeshcore\t5\t-60\t12\t{mesh}\n"
    )
    with tempfile.TemporaryDirectory() as state:
        result = subprocess.run(
            [sys.executable, str(Path(__file__).with_name("observe_profiles.py")),
             "--meshcore", "meshcore", "--meshtastic", "meshtastic", "--monitor", sys.argv[1]],
            input=lines, text=True, capture_output=True,
            env={**os.environ, "MESHCORE_DATA_DIR": state},
        )
    assert result.returncode == 0, result.stderr
    assert result.stdout.count("\nRX\t") == 4, result.stdout
    routes = [line for line in result.stdout.splitlines() if line.startswith("ROUTE ")]
    assert len(routes) == 2, result.stdout
    assert "duplicate=0 repeat_requested=1" in routes[0], routes
    assert "duplicate=1 repeat_requested=0" in routes[1], routes
    records = [line for line in result.stdout.splitlines() if line.startswith("MESHTASTIC ")]
    assert len(records) == 1, result.stdout
    assert "ts_us=4 from=12345678 to=ffffffff id=456789ab" in records[0], records
    assert "sent=0" in result.stdout, result.stdout

    profile = "meshcore 869618000 62500 8 8 0x12 16 1 0 0 20 5 2\n"
    with tempfile.TemporaryDirectory() as state:
        config = Path(state, "radio.conf")
        config.write_text(profile + "other 869300000 63000 5 5 0x2b 8 1 0 0 20 5 2\n")
        invalid = subprocess.run(
            [sys.argv[2], str(config), str(Path(state, "no-device"))],
            text=True, capture_output=True,
        )
        assert invalid.returncode == 2, invalid
        assert "radio.conf:2:" in invalid.stderr, invalid.stderr


if __name__ == "__main__":
    main()
