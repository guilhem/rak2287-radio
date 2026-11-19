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
usp_radio = Path(os.environ["USP_RADIO"])
flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I.", f"-I{hal}/libloragw/inc"]
with tempfile.TemporaryDirectory() as scratch:
    tmp = Path(scratch)
    radio = tmp / "radio"
    radio.touch()
    for name, main in [("api", "tests/api.c"), ("cli", "sx1250-rx.c"), ("reconfigure", "tests/reconfigure.c"),
                       ("dual-rx", "tests/dual_rx.c")]:
        subprocess.run([cc, *flags, f"-I{usp_radio}", main, "libsx1250_rx.a", "libsx1250_fake_hal.a",
                        "-o", tmp / name], cwd=here, check=True)
    subprocess.run([cc, *flags, "tests/tx.c", "libsx1250_tx.a", "libsx1250_rx.a",
                    "libsx1250_fake_hal.a", "-o", tmp / "tx"], cwd=here, check=True)
    subprocess.run([tmp / "tx", radio], check=True, timeout=5)
    subprocess.run([cc, *flags, "tests/dual_tx.c", "libsx1250_tx.a", "libsx1250_rx.a",
                    "libsx1250_fake_hal.a", "-Wl,--wrap=sx1250_dual_pause,--wrap=sx1250_dual_resume_report",
                    "-o", tmp / "dual-tx"], cwd=here, check=True)
    subprocess.run([tmp / "dual-tx", radio], check=True, timeout=5)
    subprocess.run([cc, *flags, "examples/tx_probe.c", "libsx1250_tx.a", "libsx1250_rx.a",
                    "libsx1250_fake_hal.a", "-o", tmp / "tx-probe"], cwd=here, check=True)
    tx_args = [tmp / "tx-probe", "--enable-transmit", radio, "010203", "5"]
    tx_env = {**os.environ, "FAKE_TX": "1", "IDLE": "1"}
    for bad_args in (tx_args[2:], [*tx_args[:3], "not-hex"], [*tx_args[:4], "60001"]):
        invalid = subprocess.run([tx_args[0], *bad_args], env=tx_env, capture_output=True, timeout=3)
        assert invalid.returncode == 2 and b"HAL connected" not in invalid.stdout + invalid.stderr
    success = subprocess.run(tx_args, env=tx_env, check=True, capture_output=True, timeout=3)
    assert b"HANDOFF mode=warm dry_run=0" in success.stdout
    assert success.stdout.count(b"TX_OK bytes=3 ") == 1 and b"tx_done=1 rf1_rearmed=1" in success.stdout
    assert b"RX_PROBE_DONE packets=0" in success.stdout
    deferred = subprocess.run(tx_args, env={k: v for k, v in tx_env.items() if k != "IDLE"},
                              capture_output=True, timeout=3)
    assert deferred.returncode == 3 and b"TX_DEFERRED rf1_busy=1 set_tx_called=0" in deferred.stderr
    assert b"TX_OK" not in deferred.stdout
    unconfirmed = subprocess.run(tx_args, env={**tx_env, "FAKE_TX_FAULT": "timeout"},
                                 capture_output=True, timeout=3)
    assert unconfirmed.returncode == 1 and b"TX_UNCONFIRMED" in unconfirmed.stderr
    assert b"TX_OK" not in unconfirmed.stdout and b"HAL disconnected" in unconfirmed.stdout
    cold = subprocess.run([tmp / "tx-probe", "--enable-cold-transmit", radio, "010203", "0"],
                          env=tx_env, check=True, capture_output=True, timeout=3)
    assert b"HANDOFF mode=cold dry_run=0" in cold.stdout and b"TX_OK bytes=3 " in cold.stdout
    for switch in ("--warm-dry-run", "--warm-dry-run=prepare", "--warm-dry-run=restore"):
        dry = subprocess.run([tmp / "tx-probe", switch, radio, "0"],
                             env={**tx_env, "FAKE_FORBID_TX": "1"}, check=True, capture_output=True, timeout=3)
        assert b" dry_run=1 rf1_downtime_ms=" in dry.stdout
        assert b"tx_started=0 tx_done=0 rf1_rearmed=1 cold_recovery=0 failed_stage=none" in dry.stdout
        assert b"DRY_RUN_OK set_tx_called=0 rf1_rearmed=1" in dry.stdout
    invalid = subprocess.run([tmp / "tx-probe", "--warm-dry-run=typo", radio], env=tx_env,
                             capture_output=True, timeout=3)
    assert invalid.returncode == 2 and b"HAL connected" not in invalid.stdout + invalid.stderr
    warm_args = [tmp / "tx-probe", "--enable-warm-transmit", radio, "010203", "0"]
    warm = subprocess.run(warm_args, env={**tx_env, "FAKE_SINGLE_TX": "1"},
                          check=True, capture_output=True, timeout=3)
    assert b"HANDOFF mode=warm dry_run=0 rf1_downtime_ms=" in warm.stdout
    assert b"tx_started=1 tx_done=1 rf1_rearmed=1 cold_recovery=0 failed_stage=none" in warm.stdout
    assert warm.stdout.count(b"TX_OK bytes=3 ") == 1
    failed_prepare = subprocess.run(warm_args, env={**tx_env, "FAKE_TX_FAULT": "warm_prepare", "FAKE_FORBID_TX": "1"},
                                    capture_output=True, timeout=3)
    assert failed_prepare.returncode == 1 and b"TX_NOT_STARTED" in failed_prepare.stderr
    assert b"tx_started=0 tx_done=0 rf1_rearmed=1 cold_recovery=1" in failed_prepare.stdout
    failed_restore = subprocess.run(warm_args, env={**tx_env, "FAKE_TX_FAULT": "warm_restore", "FAKE_SINGLE_TX": "1"},
                                    capture_output=True, timeout=3)
    assert failed_restore.returncode == 1 and b"TX_HANDOFF_FAILED" in failed_restore.stderr
    assert b"tx_started=1 tx_done=1 rf1_rearmed=1 cold_recovery=1" in failed_restore.stdout
    recovered = subprocess.run(tx_args, env={**tx_env, "FAKE_TX_FAULT": "warm_restore", "FAKE_SINGLE_TX": "1"},
                               check=True, capture_output=True, timeout=3)
    assert b"tx_started=1 tx_done=1 rf1_rearmed=1 cold_recovery=1 failed_stage=rf0-restore-" in recovered.stdout
    assert recovered.stdout.count(b"TX_OK bytes=3 ") == 1
    ambiguous = subprocess.run(warm_args, env={**tx_env, "FAKE_TX_FAULT": "write", "FAKE_SINGLE_TX": "1"},
                               capture_output=True, timeout=3)
    assert ambiguous.returncode == 1 and b"TX_UNCONFIRMED" in ambiguous.stderr
    assert b"tx_started=1 tx_done=0 rf1_rearmed=1 cold_recovery=1" in ambiguous.stdout
    subprocess.run([cc, *flags, "tests/activity.c", "libsx1250_tx.a", "libsx1250_rx.a",
                    "libsx1250_fake_hal.a", "-Wl,--wrap=clock_gettime", "-o", tmp / "activity"],
                   cwd=here, check=True)
    subprocess.run([tmp / "activity", radio], check=True, timeout=3)
    cxx = os.environ.get("CXX", "c++")
    subprocess.run([cxx, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I.", "tests/cpp_link.cc",
                    "libsx1250_tx.a", "libsx1250_rx.a", hal / "libloragw/libloragw.a", hal / "libtools/libtinymt32.a",
                    "-lrt", "-lm", "-o", tmp / "cpp"], cwd=here, check=True)
    subprocess.run([tmp / "cpp"], check=True, timeout=2)
    subprocess.run([tmp / "api", radio], check=True, capture_output=True, timeout=4)
    subprocess.run([tmp / "reconfigure", radio], check=True, capture_output=True, timeout=4)
    subprocess.run([tmp / "dual-rx", radio], check=True, timeout=4)
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
