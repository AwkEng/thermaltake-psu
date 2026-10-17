# Protocol notes

## Transport

USB HID, VID `264A` PID `2329`, product string `TTI`, manufacturer `TT`.

Report descriptor (38 bytes):

```
06 00 ff 09 01 a1 01 19 01 29 40 15 00 26 ff 00 75 08 95 40 81 00
19 01 29 40 15 00 26 ff 00 75 08 95 40 91 00 c0
```

Vendor usage page `0xFF00`, usage `0x01`; one 64-byte input report and one
64-byte output report, **no report IDs**. On Linux this is a plain `hidraw` device.

## Framing

Every exchange is one 64-byte OUT report followed by one 64-byte IN report.
The host sends `<class> <cmd>` followed by padding (TT RGB PLUS sends
uninitialized buffer memory there; zeros work fine). The PSU echoes the two
command bytes and puts the payload in bytes 2+.

Numeric values are PMBus LINEAR11, little-endian (5-bit signed exponent,
11-bit signed mantissa).

## Read commands (verified by sending them ourselves)

| Request   | Reply payload                | Meaning                          |
|-----------|------------------------------|----------------------------------|
| `fe 31`   | ASCII `TTI-2000DH3FCP-3`     | model / platform string          |
| `31 3f`   | ASCII `PSTPI1650<redacted>` | part / serial string        |
| `31 33`.. | see table below              | telemetry                        |
| `31 51`   | 8 × (V LINEAR11, A LINEAR11) | 12 V per-channel V/A (8 channels)|

Startup sequence seen from TT RGB PLUS: `fe 31`, `31 3f`, then the `31 33`–`31 3b`
block every ~1.2 s; about 12 s later it adds `31 50`, `31 51`, `31 52`, sends
`30 41 00` (fan: no change) once, and starts streaming `30 42` LED frames at ~16 Hz.

## Write commands (seen, not yet sent by us)

| Request | Payload seen | Notes |
|---------|--------------|-------|
| `30 41` | `<mode> [<pct>]` | **fan mode** (see below) |
| `30 42` | `18 00` + 18 × `ff 00 00` | LED frame (RGB is out of scope for this project; not decoded further), streamed ~16 Hz. Byte 2 `0x18` might be a mode or count; colour order unconfirmed. Reply `30 42 fe` |

`fe` in the reply to `30 xx` writes looks like an ACK.

### Fan mode (`30 41 <mode> <duty>`) – confirmed by labelled capture 03

| Mode | Name (TT RGB PLUS) | Duty byte | Steady RPM at light load |
|------|--------------------|-----------|--------------------------|
| `00` | – (no-op; app sends it on start/page change, reply `fe`) | 0 | unchanged |
| `01` | Silent             | 0         | ~520                     |
| `02` | Performance        | 0         | ~1270                    |
| `03` | Zero fan           | 0         | 0                        |
| `04` | Customize          | 30–100 (%)| 30%≈915, 40%≈1270, 50%≈1500, 60%≈1760, 70%≈2110, 80%≈2370, 90%≈2600, 100%≈2880 |

Reply echoes `30 41 <mode> <duty>`. The fan takes ~8–10 s to settle after a change.

## Telemetry registers

| Bytes 0-1 | Example raw | Decoded | Meaning (tentative)      |
|-----------|-------------|---------|--------------------------|
| `31 33`   | `e6 f8`     | 115.0   | AC input voltage (V)     |
| `31 34`   | `fb d2`     | 11.92   | 12 V rail voltage        |
| `31 35`   | `45 d1`     | 5.08    | 5 V rail voltage         |
| `31 36`   | `d5 d0`     | 3.33    | 3.3 V rail voltage       |
| `31 37`   | `09 f0`     | 2.25    | 12 V rail current (A)    |
| `31 38`   | `2f e0`     | 2.94    | 5 V rail current (A)     |
| `31 39`   | `1d e0`     | 1.81    | 3.3 V rail current (A)   |
| `31 3a`   | `27 00`     | 39      | Temperature (°C)         |
| `31 3b`   | `f5 08`     | 490     | Fan speed (RPM) – **unsigned** mantissa (wraps negative above 2046 if decoded as signed) |
| `31 50`   | `17 08`     | 46      | Output power (W)         |
| `31 51`   | `f9 d2 01 f0` ×8 | 11.89 V, 0.25 A ... | 12 V per channel |
| `31 52`   | `01 00`     | 1       | ? (fan mode / status?)   |

## Still unknown

- `31 52` (always 1 so far): fan mode? single/multi-rail OCP mode?
- Whether the fan mode persists across PSU power cycles (it does persist with no app running)
- LED mode/colour format details (`30 42`) – out of scope, not planned
