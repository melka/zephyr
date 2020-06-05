/*
 * Copyright (c) 2020 Peter Johanson
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <device.h>
#include <sys/util.h>
#include <kernel.h>
#include <drivers/i2c.h>
#include <drivers/sensor.h>

#include "pim447.h"

#include <logging/log.h>
LOG_MODULE_DECLARE(PIM447, CONFIG_SENSOR_LOG_LEVEL);

static inline void setup_alert(struct device *dev,
			       bool enable)
{
	struct pim447_data *data = (struct pim447_data *)dev->driver_data;
	const struct pim447_config *cfg =
		(const struct pim447_config *)dev->config_info;
	unsigned int flags = enable
		? GPIO_INT_EDGE_TO_ACTIVE
		: GPIO_INT_DISABLE;

	gpio_pin_interrupt_configure(data->alert_gpio, cfg->alert_pin, flags);
}

static inline void handle_alert(struct device *dev)
{
	setup_alert(dev, false);

#if defined(CONFIG_PIM447_TRIGGER_OWN_THREAD)
	struct pim447_data *data = (struct pim447_data *)dev->driver_data;

	k_sem_give(&data->gpio_sem);
#elif defined(CONFIG_PIM447_TRIGGER_GLOBAL_THREAD)
	struct pim447_data *data = (struct pim447_data *)dev->driver_data;

	k_work_submit(&data->work);
#endif
}

int pim447_trigger_set(struct device *dev,
		       const struct sensor_trigger *trig,
		       sensor_trigger_handler_t handler)
{
	struct pim447_data *data = (struct pim447_data *)dev->driver_data;
	const struct pim447_config *cfg =
		(const struct pim447_config *)dev->config_info;

	setup_alert(dev, false);

	if (trig->type != SENSOR_TRIG_DATA_READY) {
		return -ENOTSUP;
	}

	data->handler = handler;
	if (handler == NULL) {
		return 0;
	}

	data->trigger = *trig;

	setup_alert(dev, true);

	/* If ALERT is active we probably won't get the rising edge,
	 * so invoke the callback manually.
	 */
	if (gpio_pin_get(data->alert_gpio, cfg->alert_pin)) {
		handle_alert(dev);
	}

	return 0;
}

static void pim447_gpio_callback(struct device *dev,
				 struct gpio_callback *cb, u32_t pins)
{
	struct pim447_data *data =
		CONTAINER_OF(cb, struct pim447_data, alert_cb);

	handle_alert(data->dev);
}

static void pim447_thread_cb(void *arg)
{
	struct device *dev = (struct device *)arg;
	struct pim447_data *data = (struct pim447_data *)dev->driver_data;

	if (data->handler != NULL) {
		data->handler(dev, &data->trigger);
	}

	setup_alert(dev, true);
}

#ifdef CONFIG_PIM447_TRIGGER_OWN_THREAD
static void pim447_thread(int dev_ptr, int unused)
{
	struct device *dev = INT_TO_POINTER(dev_ptr);
	struct pim447_data *data = dev->driver_data;

	ARG_UNUSED(unused);

	while (1) {
		k_sem_take(&data->gpio_sem, K_FOREVER);
		pim447_thread_cb(dev);
	}
}
#endif

#ifdef CONFIG_PIM447_TRIGGER_GLOBAL_THREAD
static void pim447_work_cb(struct k_work *work)
{
	struct pim447_data *data =
		CONTAINER_OF(work, struct pim447_data, work);

	pim447_thread_cb(data->dev);
}
#endif

int pim447_init_interrupt(struct device *dev)
{
	struct pim447_data *data = dev->driver_data;
	const struct pim447_config *cfg = dev->config_info;
	struct device *gpio = device_get_binding(cfg->alert_gpio_name);
	struct device *i2c = pim447_i2c_device(dev);
	u8_t address = pim447_i2c_address(dev);
	int rc;
	u8_t int_tx_rx_buf[] = { PIM447_CMD_INTERRUPT, 0x00 };

	/* setup gpio interrupt */
	if (gpio == NULL) {
		LOG_DBG("Failed to get pointer to %s device!",
			cfg->alert_gpio_name);
		return -EINVAL;
	}
	data->alert_gpio = gpio;

	rc = gpio_pin_configure(gpio, cfg->alert_pin,
				GPIO_INPUT | cfg->alert_flags);
	if (rc != 0) {
		LOG_DBG("Failed to configure alert pin %u!", cfg->alert_pin);
		return -EIO;
	}

	gpio_init_callback(&data->alert_cb, pim447_gpio_callback,
			   BIT(cfg->alert_pin));
	rc = gpio_add_callback(gpio, &data->alert_cb);
	if (rc < 0) {
		LOG_DBG("Failed to set gpio callback!");
		return -EIO;
	}

	if (i2c_write_read(i2c, address, int_tx_rx_buf, sizeof(int_tx_rx_buf) - 1, int_tx_rx_buf + 1, sizeof(int_tx_rx_buf) - 1) < 0) {
		LOG_DBG("Failed to fetch device interrupt setting");
		return -EIO;
	}

	int_tx_rx_buf[1] |= PIM447_INTERRUPT_FLAG;

	if (i2c_write(i2c, int_tx_rx_buf, sizeof(int_tx_rx_buf), address) < 0) {
		LOG_DBG("Failed to enable device interrupt setting");
		return -EIO;
	}

#if defined(CONFIG_PIM447_TRIGGER_OWN_THREAD)
	k_sem_init(&data->gpio_sem, 0, UINT_MAX);

	k_thread_create(&data->thread, data->thread_stack,
			CONFIG_PIM447_THREAD_STACK_SIZE,
			(k_thread_entry_t)pim447_thread, dev,
			0, NULL, K_PRIO_COOP(CONFIG_PIM447_THREAD_PRIORITY),
			0, K_NO_WAIT);
#elif defined(CONFIG_PIM447_TRIGGER_GLOBAL_THREAD)
	data->work.handler = pim447_work_cb;
#endif


	return 0;
}
