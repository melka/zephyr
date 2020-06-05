/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT pimonori_pim447

#include <device.h>
#include <drivers/i2c.h>
#include <kernel.h>
#include <drivers/sensor.h>
#include <sys/__assert.h>
#include <logging/log.h>

#include "pim447.h"

LOG_MODULE_REGISTER(PIM447, CONFIG_SENSOR_LOG_LEVEL);

static int pim447_sample_fetch(struct device *dev, enum sensor_channel chan)
{
	struct pim447_data *data = dev->driver_data;
	struct device *i2c = pim447_i2c_device(dev);
	u8_t address = pim447_i2c_address(dev);
	u8_t tx_buf[] = {
		PIM447_CMD_READ_LEFT
	};
	u8_t rx_buf[5];

	__ASSERT_NO_MSG(chan == SENSOR_CHAN_ALL);

	if (i2c_write_read(i2c, address, tx_buf, sizeof(tx_buf),
			   rx_buf, sizeof(rx_buf)) < 0) {
		LOG_DBG("Failed to read sample!");
		return -EIO;
	}

	data->dx = rx_buf[1] - rx_buf[0];
	data->dy = rx_buf[2] - rx_buf[3];

	return 0;
}

static int pim447_channel_get(struct device *dev,
			      enum sensor_channel chan,
			      struct sensor_value *val)
{
	const struct pim447_data *data = dev->driver_data;

	if (chan == SENSOR_CHAN_POS_DX) {
		val->val1 = data->dx;
	} else if (chan == SENSOR_CHAN_POS_DY) {
		val->val1 = data->dy;
	} else {
		return -ENOTSUP;
	}

	return 0;
}

static int pim447_led_set(struct device *dev,
		   u8_t led_register,
		   u8_t offset,
		   const struct sensor_value *val)
{
	struct device *i2c = pim447_i2c_device(dev);
	u8_t address = pim447_i2c_address(dev);
	u8_t tx_buf[] = {
		led_register,
		(val->val1 >> offset) & 0xFF,
	};

	if (i2c_write(i2c, tx_buf, sizeof(tx_buf), address) < 0) {
		LOG_DBG("Failed to set trackball LED");
		return -EIO;
	}

	return 0;
}

int pim447_attr_set(struct device *dev,
		    enum sensor_channel chan,
		    enum sensor_attribute attr,
		    const struct sensor_value *val)
{
	struct device *i2c = pim447_i2c_device(dev);
	u8_t address = pim447_i2c_address(dev);
	enum pim447_sensor_attribute pim447_attr = (enum pim447_sensor_attribute)attr;
	if (pim447_attr == PIM447_SENSOR_ATTR_LED) {
		u8_t tx_buf[] = {
			PIM447_CMD_LED_RED,
			(val->val1 >> 24) & 0xFF,
			(val->val1 >> 16) & 0xFF,
			(val->val1 >>  8) & 0xFF,
			(val->val1 >>  0) & 0xFF,
		};

		if (i2c_write(i2c, tx_buf, sizeof(tx_buf), address) < 0) {
			LOG_DBG("Failed to set the trackball LED attributes");
			return -EIO;
		}
	} else if (pim447_attr == PIM447_SENSOR_ATTR_LED_R) {
		return pim447_led_set(dev, PIM447_CMD_LED_RED, 24, val);
	} else if (pim447_attr == PIM447_SENSOR_ATTR_LED_G) {
		return pim447_led_set(dev, PIM447_CMD_LED_GREEN, 16, val);
	} else if (pim447_attr == PIM447_SENSOR_ATTR_LED_B) {
		return pim447_led_set(dev, PIM447_CMD_LED_BLUE, 8, val);
	} else if (pim447_attr == PIM447_SENSOR_ATTR_LED_W) {
		return pim447_led_set(dev, PIM447_CMD_LED_WHITE, 0, val);
	} else {
		return -ENOTSUP;
	}

	return 0;
}

static const struct sensor_driver_api pim447_driver_api = {
#ifdef CONFIG_PIM447_TRIGGER
	.trigger_set = pim447_trigger_set,
#endif
	.attr_set = pim447_attr_set,
	.sample_fetch = pim447_sample_fetch,
	.channel_get = pim447_channel_get,
};

static u16_t pim447_version(struct device *dev)
{
	struct device *i2c = pim447_i2c_device(dev);
	u8_t address = pim447_i2c_address(dev);
	u8_t tx_buf[] = {
		PIM447_CMD_READ_CHIP_ID_LOW
	};
	u8_t rx_buf[2];

	if (i2c_write_read(i2c, address, tx_buf, sizeof(tx_buf),
			   rx_buf, sizeof(rx_buf)) < 0) {
		LOG_DBG("Failed to read chip version sample!");
		return -EIO;
	}

	/* TODO: Implement comparison of reported and expected version! */

	return 0x0000;
}


static int pim447_init(struct device *dev)
{
	struct pim447_data *data = dev->driver_data;
	const struct pim447_config *cfg = dev->config_info;
	struct device *i2c = device_get_binding(cfg->bus_name);

	if (i2c == NULL) {
		LOG_DBG("Failed to get pointer to %s device!",
			cfg->bus_name);
		return -EINVAL;
	}
	data->bus = i2c;

	if (!cfg->base_address) {
		LOG_DBG("No I2C address");
		return -EINVAL;
	}
	data->dev = dev;

	if (pim447_version(dev) != PIM447_CHIP_ID) {
		LOG_ERR("Invalid chip ID for PIM447 device at I2C address");
		return -EINVAL;
	}

#ifdef CONFIG_PIM447_TRIGGER
	if (pim447_init_interrupt(dev) < 0) {
		LOG_DBG("Failed to initialize interrupt");
		return -EIO;
	}
#endif

	return 0;
}

struct pim447_data pim4470_driver;
static const struct pim447_config pim4470_cfg = {
	.bus_name = DT_INST_BUS_LABEL(0),
#ifdef CONFIG_PIM447_TRIGGER
	.alert_gpio_name = DT_INST_GPIO_LABEL(0, alert_gpios),
#endif
	.base_address = DT_INST_REG_ADDR(0),
#ifdef CONFIG_PIM447_TRIGGER
	.alert_pin = DT_INST_GPIO_PIN(0, alert_gpios),
	.alert_flags = DT_INST_GPIO_FLAGS(0, alert_gpios),
#endif
};

DEVICE_AND_API_INIT(pim4470, DT_INST_LABEL(0),
		    pim447_init, &pim4470_driver, &pim4470_cfg,
		    POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,
		    &pim447_driver_api);
