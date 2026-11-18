"""Run C API and CLI protocol checks against a simulated packet engine."""
import os
from pathlib import Path
import select
import signal
import subprocess
import tempfile

here = Path(__file__).resolve().parent
cc = os.environ.get("CC", "cc")
hal = Path(os.environ["HAL"])
flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I.", f"-I{hal}/libloragw/inc"]
with tempfile.TemporaryDirectory() as scratch:
    tmp = Path(scratch)
    radio = tmp / "radio"
    radio.touch()
    for name, main in [("api", "tests/api.c"), ("cli", "sx1250-rx.c")]:
        subprocess.run([cc, *flags, main, "sx1250_rx.c", "sx1250_board.c",
                        "tests/fake_hal.c", "-o", tmp / name], cwd=here, check=True)
    subprocess.run([cc, *flags, "tests/tx.c", "libsx1250_tx.a", "libsx1250_rx.a",
                    "libsx1250_fake_hal.a", "-o", tmp / "tx"], cwd=here, check=True)
    subprocess.run([tmp / "tx", radio], check=True, timeout=5)
    cxx = os.environ.get("CXX", "c++")
    subprocess.run([cxx, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I.", "tests/cpp_link.cc",
                    "libsx1250_tx.a", "libsx1250_rx.a", hal / "libloragw/libloragw.a", hal / "libtools/libtinymt32.a",
                    "-lrt", "-lm", "-o", tmp / "cpp"], cwd=here, check=True)
    subprocess.run([tmp / "cpp"], check=True, timeout=2)
    subprocess.run([tmp / "api", radio], check=True, capture_output=True, timeout=4)
    cmd = [tmp / "cli", "--device", radio, "--frequency", "869618000", "--bandwidth", "62500",
           "--sf", "8", "--cr", "8", "--sync", "0x12"]
    expected = b"RX\t-80.5\t-1.75\t" + bytes((230+i) % 256 for i in range(255)).hex().encode() + b"\n"
    for idle, close_pipe in [(False, False), (False, True), (True, True)]:
        with subprocess.Popen(cmd, env={**os.environ, **({"IDLE": "1"} if idle else {})},
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE) as p:
            try:
                assert select.select([p.stderr], [], [], 2)[0]
                assert p.stderr.readline() == b"HAL connected\n"
                if not idle:
                    assert select.select([p.stdout], [], [], 2)[0]
                    assert p.stdout.readline() == expected
                if idle:
                    contender = subprocess.run(cmd, capture_output=True, timeout=2)
                    assert contender.returncode != 0 and not contender.stdout
                    assert b"HAL connected" not in contender.stderr
                if close_pipe:
                    p.stdout.close()
                else:
                    p.send_signal(signal.SIGTERM)
                assert p.wait(timeout=2) == 0
                if not close_pipe:
                    assert p.stdout.read() == b""
                assert b"HAL disconnected" in p.stderr.read()
            finally:
                if p.poll() is None:
                    p.kill()
print("LoRa RX/TX API, RF0 faults and RF1 recovery, raw TSV and shutdown tests passed.")
