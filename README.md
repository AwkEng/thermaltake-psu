# thermaltake-psu

Linux hwmon driver for Thermaltake Toughpower PSUs with a USB HID interface
(DPS G series). Thermaltake only ships Windows software, so the protocol was
reverse engineered from USB captures; see [docs/PROTOCOL.md](docs/PROTOCOL.md).

Tested with the **Toughpower i1650W** (USB `264a:2329`, model string
`TTI-2000DH3FCP-3`). Other Thermaltake USB PSUs may work if they speak the same
protocol – please open an issue with your USB ID and a dump.

The driver exposes AC input voltage, voltage/current of the +12v, +5v and +3.3v
rails, voltage/current of 8 separate +12v channels, output power, temperature
and fan speed through the standard hwmon sysfs interface (so `sensors` just
works), plus fan mode control via `pwm1`/`pwm1_enable`. See
[docs/thermaltake-psu.rst](docs/thermaltake-psu.rst) for the attribute list.

## Building and installing

Needs the headers for your running kernel (`linux-headers-$(uname -r)` on
Debian/Ubuntu).

```sh
make
sudo insmod thermaltake-psu.ko     # try it out
sensors thermaltakepsu-*
```

To keep it installed across kernel updates, use DKMS:

```sh
sudo dkms add .
sudo dkms install thermaltake-psu/0.1.0
```

The module then loads automatically whenever the PSU's USB cable is connected.
To update after pulling changes:
`sudo dkms remove thermaltake-psu/0.1.0 --all && sudo dkms add . && sudo dkms install thermaltake-psu/0.1.0`.

### Fan mode

```sh
cd /sys/class/hwmon/hwmon*/          # the one whose name is thermaltakepsu
echo 2 > pwm1_enable                 # 0 full, 1 manual, 2 silent, 3 performance, 4 zero fan
echo 1 > pwm1_enable; echo 153 > pwm1  # manual 60% (PSU minimum is 30%)
```

The PSU can't report its fan mode, so `pwm1_enable` is unreadable (and
`sensors` prints an error for it) until a mode has been set. To set one every
time the driver loads:

```sh
echo "options thermaltake-psu fan_mode=silent" | sudo tee /etc/modprobe.d/thermaltake-psu.conf
```

`fan_mode` takes `full`, `silent`, `performance`, `zero`, or a manual duty
`30`-`100`. Without it the driver never changes the fan on its own.

## Status

- [x] Telemetry (voltages, currents, power, temperature, fan RPM)
- [x] Fan mode control (silent / performance / zero / custom 30-100%)
- [x] Userspace tool (`tools/ttpsu`) – works without the driver, also on Windows
- [x] Kernel driver tested on hardware (Ubuntu 26.04, kernel 7.0.0-38, via DKMS)
- [ ] Map the 8 +12v channels to physical connectors

## Not supported: RGB lighting

RGB control is **out of scope** for this project. The vendor app drives the fan
LEDs by streaming LED frames (`30 42`, ~16 Hz); what we observed is recorded in
`docs/PROTOCOL.md` for anyone who wants to pick it up, but neither the driver
nor `ttpsu` sends LED commands. Contributions are welcome.

## Repository layout

| Path | |
|------|-|
| `thermaltake-psu.c`, `Makefile`, `dkms.conf` | the kernel driver |
| `docs/thermaltake-psu.rst` | sysfs attributes, kernel documentation style |
| `docs/PROTOCOL.md` | everything learned about the USB protocol |
| `tools/ttpsu/` | Python/hidapi userspace tool (`ttpsu status`, `watch`, `fan`) |
| `tools/capture/` | Windows capture helpers (USBPcap) and pcap decoder |
| `usbdumps/` | raw USB captures from the vendor software, with notes |

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).
