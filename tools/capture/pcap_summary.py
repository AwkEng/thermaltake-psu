"""Summarize PSU HID traffic in a USBPcap capture (needs Wireshark's tshark).

usage: python tools/capture/pcap_summary.py usbdumps/01-app-startup.pcap [--seq N]
"""
import argparse
import subprocess
from collections import OrderedDict

TSHARK = r"C:\Program Files\Wireshark\tshark.exe"


def packets(path):
    out = subprocess.run(
        [TSHARK, "-r", path, "-Y", "usb.transfer_type == 1 && usb.data_len == 64",
         "-T", "fields", "-e", "frame.time_relative", "-e", "usb.endpoint_address",
         "-e", "usb.capdata"],
        capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        t, ep, data = line.split("\t")
        yield float(t), "OUT" if int(ep, 16) & 0x80 == 0 else "IN", bytes.fromhex(data)


def printable(b):
    s = b.rstrip(b"\0")
    return s.decode("ascii") if s and all(32 <= c < 127 for c in s) else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("--seq", type=int, default=0, help="also print first N packets")
    args = ap.parse_args()
    pkts = list(packets(args.pcap))

    cmds = OrderedDict()
    for t, d, data in pkts:
        if d == "OUT":
            c = cmds.setdefault(data[:2], {"n": 0, "first": t, "bodies": set(), "replies": set()})
            c["n"] += 1
            c["bodies"].add(data[2:8])
            last = data[:2]
        elif cmds and data[:2] == last:
            cmds[last]["replies"].add(data[2:])

    span = pkts[-1][0] - pkts[0][0] if pkts else 0
    print(f"{len(pkts)} packets over {span:.1f}s\n")
    print("cmd    count  first(s)  out-bytes2-7 variants   reply")
    for k, c in cmds.items():
        bodies = ",".join(sorted(b.hex() for b in c["bodies"]))[:40]
        reps = sorted(c["replies"])
        text = printable(reps[0]) if reps else None
        rep = repr(text) if text else (reps[0][:8].hex(" ") if reps else "-")
        print(f"{k.hex(' ')}  {c['n']:5d}  {c['first']:8.3f}  {bodies:40s} "
              f"{rep}  ({len(reps)} distinct)")

    for t, d, data in pkts[:args.seq]:
        print(f"{t:8.3f} {d:3s} {data[:16].hex(' ')}")


if __name__ == "__main__":
    main()
