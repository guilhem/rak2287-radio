"""Route profile-tagged receiver records to the two receive-only observers."""

import argparse
import struct
import subprocess
import sys


def observe(source, meshcore, meshtastic, monitor):
    for line in source:
        if monitor.poll() is not None:
            raise RuntimeError(f"MeshCore observer exited with status {monitor.returncode}")
        print(line, end="", flush=True)
        fields = line.rstrip("\n").split("\t")
        if len(fields) != 6 or fields[0] != "RX":
            continue
        _, profile, timestamp, rssi, snr, payload = fields
        if profile == meshcore:
            monitor.stdin.write(f"RX\t{rssi}\t{snr}\t{payload}\n")
            monitor.stdin.flush()
        elif profile == meshtastic:
            frame = bytes.fromhex(payload)
            if len(frame) < 16:
                continue
            dest, sender, packet_id, flags, channel, next_hop, relay = struct.unpack_from("<IIIBBBB", frame)
            print(
                f"MESHTASTIC profile={profile} ts_us={timestamp} "
                f"from={sender:08x} to={dest:08x} id={packet_id:08x} "
                f"flags={flags:02x} channel={channel:02x} next_hop={next_hop:02x} "
                f"relay={relay:02x} rssi={rssi} snr={snr}",
                flush=True,
            )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--meshcore", required=True, help="profile name from the receiver config")
    parser.add_argument("--meshtastic", help="profile name from the receiver config")
    parser.add_argument("--monitor", required=True, help="path to meshcore-rx-monitor")
    args = parser.parse_args()
    if args.meshcore == args.meshtastic:
        parser.error("MeshCore and Meshtastic need distinct profile names")
    with subprocess.Popen([args.monitor], stdin=subprocess.PIPE, text=True) as monitor:
        try:
            observe(sys.stdin, args.meshcore, args.meshtastic, monitor)
        except (BrokenPipeError, RuntimeError, ValueError) as error:
            print(f"observe_profiles: {error}", file=sys.stderr)
            return 1
        finally:
            try:
                monitor.stdin.close()
            except BrokenPipeError:
                pass
        return monitor.wait()


if __name__ == "__main__":
    sys.exit(main())
