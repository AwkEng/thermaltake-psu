"""Passively read input reports from the PSU (while the vendor app polls it)
and decode them as PMBus LINEAR11 values."""
import sys
import time
from collections import defaultdict

import hid

VID, PID = 0x264A, 0x2329


def linear11(raw: int) -> float:
    exp = raw >> 11
    mant = raw & 0x7FF
    if exp > 0x0F:
        exp -= 0x20
    if mant > 0x3FF:
        mant -= 0x800
    return mant * 2.0**exp


def main(seconds: float) -> None:
    dev = hid.device()
    dev.open(VID, PID)
    seen = defaultdict(list)
    start = time.time()
    while time.time() - start < seconds:
        r = dev.read(64, 500)
        if r:
            seen[(r[0], r[1])].append(bytes(r[2:8]))
    dev.close()
    for (cls, reg), vals in sorted(seen.items()):
        last = vals[-1]
        raw = last[0] | last[1] << 8
        distinct = len(set(v[:2] for v in vals))
        print(f"{cls:02x} {reg:02x}  n={len(vals):4d} distinct={distinct:3d}  "
              f"bytes={last.hex(' ')}  u16={raw:5d}  linear11={linear11(raw):9.3f}")


if __name__ == "__main__":
    main(float(sys.argv[1]) if len(sys.argv) > 1 else 10)
