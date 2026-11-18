"""Build and run an offline RF1/RF0 handover model. No hardware backend is linked."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

DRIVER = Path(__file__).resolve().parents[1]


def build(hal, tmp):
    inc = hal / "libloragw/inc"
    src = hal / "libloragw/src"
    cc = shlex.split(os.environ.get("CC", "cc"))
    common = cc + ["-std=gnu11", "-O2", "-ffunction-sections", "-fdata-sections", "-UNDEBUG",
                   "-I", str(inc), "-I", str(hal / "libtools/inc"), "-I", str(DRIVER)]
    clocks = ["-Dclock_gettime=bench_clock_gettime", "-Dnanosleep=bench_nanosleep"]
    objects = []
    for path, opts in [
        (src / "loragw_sx1302.c", []), (src / "loragw_sx1250.c", []),
        (src / "sx1250_spi.c", ["-Dioctl=bench_ioctl"]),
        (DRIVER / "sx1250_board.c", []), (DRIVER / "sx1250_rx.c", clocks),
        (DRIVER / "sx1250_tx.c", clocks),
        (DRIVER / "sx1250_tx.c", clocks + ["-DSX1250_WARM_SIMULATION"]),
        (DRIVER / "tests/fake_hal.c", ["-DHANDOVER_BENCH"]),
        (DRIVER / "tests/handover.c", []),
    ]:
        obj = tmp / f"{len(objects)}-{path.stem}.o"
        subprocess.run(common + opts + ["-c", path, "-o", obj], check=True)
        objects.append(obj)
    exe = tmp / "handover"
    subprocess.run(cc + objects + ["-Wl,--gc-sections,--wrap=sx1250_setup,--wrap=sx1302_radio_reset",
                                    "-lm", "-o", exe], check=True)
    return exe


def run(exe, variant, wall, size, offset="none", fault=None, timeout_ms=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith(("FAKE_", "BENCH_"))}
    if timeout_ms is not None:
        env["BENCH_TIMEOUT_MS"] = str(timeout_ms)
    if fault:
        env["BENCH_FAULT"] = fault
    result = subprocess.run([exe, variant, "wall" if wall else "virtual", str(size), str(offset),
                             exe.parent / "fake-spi"], text=True, capture_output=True, env=env, timeout=15)
    if result.returncode:
        raise RuntimeError(f"{variant} size={size} offset={offset}: {result.returncode}\n"
                           + result.stdout + result.stderr)
    line = next(line for line in result.stdout.splitlines() if line.startswith("BENCH "))
    return {k: int(v) for k, v in (x.split("=") for x in line.split()[1:])}


def bench(exe, wall):
    print("clock=" + ("host CLOCK_MONOTONIC, actual sleeps" if wall else "virtual, deterministic sleeps")
          + "; SPI register effects and RF propagation simulated; durations in microseconds")
    print("mode bytes entry:standby standby:set_tx set_tx:done done:set_rx set_rx:return blind call transfers resets nominal_air")
    for size in ([32] if wall else [16, 32, 128, 255]):
        results = {}
        for variant in ("cold", "warm"):
            r = results[variant] = run(exe, variant, wall, size)
            stages = [r[b] - r[a] for a, b in (("standby","set_tx"), ("set_tx","done"),
                                                ("done","set_rx"), ("set_rx","returned"))]
            print(variant, size, r["standby"], *stages, r["set_rx"] - r["standby"],
                  r["returned"], r["transfers"], r["resets"], r["air"])
        if wall:
            continue
        assert results["cold"]["set_rx"] - results["cold"]["done"] > 520000
        assert results["warm"]["set_rx"] - results["warm"]["done"] < 520000
        if size == 32:
            print("Paired traffic schedule: independent trials at identical start offsets from API entry (us)")
            schedule = range(0, 2000001, 250000)
            received = {"cold": 0, "warm": 0}
            for offset in schedule:
                delivered = {}
                for variant, marks in results.items():
                    record = run(exe, variant, False, size, offset)
                    delivered[variant] = record["rx"]
                    assert record["rx"] == int(offset > marks["set_rx"])
                    received[variant] += record["rx"]
                print(f"  {offset:7d}: cold={delivered['cold']} warm={delivered['warm']}")
            print("Paired delivery:", *(f"{k}={v}/{len(schedule)}" for k,v in received.items()))
            for variant, r in results.items():
                # Each independent transaction receives one complete 10-byte probe frame.
                offsets = [-1000, r["set_tx"] + 2000, r["done"] - 2000]
                offsets += [r["done"] + ms * 1000 for ms in (1, 25, 75, 100, 250, 500, 600, 1000)]
                offsets += [r["set_rx"] - 1, r["set_rx"] + 1, r["returned"] + 1]
                print(variant, "probe start relative to first TxDone read (us) => delivered")
                for offset in offsets:
                    event = run(exe, variant, False, size, offset)
                    expected = int(offset > r["set_rx"])
                    if event["rx"] != expected:
                        raise AssertionError((variant, offset, event, expected))
                    print(f"  {offset - r['done']:+d} => {event['rx']}")
    if not wall:
        run(exe,"skip",False,32)
        run(exe,"warm",False,32,fault="warm_entry")
        run(exe,"warm",False,32,fault="warm_restore")
        run(exe,"warm",False,32,fault="tx_timeout")
        run(exe,"warm",False,32,fault="slow_restore")
        for variant in ("cold", "warm"):
            # Polls occur at 1, 3, 5 ... ms after the command. The host deadline
            # starts one SPI transfer (1 ms) before the radio starts its timer.
            assert run(exe, variant, False, 32, timeout_ms=9)["timeout_irq"] == 9000
            assert run(exe, variant, False, 32, timeout_ms=10)["timeout_irq"] == 0
            late = run(exe, variant, False, 32, timeout_ms=411)
            assert late["done"] - late["set_tx"] == 411000 and late["timeout_irq"] == 0
        run(exe,"warm-reuse",False,32)
        print("Controls passed: removing reset alone blocks TX; cold fallback before and after TxDone;"
              " two warm sends/one capture with RF1 reception restored.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hal", type=Path, required=True)
    parser.add_argument("--wall-clock", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="handover-") as tmp:
        temp = Path(tmp)
        (temp / "fake-spi").touch()
        bench(build(args.hal.resolve(), temp), args.wall_clock)
