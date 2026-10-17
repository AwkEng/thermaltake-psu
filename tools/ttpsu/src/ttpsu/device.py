"""Talk to the PSU over HID. See docs/PROTOCOL.md for the wire format."""
from __future__ import annotations

import sys
from dataclasses import asdict, dataclass, field

if sys.platform.startswith("linux"):
    # The default "hid" module uses libusb on Linux, which detaches the kernel
    # driver (unbinding thermaltake-psu); the hidraw backend coexists with it.
    import hidraw as hid
else:
    import hid

VID, PID = 0x264A, 0x2329
REPORT_LEN = 64

CMD_MODEL = (0xFE, 0x31)
CMD_SERIAL = (0x31, 0x3F)

# (register, field name) for single LINEAR11 values.
SENSORS = (
    (0x33, "ac_in_v"),
    (0x34, "v12_v"),
    (0x35, "v5_v"),
    (0x36, "v3_3_v"),
    (0x37, "i12_a"),
    (0x38, "i5_a"),
    (0x39, "i3_3_a"),
    (0x3A, "temp_c"),
    (0x3B, "fan_rpm"),
    (0x50, "power_out_w"),
)
REG_12V_CHANNELS = 0x51
REG_UNKNOWN_52 = 0x52


FAN_SILENT, FAN_PERFORMANCE, FAN_ZERO, FAN_CUSTOM = 1, 2, 3, 4
FAN_MODES = {"silent": FAN_SILENT, "performance": FAN_PERFORMANCE,
             "zero": FAN_ZERO, "custom": FAN_CUSTOM}
FAN_CUSTOM_MIN_PCT = 30  # lowest duty the vendor app offers


def linear11(raw: int, unsigned: bool = False) -> float:
    """Decode a PMBus LINEAR11 value (5-bit signed exponent, 11-bit mantissa).

    The fan speed register uses an unsigned mantissa (readings above 2046 RPM
    would otherwise go negative), so callers can ask for that.
    """
    exp = raw >> 11
    mant = raw & 0x7FF
    if exp > 0x0F:
        exp -= 0x20
    if mant > 0x3FF and not unsigned:
        mant -= 0x800
    return mant * 2.0**exp


@dataclass
class Reading:
    ac_in_v: float
    v12_v: float
    v5_v: float
    v3_3_v: float
    i12_a: float
    i5_a: float
    i3_3_a: float
    temp_c: float
    fan_rpm: float
    power_out_w: float
    # 12 V (volts, amps) for each of the 8 channels reported by register 0x51.
    channels_12v: list[tuple[float, float]] = field(default_factory=list)
    unknown_52: int = 0

    def asdict(self) -> dict:
        return asdict(self)


class Psu:
    def __init__(self, path: bytes | None = None):
        self._dev = hid.device()
        if path:
            self._dev.open_path(path)
        else:
            self._dev.open(VID, PID)

    def close(self) -> None:
        self._dev.close()

    def __enter__(self) -> Psu:
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def query(self, cmd: tuple[int, int]) -> bytes:
        """Send a 2-byte read command and return the matching 64-byte reply."""
        # Leading 0 = report ID (the device uses none).
        self._dev.write([0x00, *cmd] + [0x00] * (REPORT_LEN - 2))
        # Other programs (e.g. the vendor app) can read concurrently, so skip
        # replies that aren't ours.
        for _ in range(8):
            reply = bytes(self._dev.read(REPORT_LEN, 1000))
            if not reply:
                break
            if tuple(reply[:2]) == cmd:
                return reply
        raise TimeoutError(f"no reply to {bytes(cmd).hex(' ')}")

    def set_fan(self, mode: int, duty_pct: int = 0) -> None:
        """Set fan mode; duty_pct is only used by FAN_CUSTOM (30-100)."""
        if mode == FAN_CUSTOM:
            if not FAN_CUSTOM_MIN_PCT <= duty_pct <= 100:
                raise ValueError(f"custom duty must be {FAN_CUSTOM_MIN_PCT}-100%")
        elif mode in (FAN_SILENT, FAN_PERFORMANCE, FAN_ZERO):
            duty_pct = 0
        else:
            raise ValueError(f"unknown fan mode {mode}")
        self._dev.write([0x00, 0x30, 0x41, mode, duty_pct] + [0x00] * (REPORT_LEN - 4))
        for _ in range(8):
            reply = bytes(self._dev.read(REPORT_LEN, 1000))
            if not reply:
                break
            if reply[:2] == b"0A":
                if reply[2] != mode:
                    raise RuntimeError(f"PSU rejected fan mode: reply {reply[:4].hex(' ')}")
                return
        raise TimeoutError("no reply to fan mode command")

    def _u16(self, reg: int) -> int:
        r = self.query((0x31, reg))
        return r[2] | r[3] << 8

    def _string(self, cmd: tuple[int, int]) -> str:
        return self.query(cmd)[2:].split(b"\0", 1)[0].decode("ascii", "replace")

    def model(self) -> str:
        return self._string(CMD_MODEL)

    def serial(self) -> str:
        return self._string(CMD_SERIAL)

    def read(self) -> Reading:
        values = {name: linear11(self._u16(reg), unsigned=(name == "fan_rpm"))
                  for reg, name in SENSORS}
        r = self.query((0x31, REG_12V_CHANNELS))
        channels = []
        for off in range(2, 2 + 8 * 4, 4):
            v = linear11(r[off] | r[off + 1] << 8)
            a = linear11(r[off + 2] | r[off + 3] << 8)
            channels.append((v, a))
        return Reading(**values, channels_12v=channels, unknown_52=self._u16(REG_UNKNOWN_52))


def enumerate_paths() -> list[bytes]:
    return [d["path"] for d in hid.enumerate(VID, PID)]
