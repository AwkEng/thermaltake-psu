// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * thermaltake-psu.c - hwmon driver for Thermaltake Toughpower PSUs with a USB HID interface
 * Copyright (C) 2026 awkeng
 *
 * Protocol (reverse engineered, see docs/PROTOCOL.md):
 *
 *	- one 64 byte output report per request, one 64 byte input report per reply, no report IDs
 *	- request: [class][command][param0][param1][zero padding]
 *	- reply:   [class][command][data...] - the first two bytes echo the request
 *	- class 0x31 reads a sensor or string, class 0x30 writes a setting, 0xfe 0x31 reads the model
 *	- sensor values are PMBus LINEAR11, little endian (5 bit signed exponent, 11 bit mantissa);
 *	  the mantissa is signed except for the fan speed, which exceeds 2046 rpm at high duty
 *	- command 0x51 returns voltage/current pairs for 8 separate 12v channels
 *	- the fan mode can be written but not read back, so the driver caches the last value written
 */

#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif

#define DRIVER_NAME		"thermaltake-psu"

#define CMD_BUFFER_SIZE		64
#define REPLY_SIZE		64
#define CMD_TIMEOUT_MS		250
#define CHANNELS_12V		8

#define CLASS_WRITE		0x30
#define CLASS_READ		0x31
#define CLASS_INFO		0xfe

#define CMD_SERIAL		0x3f /* class 0x31 */
#define CMD_MODEL		0x31 /* class 0xfe */
#define CMD_IN_VOLTS		0x33
#define CMD_12V_VOLTS		0x34
#define CMD_5V_VOLTS		0x35
#define CMD_3V3_VOLTS		0x36
#define CMD_12V_AMPS		0x37
#define CMD_5V_AMPS		0x38
#define CMD_3V3_AMPS		0x39
#define CMD_TEMP		0x3a
#define CMD_FAN_RPM		0x3b
#define CMD_OUT_WATTS		0x50
#define CMD_12V_CHANNELS	0x51
#define CMD_FAN_MODE		0x41 /* class 0x30 */

#define FAN_MODE_SILENT		0x01
#define FAN_MODE_PERFORMANCE	0x02
#define FAN_MODE_ZERO		0x03
#define FAN_MODE_CUSTOM		0x04
#define FAN_DUTY_MIN		30 /* percent, lowest the vendor software offers */
#define FAN_DUTY_MAX		100

/* hwmon pwm1_enable values */
#define PWM_ENABLE_FULL		0
#define PWM_ENABLE_MANUAL	1
#define PWM_ENABLE_SILENT	2
#define PWM_ENABLE_PERFORMANCE	3
#define PWM_ENABLE_ZERO		4

struct ttpsu_data {
	struct hid_device *hdev;
	struct device *hwmon_dev;
	struct dentry *debugfs;
	struct completion wait_completion;
	struct mutex lock; /* serializes requests and protects the fan cache */
	u8 *cmd_buffer;
	u8 reply[REPLY_SIZE];
	u8 expect[2];
	char model[REPLY_SIZE - 1];
	char serial[REPLY_SIZE - 1];
	long pwm_enable; /* -1 until the driver has set a mode */
	long pwm; /* 0-255, applied in manual mode */
};

static const char *const in_labels[] = {
	"v_in",
	"v_out +12v",
	"v_out +5v",
	"v_out +3.3v",
	"v_out +12v ch1",
	"v_out +12v ch2",
	"v_out +12v ch3",
	"v_out +12v ch4",
	"v_out +12v ch5",
	"v_out +12v ch6",
	"v_out +12v ch7",
	"v_out +12v ch8",
};

static const char *const curr_labels[] = {
	"curr +12v",
	"curr +5v",
	"curr +3.3v",
	"curr +12v ch1",
	"curr +12v ch2",
	"curr +12v ch3",
	"curr +12v ch4",
	"curr +12v ch5",
	"curr +12v ch6",
	"curr +12v ch7",
	"curr +12v ch8",
};

static const u8 rail_volts_cmds[] = { CMD_12V_VOLTS, CMD_5V_VOLTS, CMD_3V3_VOLTS };
static const u8 rail_amps_cmds[] = { CMD_12V_AMPS, CMD_5V_AMPS, CMD_3V3_AMPS };

/* Convert a LINEAR11 value to an integer scaled by @scale (e.g. 1000 for milli units). */
static long ttpsu_linear11(u16 raw, long scale, bool unsigned_mantissa)
{
	int exp = sign_extend32(raw >> 11, 4);
	long mant = unsigned_mantissa ? (raw & 0x7ff) : sign_extend32(raw & 0x7ff, 10);
	long val = mant * scale;

	return exp >= 0 ? val * (1L << exp) : val / (1L << -exp);
}

/* Send one request and wait for the matching reply. Caller holds priv->lock. */
static int ttpsu_cmd(struct ttpsu_data *priv, u8 class, u8 cmd, u8 p0, u8 p1)
{
	unsigned long time;
	int ret;

	memset(priv->cmd_buffer, 0, CMD_BUFFER_SIZE);
	priv->cmd_buffer[0] = class;
	priv->cmd_buffer[1] = cmd;
	priv->cmd_buffer[2] = p0;
	priv->cmd_buffer[3] = p1;

	priv->expect[0] = class;
	priv->expect[1] = cmd;
	reinit_completion(&priv->wait_completion);

	ret = hid_hw_output_report(priv->hdev, priv->cmd_buffer, CMD_BUFFER_SIZE);
	if (ret < 0)
		return ret;

	time = wait_for_completion_timeout(&priv->wait_completion,
					   msecs_to_jiffies(CMD_TIMEOUT_MS));
	if (!time)
		return -ETIMEDOUT;

	return 0;
}

static int ttpsu_read_u16(struct ttpsu_data *priv, u8 cmd, u16 *raw)
{
	int ret;

	mutex_lock(&priv->lock);
	ret = ttpsu_cmd(priv, CLASS_READ, cmd, 0, 0);
	if (!ret)
		*raw = get_unaligned_le16(&priv->reply[2]);
	mutex_unlock(&priv->lock);

	return ret;
}

/* Read one value of the 12v channel block; @amps selects current instead of voltage. */
static int ttpsu_read_channel(struct ttpsu_data *priv, int channel, bool amps, u16 *raw)
{
	int ret;

	mutex_lock(&priv->lock);
	ret = ttpsu_cmd(priv, CLASS_READ, CMD_12V_CHANNELS, 0, 0);
	if (!ret)
		*raw = get_unaligned_le16(&priv->reply[2 + channel * 4 + (amps ? 2 : 0)]);
	mutex_unlock(&priv->lock);

	return ret;
}

static int ttpsu_read_string(struct ttpsu_data *priv, u8 class, u8 cmd, char *buf, size_t len)
{
	int ret;

	mutex_lock(&priv->lock);
	ret = ttpsu_cmd(priv, class, cmd, 0, 0);
	if (!ret)
		strscpy(buf, (char *)&priv->reply[2], min_t(size_t, len, REPLY_SIZE - 2));
	mutex_unlock(&priv->lock);

	return ret;
}

/* Caller holds priv->lock. */
static int ttpsu_set_fan(struct ttpsu_data *priv, u8 mode, u8 duty)
{
	int ret;

	ret = ttpsu_cmd(priv, CLASS_WRITE, CMD_FAN_MODE, mode, duty);
	if (ret)
		return ret;

	/* the PSU echoes the accepted mode */
	if (priv->reply[2] != mode)
		return -EIO;

	return 0;
}

static u8 ttpsu_pwm_to_duty(long pwm)
{
	return clamp_val(DIV_ROUND_CLOSEST(pwm * 100, 255), FAN_DUTY_MIN, FAN_DUTY_MAX);
}

/* Caller holds priv->lock. */
static int ttpsu_apply_pwm_enable(struct ttpsu_data *priv, long enable)
{
	switch (enable) {
	case PWM_ENABLE_FULL:
		return ttpsu_set_fan(priv, FAN_MODE_CUSTOM, FAN_DUTY_MAX);
	case PWM_ENABLE_MANUAL:
		return ttpsu_set_fan(priv, FAN_MODE_CUSTOM, ttpsu_pwm_to_duty(priv->pwm));
	case PWM_ENABLE_SILENT:
		return ttpsu_set_fan(priv, FAN_MODE_SILENT, 0);
	case PWM_ENABLE_PERFORMANCE:
		return ttpsu_set_fan(priv, FAN_MODE_PERFORMANCE, 0);
	case PWM_ENABLE_ZERO:
		return ttpsu_set_fan(priv, FAN_MODE_ZERO, 0);
	default:
		return -EINVAL;
	}
}

static umode_t ttpsu_hwmon_ops_is_visible(const void *data, enum hwmon_sensor_types type,
					  u32 attr, int channel)
{
	if (type == hwmon_pwm)
		return 0644;

	return 0444;
}

static int ttpsu_hwmon_in_read(struct ttpsu_data *priv, int channel, long *val)
{
	u16 raw;
	int ret;

	if (channel == 0)
		ret = ttpsu_read_u16(priv, CMD_IN_VOLTS, &raw);
	else if (channel <= ARRAY_SIZE(rail_volts_cmds))
		ret = ttpsu_read_u16(priv, rail_volts_cmds[channel - 1], &raw);
	else
		ret = ttpsu_read_channel(priv, channel - 4, false, &raw);

	if (!ret)
		*val = ttpsu_linear11(raw, 1000, false);

	return ret;
}

static int ttpsu_hwmon_curr_read(struct ttpsu_data *priv, int channel, long *val)
{
	u16 raw;
	int ret;

	if (channel < ARRAY_SIZE(rail_amps_cmds))
		ret = ttpsu_read_u16(priv, rail_amps_cmds[channel], &raw);
	else
		ret = ttpsu_read_channel(priv, channel - 3, true, &raw);

	if (!ret)
		*val = ttpsu_linear11(raw, 1000, false);

	return ret;
}

static int ttpsu_hwmon_ops_read(struct device *dev, enum hwmon_sensor_types type, u32 attr,
				int channel, long *val)
{
	struct ttpsu_data *priv = dev_get_drvdata(dev);
	u16 raw;
	int ret;

	switch (type) {
	case hwmon_temp:
		if (attr != hwmon_temp_input)
			return -EOPNOTSUPP;
		ret = ttpsu_read_u16(priv, CMD_TEMP, &raw);
		if (!ret)
			*val = ttpsu_linear11(raw, 1000, false);
		return ret;
	case hwmon_fan:
		if (attr != hwmon_fan_input)
			return -EOPNOTSUPP;
		ret = ttpsu_read_u16(priv, CMD_FAN_RPM, &raw);
		if (!ret)
			*val = ttpsu_linear11(raw, 1, true);
		return ret;
	case hwmon_power:
		if (attr != hwmon_power_input)
			return -EOPNOTSUPP;
		ret = ttpsu_read_u16(priv, CMD_OUT_WATTS, &raw);
		if (!ret)
			*val = ttpsu_linear11(raw, 1000000, false);
		return ret;
	case hwmon_in:
		if (attr != hwmon_in_input)
			return -EOPNOTSUPP;
		return ttpsu_hwmon_in_read(priv, channel, val);
	case hwmon_curr:
		if (attr != hwmon_curr_input)
			return -EOPNOTSUPP;
		return ttpsu_hwmon_curr_read(priv, channel, val);
	case hwmon_pwm:
		mutex_lock(&priv->lock);
		ret = 0;
		if (attr == hwmon_pwm_input)
			*val = priv->pwm;
		else if (attr == hwmon_pwm_enable && priv->pwm_enable >= 0)
			*val = priv->pwm_enable;
		else
			ret = -ENODATA; /* mode can't be read from the PSU and hasn't been set */
		mutex_unlock(&priv->lock);
		return ret;
	default:
		return -EOPNOTSUPP;
	}
}

static int ttpsu_hwmon_ops_read_string(struct device *dev, enum hwmon_sensor_types type,
				       u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		*str = "temp";
		return 0;
	case hwmon_fan:
		*str = "psu fan";
		return 0;
	case hwmon_power:
		*str = "power total";
		return 0;
	case hwmon_in:
		*str = in_labels[channel];
		return 0;
	case hwmon_curr:
		*str = curr_labels[channel];
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int ttpsu_hwmon_ops_write(struct device *dev, enum hwmon_sensor_types type, u32 attr,
				 int channel, long val)
{
	struct ttpsu_data *priv = dev_get_drvdata(dev);
	int ret = 0;

	if (type != hwmon_pwm)
		return -EOPNOTSUPP;

	mutex_lock(&priv->lock);
	switch (attr) {
	case hwmon_pwm_input:
		if (val < 0 || val > 255) {
			ret = -EINVAL;
			break;
		}
		priv->pwm = val;
		if (priv->pwm_enable == PWM_ENABLE_MANUAL)
			ret = ttpsu_apply_pwm_enable(priv, PWM_ENABLE_MANUAL);
		break;
	case hwmon_pwm_enable:
		ret = ttpsu_apply_pwm_enable(priv, val);
		if (!ret)
			priv->pwm_enable = val;
		break;
	default:
		ret = -EOPNOTSUPP;
	}
	mutex_unlock(&priv->lock);

	return ret;
}

static const struct hwmon_ops ttpsu_hwmon_ops = {
	.is_visible	= ttpsu_hwmon_ops_is_visible,
	.read		= ttpsu_hwmon_ops_read,
	.read_string	= ttpsu_hwmon_ops_read_string,
	.write		= ttpsu_hwmon_ops_write,
};

static const struct hwmon_channel_info *const ttpsu_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_LABEL),
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL),
	NULL
};

static const struct hwmon_chip_info ttpsu_chip_info = {
	.ops	= &ttpsu_hwmon_ops,
	.info	= ttpsu_info,
};

#ifdef CONFIG_DEBUG_FS

static int model_show(struct seq_file *seqf, void *unused)
{
	struct ttpsu_data *priv = seqf->private;

	seq_printf(seqf, "%s\n", priv->model);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(model);

static int serial_show(struct seq_file *seqf, void *unused)
{
	struct ttpsu_data *priv = seqf->private;

	seq_printf(seqf, "%s\n", priv->serial);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(serial);

static void ttpsu_debugfs_init(struct ttpsu_data *priv)
{
	char name[32];

	scnprintf(name, sizeof(name), "%s-%s", DRIVER_NAME, dev_name(&priv->hdev->dev));

	priv->debugfs = debugfs_create_dir(name, NULL);
	debugfs_create_file("model", 0444, priv->debugfs, priv, &model_fops);
	debugfs_create_file("serial", 0444, priv->debugfs, priv, &serial_fops);
}

#else

static void ttpsu_debugfs_init(struct ttpsu_data *priv)
{
}

#endif

static int ttpsu_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct ttpsu_data *priv;
	int ret;

	priv = devm_kzalloc(&hdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->cmd_buffer = devm_kmalloc(&hdev->dev, CMD_BUFFER_SIZE, GFP_KERNEL);
	if (!priv->cmd_buffer)
		return -ENOMEM;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	ret = hid_hw_open(hdev);
	if (ret)
		goto fail_and_stop;

	priv->hdev = hdev;
	priv->pwm_enable = -1;
	priv->pwm = 255;
	hid_set_drvdata(hdev, priv);
	mutex_init(&priv->lock);
	init_completion(&priv->wait_completion);

	hid_device_io_start(hdev);

	ret = ttpsu_read_string(priv, CLASS_INFO, CMD_MODEL, priv->model, sizeof(priv->model));
	if (ret < 0) {
		dev_err(&hdev->dev, "no reply to model request (%d)\n", ret);
		goto fail_and_close;
	}

	ret = ttpsu_read_string(priv, CLASS_READ, CMD_SERIAL, priv->serial, sizeof(priv->serial));
	if (ret < 0)
		dev_warn(&hdev->dev, "no reply to serial request (%d)\n", ret);

	priv->hwmon_dev = hwmon_device_register_with_info(&hdev->dev, "thermaltakepsu", priv,
							  &ttpsu_chip_info, NULL);
	if (IS_ERR(priv->hwmon_dev)) {
		ret = PTR_ERR(priv->hwmon_dev);
		goto fail_and_close;
	}

	ttpsu_debugfs_init(priv);
	hid_info(hdev, "%s (%s)\n", priv->model, priv->serial);

	return 0;

fail_and_close:
	hid_device_io_stop(hdev);
	hid_hw_close(hdev);
fail_and_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void ttpsu_remove(struct hid_device *hdev)
{
	struct ttpsu_data *priv = hid_get_drvdata(hdev);

	debugfs_remove_recursive(priv->debugfs);
	hwmon_device_unregister(priv->hwmon_dev);
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static int ttpsu_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data,
			   int size)
{
	struct ttpsu_data *priv = hid_get_drvdata(hdev);

	if (!priv || size < 4 || completion_done(&priv->wait_completion))
		return 0;

	/* userspace (hidraw) may be talking to the PSU too, so only take our reply */
	if (data[0] == priv->expect[0] && data[1] == priv->expect[1]) {
		memcpy(priv->reply, data, min(size, REPLY_SIZE));
		complete(&priv->wait_completion);
	}

	return 0;
}

static const struct hid_device_id ttpsu_idtable[] = {
	{ HID_USB_DEVICE(0x264a, 0x2329) }, /* Toughpower i1650W (TTI-2000DH3FCP-3) */
	{ },
};
MODULE_DEVICE_TABLE(hid, ttpsu_idtable);

static struct hid_driver ttpsu_driver = {
	.name		= DRIVER_NAME,
	.id_table	= ttpsu_idtable,
	.probe		= ttpsu_probe,
	.remove		= ttpsu_remove,
	.raw_event	= ttpsu_raw_event,
};

module_hid_driver(ttpsu_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("awkeng");
MODULE_DESCRIPTION("hwmon driver for Thermaltake Toughpower PSUs with a USB HID interface");
