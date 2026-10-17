"""ttpsu: read Thermaltake Toughpower DPS G telemetry."""
from __future__ import annotations

import argparse
import json
import sys
import time

from .device import FAN_CUSTOM, FAN_MODES, Psu, Reading


def format_reading(r: Reading) -> str:
    lines = [
        f"AC input     {r.ac_in_v:7.1f} V",
        f"Temperature  {r.temp_c:7.1f} C",
        f"Fan          {r.fan_rpm:7.0f} RPM",
        f"Output power {r.power_out_w:7.1f} W",
        "",
        "Rail     Volts    Amps    Watts",
    ]
    for name, v, a in (("+12V", r.v12_v, r.i12_a), ("+5V", r.v5_v, r.i5_a),
                       ("+3.3V", r.v3_3_v, r.i3_3_a)):
        lines.append(f"{name:6s} {v:7.2f} {a:7.2f} {v * a:8.1f}")
    lines.append("")
    lines.append("12V channel  Volts    Amps")
    for i, (v, a) in enumerate(r.channels_12v, 1):
        lines.append(f"  {i}        {v:7.2f} {a:7.2f}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="ttpsu", description=__doc__)
    sub = ap.add_subparsers(dest="cmd")
    p_status = sub.add_parser("status", help="print one reading (default)")
    p_status.add_argument("--json", action="store_true")
    p_watch = sub.add_parser("watch", help="print readings continuously")
    p_watch.add_argument("-i", "--interval", type=float, default=1.0)
    p_watch.add_argument("--json", action="store_true", help="one JSON object per line")
    sub.add_parser("info", help="print model and serial strings")
    p_fan = sub.add_parser("fan", help="set fan mode")
    p_fan.add_argument("mode", choices=FAN_MODES)
    p_fan.add_argument("duty", nargs="?", type=int, help="duty %% for custom (30-100)")
    args = ap.parse_args(argv)

    try:
        psu = Psu()
    except OSError as e:
        print(f"ttpsu: can't open PSU ({e}). Is it plugged in, and do you have "
              "permission? (see udev/60-ttpsu.rules)", file=sys.stderr)
        return 1

    with psu:
        if args.cmd == "fan":
            mode = FAN_MODES[args.mode]
            if mode == FAN_CUSTOM and args.duty is None:
                ap.error("custom needs a duty %, e.g. 'ttpsu fan custom 50'")
            try:
                psu.set_fan(mode, args.duty or 0)
            except ValueError as e:
                ap.error(str(e))
            print(f"fan: {args.mode}" + (f" {args.duty}%" if mode == FAN_CUSTOM else ""))
        elif args.cmd == "info":
            print(f"model:  {psu.model()}")
            print(f"serial: {psu.serial()}")
        elif args.cmd == "watch":
            try:
                while True:
                    r = psu.read()
                    if args.json:
                        print(json.dumps({"time": time.time(), **r.asdict()}), flush=True)
                    else:
                        print("\033[2J\033[H" + format_reading(r), flush=True)
                    time.sleep(args.interval)
            except KeyboardInterrupt:
                pass
        else:
            r = psu.read()
            print(json.dumps(r.asdict(), indent=2) if getattr(args, "json", False)
                  else format_reading(r))
    return 0


if __name__ == "__main__":
    sys.exit(main())
