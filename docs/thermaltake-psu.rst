.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver thermaltake-psu
=============================

Supported devices:

* Thermaltake Toughpower i1650W (USB ID 264a:2329, model string ``TTI-2000DH3FCP-3``)

Other Thermaltake DPS G PSUs may use the same protocol; reports with their USB IDs
and ``usbdumps`` are welcome.

Author: awkeng

Description
-----------

This driver implements the sysfs interface for Thermaltake PSUs with a USB HID
interface. The PSU's micro-controller reports AC input voltage, the voltage and
current of the +12v, +5v and +3.3v rails, the voltage and current of 8 separate
+12v channels, total output power, one temperature and the fan speed. The fan mode
can be set. RGB lighting is not supported.

The protocol is undocumented; it was reverse engineered from the vendor's Windows
software (see ``docs/PROTOCOL.md``).

Sysfs entries
-------------

=======================	========================================================
in0_input		Voltage of the psu ac input
in1_input		Voltage of the +12v psu rail
in2_input		Voltage of the +5v psu rail
in3_input		Voltage of the +3.3v psu rail
in4_input .. in11_input	Voltage of +12v channels 1 to 8
curr1_input		Current on the +12v psu rail
curr2_input		Current on the +5v psu rail
curr3_input		Current on the +3.3v psu rail
curr4_input .. curr11_input	Current on +12v channels 1 to 8
power1_input		Total output power
temp1_input		Temperature of the psu
fan1_input		RPM of the psu fan
pwm1			Fan duty used in manual mode (0-255; the PSU accepts 30%
			to 100%, lower values are raised to 30%)
pwm1_enable		Fan mode:

			- 0: full speed (manual, 100%)
			- 1: manual, duty taken from pwm1
			- 2: automatic, silent profile
			- 3: automatic, performance profile
			- 4: zero fan (fan stops at low load/temperature)

			The PSU can't report its fan mode, so reading
			pwm1_enable fails with -ENODATA until it has been
			written once (or set by the fan_mode parameter). The
			PSU keeps the mode without the driver.
=======================	========================================================

Every ``*_input`` attribute has a matching ``*_label``.

Module parameters
-----------------

=======================	========================================================
fan_mode		Fan mode to set when the PSU is probed: ``full``,
			``silent``, ``performance``, ``zero``, or a manual duty
			in percent (``30`` to ``100``). If unset (the default)
			the driver leaves the fan as it is.
=======================	========================================================

Debugfs entries
---------------

=======================	========================================================
model			Model string reported by the psu
serial			Part/serial string reported by the psu
=======================	========================================================
