/*
 * Copyright (c) 2020 Peter Johanson
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_SENSOR_PIM447_PIM447_H_
#define ZEPHYR_DRIVERS_SENSOR_PIM447_PIM447_H_

#include <device.h>
#include <kernel.h>
#include <drivers/gpio.h>
#include <drivers/sensor/pim447.h>

#define PIM447_CHIP_ID                    0xBA11

#define PIM447_CMD_LED_RED                0x00
#define PIM447_CMD_LED_GREEN              0x01
#define PIM447_CMD_LED_BLUE               0x02
#define PIM447_CMD_LED_WHITE              0x03
#define PIM447_CMD_READ_LEFT              0x04
#define PIM447_CMD_READ_RIGHT             0x05
#define PIM447_CMD_READ_UP                0x06
#define PIM447_CMD_READ_DOWN              0x07
#define PIM447_CMD_READ_SWITCH            0x08


#define PIM447_CMD_READ_CHIP_ID_LOW       0xFA
#define PIM447_CMD_READ_CHIP_ID_HIGH      0xFB
#define PIM447_CMD_INTERRUPT              0xF9
#define PIM447_INTERRUPT_FLAG             0b00000010

struct pim447_config {
	char *bus_name;
#ifdef CONFIG_PIM447_TRIGGER
	char *alert_gpio_name;
#endif /* CONFIG_PIM447_TRIGGER */

	u8_t base_address;
#ifdef CONFIG_PIM447_TRIGGER
	u8_t alert_pin;
	u8_t alert_flags;
#endif /* CONFIG_PIM447_TRIGGER */
};

struct pim447_data {
	struct device *dev;
	struct device *bus;

	s32_t dx, dy;
	u32_t swtch;

#ifdef CONFIG_PIM447_TRIGGER
	struct device *alert_gpio;
	struct gpio_callback alert_cb;

	sensor_trigger_handler_t handler;
	struct sensor_trigger trigger;

#if defined(CONFIG_PIM447_TRIGGER_OWN_THREAD)
	K_THREAD_STACK_MEMBER(thread_stack, CONFIG_PIM447_THREAD_STACK_SIZE);
	struct k_sem gpio_sem;
	struct k_thread thread;
#elif defined(CONFIG_PIM447_TRIGGER_GLOBAL_THREAD)
	struct k_work work;
#endif

#endif /* CONFIG_PIM447_TRIGGER */
};

static inline u8_t pim447_i2c_address(struct device *dev)
{
	const struct pim447_config *dcp = dev->config_info;

	return dcp->base_address;
}

static inline struct device *pim447_i2c_device(struct device *dev)
{
	const struct pim447_data *ddp = dev->driver_data;

	return ddp->bus;
}


#ifdef CONFIG_PIM447_TRIGGER
int pim447_trigger_set(struct device *dev,
		       const struct sensor_trigger *trig,
		       sensor_trigger_handler_t handler);

int pim447_init_interrupt(struct device *dev);
#endif

#endif /* ZEPHYR_DRIVERS_SENSOR_PIM447_PIM447_H_ */
