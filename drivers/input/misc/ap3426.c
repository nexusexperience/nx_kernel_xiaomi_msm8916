/* Copyright (c) 2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/sysctl.h>
#include <linux/regulator/consumer.h>
#include <linux/input.h>
#include <linux/regmap.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/interrupt.h>
#include <linux/sensors.h>
#include <linux/pm_wakeup.h>
#include <linux/uaccess.h>
#include <linux/atomic.h>

#define AP3426_I2C_NAME			"ap3426"
#define AP3426_LIGHT_INPUT_NAME		"ap3426-light"
#define AP3426_PROXIMITY_INPUT_NAME	"ap3426-proximity"

/* AP3426 registers */
#define AP3426_REG_CONFIG		0x00
#define AP3426_REG_INT_FLAG		0x01
#define AP3426_REG_INT_CTL		0x02
#define AP3426_REG_WAIT_TIME		0x06
#define AP3426_REG_IR_DATA_LOW		0x0A
#define AP3426_REG_IR_DATA_HIGH		0x0B
#define AP3426_REG_ALS_DATA_LOW		0x0C
#define AP3426_REG_ALS_DATA_HIGH	0x0D
#define AP3426_REG_PS_DATA_LOW		0x0E
#define AP3426_REG_PS_DATA_HIGH		0x0F
#define AP3426_REG_ALS_GAIN		0x10
#define AP3426_REG_ALS_PERSIST		0x14
#define AP3426_REG_ALS_LOW_THRES_0	0x1A
#define AP3426_REG_ALS_LOW_THRES_1	0x1B
#define AP3426_REG_ALS_HIGH_THRES_0	0x1C
#define AP3426_REG_ALS_HIGH_THRES_1	0x1D
#define AP3426_REG_PS_GAIN		0x20
#define AP3426_REG_PS_LED_DRIVER	0x21
#define AP3426_REG_PS_INT_FORM		0x22
#define AP3426_REG_PS_MEAN_TIME		0x23
#define AP3426_REG_PS_SMART_INT		0x24
#define AP3426_REG_PS_INT_TIME		0x25
#define AP3426_REG_PS_PERSIST		0x26
#define AP3426_REG_PS_CAL_L		0x28
#define AP3426_REG_PS_CAL_H		0x29
#define AP3426_REG_PS_LOW_THRES_0	0x2A
#define AP3426_REG_PS_LOW_THRES_1	0x2B
#define AP3426_REG_PS_HIGH_THRES_0	0x2C
#define AP3426_REG_PS_HIGH_THRES_1	0x2D

#define AP3426_REG_MAGIC		0xFF
#define AP3426_REG_COUNT		0x2E

#define AP3426_ALS_INT_MASK		0x01
#define AP3426_PS_INT_MASK		0x02

/* AP3426 ALS data is 16 bit */
#define ALS_DATA_MASK		0xffff
#define ALS_LOW_BYTE(data)	((data) & 0xff)
#define ALS_HIGH_BYTE(data)	(((data) >> 8) & 0xff)

/* AP3426 PS data is 10 bit */
#define PS_DATA_MASK		0x3ff
#define PS_LOW_BYTE(data)	((data) & 0xff)
#define PS_HIGH_BYTE(data)	(((data) >> 8) & 0x3)

/* default als sensitivity in lux */
#define AP3426_ALS_SENSITIVITY		50

/* AP3426 takes at least 10ms to boot up */
#define AP3426_BOOT_TIME_MS		12

enum {
	CMD_WRITE = 0,
	CMD_READ = 1,
};

struct regulator_map {
	struct regulator	*regulator;
	int			min_uv;
	int			max_uv;
	char			*supply;
};

struct pinctrl_config {
	struct pinctrl		*pinctrl;
	struct pinctrl_state	*state[2];
	char			*name[2];
};

struct ap3426_data {
	struct i2c_client	*i2c;
	struct regmap		*regmap;
	struct regulator	*config;
	struct input_dev	*input_light;
	struct input_dev	*input_proximity;
	struct workqueue_struct	*workqueue;

	struct sensors_classdev	als_cdev;
	struct sensors_classdev	ps_cdev;
	struct mutex		ops_lock;
	struct work_struct	report_work;
	struct work_struct	als_enable_work;
	struct work_struct	als_disable_work;
	struct work_struct	ps_enable_work;
	struct work_struct	ps_disable_work;
	atomic_t		wake_count;

	int			irq_gpio;
	int			irq;
	bool			als_enabled;
	bool			ps_enabled;
	u32			irq_flags;
	unsigned int		als_delay;
	unsigned int		ps_delay;
	int			als_cal;
	int			ps_cal;
	int			als_gain;
	int			als_persist;
	unsigned int		als_sensitivity;
	int			ps_gain;
	int			ps_persist;
	int			ps_led_driver;
	int			ps_mean_time;
	int			ps_integrated_time;
	int			ps_wakeup_threshold;

	int			last_als;
	int			last_ps;
	int			flush_count;
	int			power_enabled;

	unsigned int		reg_addr;
};


static struct regulator_map power_config[] = {
	{.supply = "vdd", .min_uv = 2000000, .max_uv = 3300000, },
	{.supply = "vio", .min_uv = 1750000, .max_uv = 1950000, },
};

static struct pinctrl_config pin_config = {
	.name = { "default", "sleep" },
};

static int gain_table[] = { 32768, 8192, 2048, 512 };
static int pmt_table[] = { 5, 10, 14, 19 }; /* 5.0 9.6, 14.1 18.7 */

/* PS distance table */
static int ps_distance_table[] = { 887, 282, 111, 78, 53, 46, };

static struct sensors_classdev als_cdev = {
	.name = "ap3426-light",
	.vendor = "Dyna Image Corporation",
	.version = 1,
	.handle = SENSORS_LIGHT_HANDLE,
	.type = SENSOR_TYPE_LIGHT,
	.max_range = "655360",
	.resolution = "1.0",
	.sensor_power = "0.35",
	.min_delay = 100000,
	.max_delay = 1375,
	.fifo_reserved_event_count = 0,
	.fifo_max_event_count = 0,
	.flags = 2,
	.enabled = 0,
	.delay_msec = 50,
	.sensors_enable = NULL,
	.sensors_poll_delay = NULL,
};

static struct sensors_classdev ps_cdev = {
	.name = "ap3426-proximity",
	.vendor = "Dyna Image Corporation",
	.version = 1,
	.handle = SENSORS_PROXIMITY_HANDLE,
	.type = SENSOR_TYPE_PROXIMITY,
	.max_range = "6",
	.resolution = "1.0",
	.sensor_power = "0.35",
	.min_delay = 5000,
	.max_delay = 1280,
	.fifo_reserved_event_count = 0,
	.fifo_max_event_count = 0,
	.flags = 3,
	.enabled = 0,
	.delay_msec = 50,
	.sensors_enable = NULL,
	.sensors_poll_delay = NULL,
};

static int sensor_power_init(struct device *dev, struct regulator_map *map,
		int size)
{
	int rc;
	int i;

	for (i = 0; i < size; i++) {
		map[i].regulator = devm_regulator_get(dev, map[i].supply);
		if (IS_ERR(map[i].regulator)) {
			rc = PTR_ERR(map[i].regulator);
			dev_err(dev, "Regualtor get failed vdd rc=%d\n", rc);
			goto exit;
		}
		if (regulator_count_voltages(map[i].regulator) > 0) {
			rc = regulator_set_voltage(map[i].regulator,
					map[i].min_uv, map[i].max_uv);
			if (rc) {
				dev_err(dev, "Regulator set failed vdd rc=%d\n",
						rc);
				goto exit;
			}
		}
	}

	return 0;

exit:
	/* Regulator not set correctly */
	for (i = i - 1; i >= 0; i--) {
		if (regulator_count_voltages(map[i].regulator))
			regulator_set_voltage(map[i].regulator, 0,
					map[i].max_uv);
	}

	return rc;
}

static int sensor_power_deinit(struct device *dev, struct regulator_map *map,
		int size)
{
	int i;

	for (i = 0; i < size; i++) {
		if (!IS_ERR_OR_NULL(map[i].regulator)) {
			if (regulator_count_voltages(map[i].regulator) > 0)
				regulator_set_voltage(map[i].regulator, 0,
						map[i].max_uv);
		}
	}

	return 0;
}

static int sensor_power_config(struct device *dev, struct regulator_map *map,
		int size, bool enable)
{
	int i;
	int rc = 0;

	if (enable) {
		for (i = 0; i < size; i++) {
			rc = regulator_enable(map[i].regulator);
			if (rc) {
				dev_err(dev, "enable %s failed.\n",
						map[i].supply);
				goto exit_enable;
			}
		}
	} else {
		for (i = 0; i < size; i++) {
			rc = regulator_disable(map[i].regulator);
			if (rc) {
				dev_err(dev, "disable %s failed.\n",
						map[i].supply);
				goto exit_disable;
			}
		}
	}

	return 0;

exit_enable:
	for (i = i - 1; i >= 0; i--)
		regulator_disable(map[i].regulator);

	return rc;

exit_disable:
	for (i = i - 1; i >= 0; i--)
		if (regulator_enable(map[i].regulator))
			dev_err(dev, "enable %s failed\n", map[i].supply);

	return rc;
}

static int sensor_pinctrl_init(struct device *dev,
		struct pinctrl_config *config)
{
	config->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(config->pinctrl)) {
		dev_err(dev, "Failed to get pinctrl\n");
		return PTR_ERR(config->pinctrl);
	}

	config->state[0] =
		pinctrl_lookup_state(config->pinctrl, config->name[0]);
	if (IS_ERR_OR_NULL(config->state[0])) {
		dev_err(dev, "Failed to look up default state\n");
		return PTR_ERR(config->state[0]);
	}

	config->state[1] =
		pinctrl_lookup_state(config->pinctrl, config->name[1]);
	if (IS_ERR_OR_NULL(config->state[1])) {
		dev_err(dev, "Failed to look up default state\n");
		return PTR_ERR(config->state[1]);
	}

	return 0;
}

static int ap3426_parse_dt(struct device *dev, struct ap3426_data *di)
{
	struct device_node *dp = dev->of_node;
	int rc;
	u32 value;
	int i;

	rc = of_get_named_gpio_flags(dp, "di,irq-gpio", 0,
			&di->irq_flags);
	if (rc < 0) {
		dev_err(dev, "unable to read irq gpio\n");
		return rc;
	}

	di->irq_gpio = rc;

	rc = of_property_read_u32(dp, "di,als-cal", &value);
	if (rc) {
		dev_err(dev, "read di,als-cal failed\n");
		return rc;
	}
	di->als_cal = value;

	rc = of_property_read_u32(dp, "di,als-gain", &value);
	if (rc) {
		dev_err(dev, "read di,als-gain failed\n");
		return rc;
	}
	if (value >= ARRAY_SIZE(gain_table)) {
		dev_err(dev, "di,als-gain out of range\n");
		return -EINVAL;
	}
	di->als_gain = value;

	rc = of_property_read_u32(dp, "di,als-persist", &value);
	if (rc) {
		dev_err(dev, "read di,als-persist failed\n");
		return rc;
	}
	if (value > 0x3f) { /* The maximum value is 63 conversion time. */
		dev_err(dev, "di,als-persist out of range\n");
		return -EINVAL;
	}
	di->als_persist = value;

	rc = of_property_read_u32(dp, "di,ps-gain", &value);
	if (rc) {
		dev_err(dev, "read di,ps-gain failed\n");
		return rc;
	}
	if (value > 0x03) { /* The maximum value is 3, stands for 8x gain. */
		dev_err(dev, "proximity gain out of range\n");
		return -EINVAL;
	}
	di->ps_gain = value;

	rc = of_property_read_u32(dp, "di,ps-persist", &value);
	if (rc) {
		dev_err(dev, "read di,ps-persist failed\n");
		return rc;
	}
	if (value > 0x3f) { /* The maximum value is 63 conversion time. */
		dev_err(dev, "di,ps-persist out of range\n");
		return -EINVAL;
	}
	di->ps_persist = value;

	rc = of_property_read_u32(dp, "di,ps-led-driver", &value);
	if (rc) {
		dev_err(dev, "read di,ps-led-driver failed\n");
		return rc;
	}
	if (value > 0x03) { /* The maximum value is 3, stands for 100% duty. */
		dev_err(dev, "led driver out of range\n");
		return -EINVAL;
	}
	di->ps_led_driver = value;

	rc = of_property_read_u32(dp, "di,ps-mean-time", &value);
	if (rc) {
		dev_err(dev, "di,ps-mean-time incorrect\n");
		return rc;
	}
	if (value >= ARRAY_SIZE(pmt_table)) {
		dev_err(dev, "ps mean time out of range\n");
		return -EINVAL;
	}
	di->ps_mean_time = value;

	rc = of_property_read_u32(dp, "di,ps-integrated-time", &value);
	if (rc) {
		dev_err(dev, "read di,ps-intergrated-time failed\n");
		return rc;
	}
	if (value > 0x3f) { /* The maximum value is 63. */
		dev_err(dev, "ps integrated time out of range\n");
		return -EINVAL;
	}
	di->ps_integrated_time = value;

	rc = of_property_read_u32(dp, "di,wakeup-threshold", &value);
	if (rc) {
		dev_info(dev, "di,wakeup-threshold incorrect, drop to default\n");
		value = -1;
	}
	if (rc >= ARRAY_SIZE(ps_distance_table)) {
		dev_err(dev, "wakeup threshold too big\n");
		return -EINVAL;
	}
	di->ps_wakeup_threshold = value;

	rc = of_property_read_u32(dp, "di,als-sensitivity", &value);
	if (rc) {
		dev_info(dev,
			"di,als-sensitivity is not correctly set");
		value = AP3426_ALS_SENSITIVITY;
	}

	/* formula to transfer sensitivity in lux to adc value */
	di->als_sensitivity = (value * 10 << 16) /
		(gain_table[di->als_gain] * di->als_cal);

	if (di->als_sensitivity == 0) {
		dev_info(dev,
			"als sensitivity %d can't reach. Drop to highest.\n",
			value);
		di->als_sensitivity = 1;
	}

	rc = of_property_read_u32_array(dp, "di,ps-distance-table",
			ps_distance_table, ARRAY_SIZE(ps_distance_table));
	if ((rc == -ENODATA) || (rc == -EOVERFLOW)) {
		dev_err(dev, "di,ps-distance-table is not correctly set\n");
		return rc;
	}

	for (i = 1; i < ARRAY_SIZE(ps_distance_table); i++) {
		if (ps_distance_table[i - 1] < ps_distance_table[i]) {
			dev_err(dev, "ps distance table should in descend order\n");
			return -EINVAL;
		}
	}

	if (ps_distance_table[0] > PS_DATA_MASK) {
		dev_err(dev, "distance table out of range\n");
		return -EINVAL;
	}

	return 0;
}

static int ap3426_check_device(struct ap3426_data *di)
{
	unsigned int part_id;
	int rc;

	/* AP3426 don't have part id registers */
	rc = regmap_read(di->regmap, AP3426_REG_CONFIG, &part_id);
	if (rc) {
		dev_err(&di->i2c->dev, "read reg %d failed.(%d)\n",
				AP3426_REG_CONFIG, rc);
		return rc;
	}

	dev_dbg(&di->i2c->dev, "register 0x%d:0x%x\n", AP3426_REG_CONFIG,
			part_id);
	return 0;
}

static int ap3426_init_input(struct ap3426_data *di)
{
	struct input_dev *input;
	int status;

	input = devm_input_allocate_device(&di->i2c->dev);
	if (!input) {
		dev_err(&di->i2c->dev, "allocate light input device failed\n");
		return PTR_ERR(input);
	}

	input->name = AP3426_LIGHT_INPUT_NAME;
	input->phys = "ap3426/input0";
	input->id.bustype = BUS_I2C;

	__set_bit(EV_ABS, input->evbit);
	input_set_abs_params(input, ABS_MISC, 0, 655360, 0, 0);

	status = input_register_device(input);
	if (status) {
		dev_err(&di->i2c->dev, "register light input device failed.\n");
		return status;
	}

	di->input_light = input;

	input = devm_input_allocate_device(&di->i2c->dev);
	if (!input) {
		dev_err(&di->i2c->dev, "allocate light input device failed\n");
		return PTR_ERR(input);
	}

	input->name = AP3426_PROXIMITY_INPUT_NAME;
	input->phys = "ap3426/input1";
	input->id.bustype = BUS_I2C;

	__set_bit(EV_ABS, input->evbit);
	input_set_abs_params(input, ABS_DISTANCE, 0, 1023, 0, 0);

	status = input_register_device(input);
	if (status) {
		dev_err(&di->i2c->dev, "register proxmity input device failed.\n");
		return status;
	}

	di->input_proximity = input;

	return 0;
}

static int ap3426_init_device(struct ap3426_data *di)
{
	int rc;

	/* Enable als/ps interrupt and clear interrupt by software */
	rc = regmap_write(di->regmap, AP3426_REG_INT_CTL, 0x89);
	if (rc) {
		dev_err(&di->i2c->dev, "write %d register failed\n",
				AP3426_REG_INT_CTL);
		return rc;
	}

	/* Set als gain */
	rc = regmap_write(di->regmap, AP3426_REG_ALS_GAIN, di->als_gain << 4);
	if (rc) {
		dev_err(&di->i2c->dev, "write %d register failed\n",
				AP3426_REG_ALS_GAIN);
		return rc;
	}

	/* Set als persistense */
	rc = regmap_write(di->regmap, AP3426_REG_ALS_PERSIST, di->als_persist);
	if (rc) {
		dev_err(&di->i2c->dev, "write %d register failed\n",
				AP3426_REG_ALS_PERSIST);
		return rc;
	}

	/* Set ps interrupt form */
	rc = regmap_write(di->regmap, AP3426_REG_PS_INT_FORM, 0);
	if (rc) {
		dev_err(&di->i2c->dev, "write %d register failed\n",
				AP3426_REG_PS_INT_FORM);
		return rc;
	}

	/* Set ps gain */
	rc = regmap_write(di->regmap, AP3426_REG_PS_GAIN, di->ps_gain << 2);
	if (rc) {
		dev_err(&di->i2c->dev, "write %d register failed\n",
				AP3426_REG_PS_GAIN);
		return rc;
	}

	/* Set ps persist */
	rc = regmap_write(di->regmap, AP3426_REG_PS_PERSIST, di->ps_persist);
	if (rc) {
		dev_err(&di->i2c->dev, "write %d register failed\n",
				AP3426_REG_PS_PERSIST);
		return rc;
	}

	/* Set PS LED driver strength */
	rc = regmap_write(di->regmap, AP3426_REG_PS_LED_DRIVER,
			di->ps_led_driver);
	if (rc) {
		dev_err(&di->i2c->dev, "write %d register failed\n",
				AP3426_REG_PS_LED_DRIVER);
		return rc;
	}

	/* Set PS mean time */
	rc = regmap_write(di->regmap, AP3426_REG_PS_MEAN_TIME,
			di->ps_mean_time);
	if (rc) {
		dev_err(&di->i2c->dev, "write %d register failed\n",
				AP3426_REG_PS_MEAN_TIME);
		return rc;
	}

	/* Set PS integrated time */
	rc = regmap_write(di->regmap, AP3426_REG_PS_INT_TIME,
			di->ps_integrated_time);
	if (rc) {
		dev_err(&di->i2c->dev, "write %d register failed\n",
				AP3426_REG_PS_INT_TIME);
		return rc;
	}

	dev_dbg(&di->i2c->dev, "ap3426 initialize sucessful\n");

	return 0;
}

static int ap3426_calc_conversion_time(struct ap3426_data *di, int als_enabled,
		int ps_enabled)
{
	int conversion_time = 0;

	/* ALS conversion time is 100ms */
	if (als_enabled)
		conversion_time = 100;

	if (ps_enabled)
		conversion_time += pmt_table[di->ps_mean_time] +
			di->ps_mean_time * di->ps_integrated_time / 16;

	return conversion_time;
}

/* Read raw data, convert it to human readable values, report it and
 * reconfigure the sensor.
 */
static int ap3426_process_data(struct ap3426_data *di, int als_ps)
{
	unsigned int gain;
	ktime_t timestamp;
	int rc = 0;

	unsigned int tmp;
	u8 als_data[4];
	int lux;

	u8 ps_data[4];
	int i;
	int distance;

	timestamp = ktime_get_boottime();

	if (als_ps) { /* process als value */
		/* Read data */
		rc = regmap_bulk_read(di->regmap, AP3426_REG_ALS_DATA_LOW,
				als_data, 2);
		if (rc) {
			dev_err(&di->i2c->dev, "read %d failed.(%d)\n",
					AP3426_REG_ALS_DATA_LOW, rc);
			goto exit;
		}

		gain = gain_table[di->als_gain];

		/* lower bit */
		lux = (als_data[0] * di->als_cal * gain / 10) >> 16;
		/* higher bit */
		lux += (als_data[1] * di->als_cal * gain / 10) >> 8;

		dev_dbg(&di->i2c->dev, "lux:%d als_data:0x%x-0x%x\n",
				lux, als_data[0], als_data[1]);

		if (lux != di->last_als)  {
			input_report_abs(di->input_light, ABS_MISC, lux);
			input_event(di->input_light, EV_SYN, SYN_TIME_SEC,
					ktime_to_timespec(timestamp).tv_sec);
			input_event(di->input_light, EV_SYN, SYN_TIME_NSEC,
					ktime_to_timespec(timestamp).tv_nsec);
			input_sync(di->input_light);
		}

		di->last_als = lux;
		/* Set up threshold */
		tmp = als_data[0] | (als_data[1] << 8);

		/* lower threshold */
		if (tmp < di->als_sensitivity) {
			als_data[0] = 0x0;
			als_data[1] = 0x0;
		} else {
			als_data[0] = ALS_LOW_BYTE(tmp - di->als_sensitivity);
			als_data[1] = ALS_HIGH_BYTE(tmp - di->als_sensitivity);
		}

		/* upper threshold */
		if (tmp + di->als_sensitivity > ALS_DATA_MASK) {
			als_data[2] = 0xff;
			als_data[3] = 0xff;
		} else {
			als_data[2] = ALS_LOW_BYTE(tmp + di->als_sensitivity);
			als_data[3] = ALS_HIGH_BYTE(tmp + di->als_sensitivity);
		}

		rc = regmap_bulk_write(di->regmap, AP3426_REG_ALS_LOW_THRES_0,
				als_data, 4);
		if (rc) {
			dev_err(&di->i2c->dev, "write %d failed.(%d)\n",
					AP3426_REG_ALS_LOW_THRES_0, rc);
			goto exit;
		}

		dev_dbg(&di->i2c->dev, "als threshold: 0x%x 0x%x 0x%x 0x%x\n",
				als_data[0], als_data[1], als_data[2],
				als_data[3]);

	} else { /* process ps value*/
		rc = regmap_bulk_read(di->regmap, AP3426_REG_PS_DATA_LOW,
				ps_data, 2);
		if (rc) {
			dev_err(&di->i2c->dev, "read %d failed.(%d)\n",
					AP3426_REG_PS_DATA_LOW, rc);
			goto exit;
		}

		dev_dbg(&di->i2c->dev, "ps data: 0x%x 0x%x\n",
				ps_data[0], ps_data[1]);

		tmp = ps_data[0] | (ps_data[1] << 8);
		for (i = 0; i < ARRAY_SIZE(ps_distance_table); i++) {
			if (tmp > ps_distance_table[i])
				break;
		}
		distance = i;

		dev_dbg(&di->i2c->dev, "reprt work ps_data:%d\n", tmp);

		/* Report ps data */
		if (distance != di->last_ps) {
			input_report_abs(di->input_proximity, ABS_DISTANCE,
					distance);
			input_event(di->input_proximity, EV_SYN, SYN_TIME_SEC,
					ktime_to_timespec(timestamp).tv_sec);
			input_event(di->input_proximity, EV_SYN, SYN_TIME_NSEC,
					ktime_to_timespec(timestamp).tv_nsec);
			input_sync(di->input_proximity);
		}

		di->last_ps = distance;

		/* lower threshold */
		if (distance < ARRAY_SIZE(ps_distance_table))
			tmp = ps_distance_table[distance];
		else
			tmp = 0;

		ps_data[0] = PS_LOW_BYTE(tmp);
		ps_data[1] = PS_HIGH_BYTE(tmp);

		/* upper threshold */
		if (distance > 0)
			tmp = ps_distance_table[distance - 1];
		else
			tmp = 0x3ff;

		ps_data[2] = PS_LOW_BYTE(tmp);
		ps_data[3] = PS_HIGH_BYTE(tmp);

		dev_dbg(&di->i2c->dev, "ps threshold: 0x%x 0x%x 0x%x 0x%x\n",
				ps_data[0], ps_data[1], ps_data[2], ps_data[3]);

		rc = regmap_bulk_write(di->regmap, AP3426_REG_PS_LOW_THRES_0,
				ps_data, 4);
		if (rc) {
			dev_err(&di->i2c->dev, "write %d failed.(%d)\n",
					AP3426_REG_PS_LOW_THRES_0, rc);
			goto exit;
		}

		dev_dbg(&di->i2c->dev, "ps report exit\n");
	}

exit:
	return rc;
}

static irqreturn_t ap3426_irq_handler(int irq, void *data)
{
	struct ap3426_data *di = data;

	/* wake up event should hold a wake lock until reported */
	if (atomic_inc_return(&di->wake_count) == 1)
		pm_stay_awake(&di->i2c->dev);

	queue_work(di->workqueue, &di->report_work);

	return IRQ_HANDLED;
}

static void ap3426_report_work(struct work_struct *work)
{
	struct ap3426_data *di = container_of(work, struct ap3426_data,
			report_work);
	int rc;
	unsigned int status;

	mutex_lock(&di->ops_lock);
	rc = regmap_read(di->regmap, AP3426_REG_INT_FLAG, &status);
	if (rc) {
		dev_err(&di->i2c->dev, "read %d failed.(%d)\n",
				AP3426_REG_INT_FLAG, rc);
		goto exit;
	}

	dev_dbg(&di->i2c->dev, "interrupt issued status=0x%x.\n", status);

	if (!(status & 0x02)) {
		dev_dbg(&di->i2c->dev, "not a proximity event\n");
		if (atomic_dec_and_test(&di->wake_count))
			pm_relax(&di->i2c->dev);
	}

	/* als interrupt issueed */
	if ((status & AP3426_ALS_INT_MASK) && (di->als_enabled)) {
		rc = ap3426_process_data(di, 1);
		if (rc)
			goto exit;
		dev_dbg(&di->i2c->dev, "process als data done!\n");
	}

	if ((status & AP3426_PS_INT_MASK) && (di->ps_enabled)) {
		rc = ap3426_process_data(di, 0);
		if (rc)
			goto exit;
		dev_dbg(&di->i2c->dev, "process ps data done!\n");
	}

exit:
	/* clear interrupt */
	if (regmap_write(di->regmap, AP3426_REG_INT_FLAG, 0x0))
		dev_err(&di->i2c->dev, "clear interrupt failed\n");

	/* sensor event processing done */
	if (status & 0x02) {
		dev_dbg(&di->i2c->dev, "proximity data processing done!\n");
		if (atomic_dec_and_test(&di->wake_count))
			pm_relax(&di->i2c->dev);

		/* Hold a 200ms wake lock to allow framework handle it */
		if (di->ps_enabled)
			pm_wakeup_event(&di->input_proximity->dev, 200);
	}

	mutex_unlock(&di->ops_lock);
}

static int ap3426_enable_ps(struct ap3426_data *di, int enable)
{
	unsigned int config;
	int rc = 0;

	rc = regmap_read(di->regmap, AP3426_REG_CONFIG, &config);
	if (rc) {
		dev_err(&di->i2c->dev, "read %d failed.(%d)\n",
				AP3426_REG_CONFIG, rc);
		goto exit;
	}

	/* avoid operate sensor in different executing context */
	if (enable) {
		/* Enable ps sensor */
		rc = regmap_write(di->regmap, AP3426_REG_CONFIG, config | 0x02);
		if (rc) {
			dev_err(&di->i2c->dev, "write %d failed.(%d)\n",
					AP3426_REG_CONFIG, rc);
			goto exit;
		}

		/* wait the data ready */
		msleep(ap3426_calc_conversion_time(di, di->als_enabled, 1));

		/* Clear last value and report it even not change. */
		di->last_ps = -1;
		rc = ap3426_process_data(di, 0);
		if (rc) {
			dev_err(&di->i2c->dev, "process ps data failed\n");
			goto exit;
		}

		/* clear interrupt */
		rc = regmap_write(di->regmap, AP3426_REG_INT_FLAG, 0x0);
		if (rc) {
			dev_err(&di->i2c->dev, "clear interrupt failed\n");
			goto exit;
		}

		di->ps_enabled = true;
	} else {
		/* disable the ps_sensor */
		rc = regmap_write(di->regmap, AP3426_REG_CONFIG,
				config & (~0x2));
		if (rc) {
			dev_err(&di->i2c->dev, "write %d failed.(%d)\n",
					AP3426_REG_CONFIG, rc);
			goto exit;
		}
		di->ps_enabled = false;
	}
exit:
	return rc;
}

static int ap3426_enable_als(struct ap3426_data *di, int enable)
{
	unsigned int config;
	int rc = 0;

	/* Read the system config register */
	rc = regmap_read(di->regmap, AP3426_REG_CONFIG, &config);
	if (rc) {
		dev_err(&di->i2c->dev, "read %d failed.(%d)\n",
				AP3426_REG_CONFIG, rc);
		goto exit;
	}

	if (enable) {
		/* enable als_sensor */
		rc = regmap_write(di->regmap, AP3426_REG_CONFIG, config | 0x01);
		if (rc) {
			dev_err(&di->i2c->dev, "write %d failed.(%d)\n",
					AP3426_REG_CONFIG, rc);
			goto exit;
		}

		/* wait data ready */
		msleep(ap3426_calc_conversion_time(di, 1, di->ps_enabled));

		/* Clear last value and report even not change. */
		di->last_als = -1;

		rc = ap3426_process_data(di, 1);
		if (rc) {
			dev_err(&di->i2c->dev, "process als data failed\n");
			goto exit;
		}

		/* clear interrupt */
		rc = regmap_write(di->regmap, AP3426_REG_INT_FLAG, 0x0);
		if (rc) {
			dev_err(&di->i2c->dev, "clear interrupt failed\n");
			goto exit;
		}

		di->als_enabled = 1;

	} else {
		/* disable the als_sensor */
		rc = regmap_write(di->regmap, AP3426_REG_CONFIG,
				config & (~0x1));
		if (rc) {
			dev_err(&di->i2c->dev, "write %d failed.(%d)\n",
					AP3426_REG_CONFIG, rc);
			goto exit;
		}
		di->als_enabled = 0;
	}
exit:
	return rc;
}

/* Sync delay to hardware according configurations
 * Note one of the sensor may report faster than expected.
 */
static int ap3426_sync_delay(struct ap3426_data *di, int als_enabled,
		int ps_enabled, unsigned int als_delay, unsigned int ps_delay)
{
	unsigned int convert_msec;
	unsigned int delay;
	int rc;

	convert_msec = ap3426_calc_conversion_time(di, als_enabled, ps_enabled);

	if (als_enabled && ps_enabled)
		delay = min(als_delay, ps_delay);
	else if (als_enabled)
		delay = als_delay;
	else if (ps_enabled)
		delay = ps_delay;
	else
		return 0;

	if (delay < convert_msec)
		delay = 0;
	else
		delay -= convert_msec;

	/* Insert delay_msec into wait slots. The maximum is 255 * 5ms */
	rc = regmap_write(di->regmap, AP3426_REG_WAIT_TIME,
			min(delay / 5UL, 255UL));
	if (rc) {
		dev_err(&di->i2c->dev, "write %d failed\n",
				AP3426_REG_WAIT_TIME);
		return rc;
	}

	return 0;
}

static void ap3426_als_enable_work(struct work_struct *work)
{
	struct ap3426_data *di = container_of(work, struct ap3426_data,
			als_enable_work);

	mutex_lock(&di->ops_lock);
	if (!di->power_enabled) {
		if (sensor_power_config(&di->i2c->dev, power_config,
					ARRAY_SIZE(power_config), true)) {
			dev_err(&di->i2c->dev, "power up sensor failed.\n");
			goto exit;
		}

		msleep(AP3426_BOOT_TIME_MS);
		di->power_enabled = true;
		if (ap3426_init_device(di)) {
			dev_err(&di->i2c->dev, "init device failed\n");
			goto exit_power_off;
		}
	}

	/* Old HAL: Sync to last delay. New HAL: Sync to current delay */
	if (ap3426_sync_delay(di, 1, di->ps_enabled, di->als_delay,
				di->ps_delay))
		goto exit_power_off;

	if (ap3426_enable_als(di, 1)) {
		dev_err(&di->i2c->dev, "enable als failed\n");
		goto exit_power_off;
	}

exit_power_off:
	if ((!di->als_enabled) && (!di->ps_enabled) &&
			di->power_enabled) {
		if (sensor_power_config(&di->i2c->dev, power_config,
					ARRAY_SIZE(power_config), false)) {
			dev_err(&di->i2c->dev, "power up sensor failed.\n");
			goto exit;
		}
		di->power_enabled = false;
	}

exit:
	mutex_unlock(&di->ops_lock);
	return;
}


static void ap3426_als_disable_work(struct work_struct *work)
{
	struct ap3426_data *di = container_of(work, struct ap3426_data,
			als_disable_work);

	mutex_lock(&di->ops_lock);

	if (ap3426_enable_als(di, 0)) {
		dev_err(&di->i2c->dev, "disable als failed\n");
		goto exit;
	}

	if (ap3426_sync_delay(di, 0, di->ps_enabled, di->als_delay,
			di->ps_delay))
		goto exit;

	if ((!di->als_enabled) && (!di->ps_enabled) && di->power_enabled) {
		if (sensor_power_config(&di->i2c->dev, power_config,
					ARRAY_SIZE(power_config), false)) {
			dev_err(&di->i2c->dev, "power up sensor failed.\n");
			goto exit;
		}

		di->power_enabled = false;
	}

exit:
	mutex_unlock(&di->ops_lock);
}

static void ap3426_ps_enable_work(struct work_struct *work)
{
	struct ap3426_data *di = container_of(work, struct ap3426_data,
			ps_enable_work);

	mutex_lock(&di->ops_lock);
	if (!di->power_enabled) {
		if (sensor_power_config(&di->i2c->dev, power_config,
					ARRAY_SIZE(power_config), true)) {
			dev_err(&di->i2c->dev, "power up sensor failed.\n");
			goto exit;
		}

		msleep(AP3426_BOOT_TIME_MS);
		di->power_enabled = true;

		if (ap3426_init_device(di)) {
			dev_err(&di->i2c->dev, "init device failed\n");
			goto exit_power_off;
		}
	}

	/* Old HAL: Sync to last delay. New HAL: Sync to current delay */
	if (ap3426_sync_delay(di, di->als_enabled, 1, di->als_delay,
				di->ps_delay))
		goto exit_power_off;

	if (ap3426_enable_ps(di, 1)) {
		dev_err(&di->i2c->dev, "enable ps failed\n");
		goto exit_power_off;
	}

exit_power_off:
	if ((!di->als_enabled) && (!di->ps_enabled) &&
			di->power_enabled) {
		if (sensor_power_config(&di->i2c->dev, power_config,
					ARRAY_SIZE(power_config), false)) {
			dev_err(&di->i2c->dev, "power up sensor failed.\n");
			goto exit;
		}
		di->power_enabled = false;
	}

exit:
	mutex_unlock(&di->ops_lock);
	return;
}

static void ap3426_ps_disable_work(struct work_struct *work)
{
	struct ap3426_data *di = container_of(work, struct ap3426_data,
			ps_disable_work);

	mutex_lock(&di->ops_lock);

	if (ap3426_enable_ps(di, 0)) {
		dev_err(&di->i2c->dev, "disable ps failed\n");
		goto exit;
	}

	if (ap3426_sync_delay(di, di->als_enabled, 0, di->als_delay,
			di->ps_delay))
		goto exit;

	if ((!di->als_enabled) && (!di->ps_enabled) && di->power_enabled) {
		if (sensor_power_config(&di->i2c->dev, power_config,
					ARRAY_SIZE(power_config), false)) {
			dev_err(&di->i2c->dev, "power up sensor failed.\n");
			goto exit;
		}

		di->power_enabled = false;
	}
exit:
	mutex_unlock(&di->ops_lock);
}

static struct regmap_config ap3426_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int ap3426_cdev_enable_als(struct sensors_classdev *sensors_cdev,
		unsigned int enable)
{
	int res = 0;
	struct ap3426_data *di = container_of(sensors_cdev,
			struct ap3426_data, als_cdev);

	mutex_lock(&di->ops_lock);

	if (enable)
		queue_work(di->workqueue, &di->als_enable_work);
	else
		queue_work(di->workqueue, &di->als_disable_work);

	mutex_unlock(&di->ops_lock);

	return res;
}

static int ap3426_cdev_enable_ps(struct sensors_classdev *sensors_cdev,
		unsigned int enable)
{
	struct ap3426_data *di = container_of(sensors_cdev,
			struct ap3426_data, ps_cdev);

	mutex_lock(&di->ops_lock);

	if (enable)
		queue_work(di->workqueue, &di->ps_enable_work);
	else
		queue_work(di->workqueue, &di->ps_disable_work);

	mutex_unlock(&di->ops_lock);

	return 0;
}

static int ap3426_cdev_set_als_delay(struct sensors_classdev *sensors_cdev,
		unsigned int delay_msec)
{
	struct ap3426_data *di = container_of(sensors_cdev,
			struct ap3426_data, als_cdev);

	mutex_lock(&di->ops_lock);

	di->als_delay = delay_msec;
	ap3426_sync_delay(di, di->als_enabled, di->ps_enabled,
			di->als_delay, di->ps_delay);

	mutex_unlock(&di->ops_lock);

	return 0;
}

static int ap3426_cdev_set_ps_delay(struct sensors_classdev *sensors_cdev,
		unsigned int delay_msec)
{
	struct ap3426_data *di = container_of(sensors_cdev,
			struct ap3426_data, ps_cdev);

	mutex_lock(&di->ops_lock);

	di->ps_delay = delay_msec;
	ap3426_sync_delay(di, di->als_enabled, di->ps_enabled,
			di->als_delay, di->ps_delay);

	mutex_unlock(&di->ops_lock);

	return 0;
}

static int ap3426_cdev_ps_flush(struct sensors_classdev *sensors_cdev)
{
	struct ap3426_data *di = container_of(sensors_cdev,
			struct ap3426_data, ps_cdev);

	input_event(di->input_proximity, EV_SYN, SYN_CONFIG,
			di->flush_count++);
	input_sync(di->input_proximity);

	return 0;
}

static int ap3426_cdev_als_flush(struct sensors_classdev *sensors_cdev)
{
	struct ap3426_data *di = container_of(sensors_cdev,
			struct ap3426_data, als_cdev);

	input_event(di->input_light, EV_SYN, SYN_CONFIG, di->flush_count++);
	input_sync(di->input_light);

	return 0;
}

static ssize_t ap3426_register_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct ap3426_data *di = dev_get_drvdata(dev);
	unsigned int val;
	int rc;
	ssize_t count = 0;
	int i;

	if (di->reg_addr == AP3426_REG_MAGIC) {
		for (i = 0; i < AP3426_REG_COUNT; i++) {
			rc = regmap_read(di->regmap, AP3426_REG_CONFIG + i,
					&val);
			if (rc) {
				dev_err(&di->i2c->dev, "read %d failed\n",
						AP3426_REG_CONFIG + i);
				break;
			}
			count += snprintf(&buf[count], PAGE_SIZE,
					"0x%x: 0x%x\n", AP3426_REG_CONFIG + i,
					val);
		}
	} else {
		rc = regmap_read(di->regmap, di->reg_addr, &val);
		if (rc) {
			dev_err(&di->i2c->dev, "read %d failed\n",
					di->reg_addr);
			return rc;
		}
		count += snprintf(&buf[count], PAGE_SIZE, "0x%x:0x%x\n",
				di->reg_addr, val);
	}

	return count;
}

static ssize_t ap3426_register_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct ap3426_data *di = dev_get_drvdata(dev);
	unsigned int reg;
	unsigned int val;
	unsigned int cmd;
	int rc;

	if (sscanf(buf, "%u %u %u\n", &cmd, &reg, &val) < 2) {
		dev_err(&di->i2c->dev, "argument error\n");
		return -EINVAL;
	}

	if (cmd == CMD_WRITE) {
		rc = regmap_write(di->regmap, reg, val);
		if (rc) {
			dev_err(&di->i2c->dev, "write %d failed\n", reg);
			return rc;
		}
	} else if (cmd == CMD_READ) {
		di->reg_addr = reg;
		dev_dbg(&di->i2c->dev, "register address set to 0x%x\n", reg);
	}

	return size;
}

static DEVICE_ATTR(register, S_IWUSR | S_IRUGO,
		ap3426_register_show,
		ap3426_register_store);

static struct attribute *ap3426_attr[] = {
	&dev_attr_register.attr,
	NULL
};

static const struct attribute_group ap3426_attr_group = {
	.attrs = ap3426_attr,
};

static int ap3426_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	struct ap3426_data *di;
	int res = 0;

	dev_dbg(&client->dev, "probing ap3426...\n");

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "ap3426 i2c check failed.\n");
		return -ENODEV;
	}

	di = devm_kzalloc(&client->dev, sizeof(struct ap3426_data),
			GFP_KERNEL);
	if (!di) {
		dev_err(&client->dev, "memory allocation failed,\n");
		return -ENOMEM;
	}

	di->i2c = client;

	if (client->dev.of_node) {
		res = ap3426_parse_dt(&client->dev, di);
		if (res) {
			dev_err(&client->dev,
				"unable to parse device tree.(%d)\n", res);
			goto out;
		}
	} else {
		dev_err(&client->dev, "device tree not found.\n");
		res = -ENODEV;
		goto out;
	}

	dev_set_drvdata(&client->dev, di);
	mutex_init(&di->ops_lock);

	di->regmap = devm_regmap_init_i2c(client, &ap3426_regmap_config);
	if (IS_ERR(di->regmap)) {
		dev_err(&client->dev, "init regmap failed.(%ld)\n",
				PTR_ERR(di->regmap));
		res = PTR_ERR(di->regmap);
		goto out;
	}

	res = sensor_power_init(&client->dev, power_config,
			ARRAY_SIZE(power_config));
	if (res) {
		dev_err(&client->dev, "init power failed.\n");
		goto out;
	}

	res = sensor_power_config(&client->dev, power_config,
			ARRAY_SIZE(power_config), true);
	if (res) {
		dev_err(&client->dev, "power up sensor failed.\n");
		goto out;
	}

	res = sensor_pinctrl_init(&client->dev, &pin_config);
	if (res) {
		dev_err(&client->dev, "init pinctrl failed.\n");
		goto err_pinctrl_init;
	}

	/* wait the device to boot up */
	msleep(AP3426_BOOT_TIME_MS);

	res = ap3426_check_device(di);
	if (res) {
		dev_err(&client->dev, "check device failed.\n");
		goto err_check_device;
	}

	res = ap3426_init_device(di);
	if (res) {
		dev_err(&client->dev, "check device failed.\n");
		goto err_init_device;
	}

	/* configure interrupt */
	if (gpio_is_valid(di->irq_gpio)) {
		res = gpio_request(di->irq_gpio, "ap3426_interrupt");
		if (res) {
			dev_err(&client->dev,
				"unable to request interrupt gpio %d\n",
				di->irq_gpio);
			goto err_request_gpio;
		}

		res = gpio_direction_input(di->irq_gpio);
		if (res) {
			dev_err(&client->dev,
				"unable to set direction for gpio %d\n",
				di->irq_gpio);
			goto err_set_direction;
		}

		di->irq = gpio_to_irq(di->irq_gpio);

		res = devm_request_irq(&client->dev, di->irq,
				ap3426_irq_handler,
				di->irq_flags | IRQF_ONESHOT,
				"ap3426", di);

		if (res) {
			dev_err(&client->dev,
					"request irq %d failed(%d),\n",
					di->irq, res);
			goto err_request_irq;
		}

		/* device wakeup initialization */
		device_init_wakeup(&client->dev, 1);

		di->workqueue = alloc_workqueue("ap3426_workqueue",
				WQ_NON_REENTRANT | WQ_FREEZABLE, 0);
		INIT_WORK(&di->report_work, ap3426_report_work);
		INIT_WORK(&di->als_enable_work, ap3426_als_enable_work);
		INIT_WORK(&di->als_disable_work, ap3426_als_disable_work);
		INIT_WORK(&di->ps_enable_work, ap3426_ps_enable_work);
		INIT_WORK(&di->ps_disable_work, ap3426_ps_disable_work);

	} else {
		res = -ENODEV;
		goto err_init_device;
	}

	res = sysfs_create_group(&client->dev.kobj, &ap3426_attr_group);
	if (res) {
		dev_err(&client->dev, "sysfs create group failed\n");
		goto err_create_group;
	}

	res = ap3426_init_input(di);
	if (res) {
		dev_err(&client->dev, "init input failed.\n");
		goto err_init_input;
	}

	/* input device should hold a 200ms wake lock */
	device_init_wakeup(&di->input_proximity->dev, 1);

	di->als_cdev = als_cdev;
	di->als_cdev.sensors_enable = ap3426_cdev_enable_als;
	di->als_cdev.sensors_poll_delay = ap3426_cdev_set_als_delay;
	di->als_cdev.sensors_flush = ap3426_cdev_als_flush;
	res = sensors_classdev_register(&di->input_light->dev, &di->als_cdev);
	if (res) {
		dev_err(&client->dev, "sensors class register failed.\n");
		goto err_register_als_cdev;
	}

	di->ps_cdev = ps_cdev;
	di->ps_cdev.sensors_enable = ap3426_cdev_enable_ps;
	di->ps_cdev.sensors_poll_delay = ap3426_cdev_set_ps_delay;
	di->ps_cdev.sensors_flush = ap3426_cdev_ps_flush;
	res = sensors_classdev_register(&di->input_proximity->dev,
			&di->ps_cdev);
	if (res) {
		dev_err(&client->dev, "sensors class register failed.\n");
		goto err_register_ps_cdev;
	}

	sensor_power_config(&client->dev, power_config,
			ARRAY_SIZE(power_config), false);

	dev_info(&client->dev, "ap3426 successfully probed!\n");

	return 0;

err_register_ps_cdev:
	sensors_classdev_unregister(&di->als_cdev);
err_register_als_cdev:
	device_init_wakeup(&di->input_proximity->dev, 0);
err_init_input:
	sysfs_remove_group(&client->dev.kobj, &ap3426_attr_group);
err_create_group:
err_request_irq:
err_set_direction:
	gpio_free(di->irq_gpio);
err_request_gpio:
err_init_device:
	device_init_wakeup(&client->dev, 0);
err_check_device:
err_pinctrl_init:
	sensor_power_deinit(&client->dev, power_config,
			ARRAY_SIZE(power_config));
out:
	return res;
}

static int ap3426_remove(struct i2c_client *client)
{
	struct ap3426_data *di = dev_get_drvdata(&client->dev);

	sensors_classdev_unregister(&di->ps_cdev);
	sensors_classdev_unregister(&di->als_cdev);

	if (di->input_light)
		input_unregister_device(di->input_light);

	if (di->input_proximity)
		input_unregister_device(di->input_proximity);

	destroy_workqueue(di->workqueue);
	device_init_wakeup(&di->i2c->dev, 0);
	device_init_wakeup(&di->input_proximity->dev, 0);

	sensor_power_config(&client->dev, power_config,
			ARRAY_SIZE(power_config), false);
	sensor_power_deinit(&client->dev, power_config,
			ARRAY_SIZE(power_config));

	return 0;
}

static int ap3426_suspend(struct device *dev)
{
	int res = 0;
	struct ap3426_data *di = dev_get_drvdata(dev);
	u8 ps_data[4];
	int idx = di->ps_wakeup_threshold;

	dev_dbg(dev, "suspending ap3426...");

	mutex_lock(&di->ops_lock);

	/* proximity is enabled */
	if (di->ps_enabled) {
		/* Don't power off sensor because proximity is a
		 * wake up sensor.
		 */
		if (device_may_wakeup(&di->i2c->dev)) {
			dev_dbg(&di->i2c->dev, "enable irq wake\n");
			enable_irq_wake(di->irq);
		}

		/* Setup threshold to avoid frequent wakeup */
		if (device_may_wakeup(&di->i2c->dev) && (idx != -1)) {
			dev_dbg(&di->i2c->dev, "last ps: %d\n", di->last_ps);
			if (di->last_ps > idx) {
				ps_data[0] = 0x0;
				ps_data[1] = 0x0;
				ps_data[2] =
					PS_LOW_BYTE(ps_distance_table[idx]);
				ps_data[3] =
					PS_HIGH_BYTE(ps_distance_table[idx]);
			} else {
				ps_data[0] =
					PS_LOW_BYTE(ps_distance_table[idx]);
				ps_data[1] =
					PS_HIGH_BYTE(ps_distance_table[idx]);
				ps_data[2] = 0xff;
				ps_data[3] = 0xff;
			}

			res = regmap_bulk_write(di->regmap,
					AP3426_REG_PS_LOW_THRES_0, ps_data, 4);
			if (res) {
				dev_err(&di->i2c->dev, "set up threshold failed\n");
				goto exit;
			}
		}
	} else {
		/* power off */
		disable_irq(di->irq);
		if (di->power_enabled) {
			res = sensor_power_config(dev, power_config,
					ARRAY_SIZE(power_config), false);
			if (res) {
				dev_err(dev, "failed to suspend ap3426\n");
				enable_irq(di->irq);
				goto exit;
			}
		}
		pinctrl_select_state(pin_config.pinctrl, pin_config.state[1]);
	}
exit:
	mutex_unlock(&di->ops_lock);
	return res;
}

static int ap3426_resume(struct device *dev)
{
	int res = 0;
	struct ap3426_data *di = dev_get_drvdata(dev);

	dev_dbg(dev, "resuming ap3426...");
	if (di->ps_enabled) {
		if (device_may_wakeup(&di->i2c->dev)) {
			dev_dbg(&di->i2c->dev, "disable irq wake\n");
			disable_irq_wake(di->irq);
		}
	} else {
		pinctrl_select_state(pin_config.pinctrl, pin_config.state[0]);
		/* Power up sensor */
		if (di->power_enabled) {
			res = sensor_power_config(dev, power_config,
					ARRAY_SIZE(power_config), true);
			if (res) {
				dev_err(dev, "failed to power up ap3426\n");
				goto exit;
			}
			msleep(AP3426_BOOT_TIME_MS);

			res = ap3426_init_device(di);
			if (res) {
				dev_err(dev, "failed to init ap3426\n");
				goto exit_power_off;
			}

			ap3426_sync_delay(di, di->als_enabled, 0, di->als_delay,
					di->ps_delay);
		}

		if (di->als_enabled) {
			res = ap3426_enable_als(di, di->als_enabled);
			if (res) {
				dev_err(dev, "failed to enable ap3426\n");
				goto exit_power_off;
			}
		}

		enable_irq(di->irq);
	}

	return res;

exit_power_off:
	if ((!di->als_enabled) && (!di->ps_enabled) &&
			di->power_enabled) {
		if (sensor_power_config(&di->i2c->dev, power_config,
					ARRAY_SIZE(power_config), false)) {
			dev_err(&di->i2c->dev, "power up sensor failed.\n");
			goto exit;
		}
		di->power_enabled = false;
	}

exit:
	return res;
}

static const struct i2c_device_id ap3426_id[] = {
	{ AP3426_I2C_NAME, 0 },
	{ }
};

static struct of_device_id ap3426_match_table[] = {
	{ .compatible = "di,ap3426", },
	{ },
};

static const struct dev_pm_ops ap3426_pm_ops = {
	.suspend = ap3426_suspend,
	.resume = ap3426_resume,
};

static struct i2c_driver ap3426_driver = {
	.probe		= ap3426_probe,
	.remove	= ap3426_remove,
	.id_table	= ap3426_id,
	.driver	= {
		.owner	= THIS_MODULE,
		.name	= AP3426_I2C_NAME,
		.of_match_table = ap3426_match_table,
		.pm = &ap3426_pm_ops,
	},
};

module_i2c_driver(ap3426_driver);

MODULE_DESCRIPTION("AP3426 ALPS Driver");
MODULE_LICENSE("GPLv2");

