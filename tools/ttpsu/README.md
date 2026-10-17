# ttpsu

Userspace tool for Thermaltake Toughpower USB PSUs using hidapi. It talks to
the PSU directly over hidraw, so it works without the kernel driver (and on
Windows). It also works alongside the driver.

```sh
python -m venv .venv && . .venv/bin/activate   # Windows: .venv\Scripts\activate
pip install -e tools/ttpsu
ttpsu info               # model + serial
ttpsu status [--json]    # one reading
ttpsu watch [-i 2] [--json]
ttpsu fan silent         # or: performance, zero, custom 30..100
```

On Linux, install the udev rule so it works without root:

```sh
sudo cp tools/ttpsu/udev/60-ttpsu.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
```

On Windows, close TT RGB PLUS first; it would compete for replies and may
override fan settings.
