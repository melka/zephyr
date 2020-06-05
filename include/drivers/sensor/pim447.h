/*
 * Copyright (c) 2020 Peter Johanson
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Extended public API for PIM447 Trackball Breakout
 *
 * Additional attributes defined for setting the trackball
 * R, G, B, and W LED values.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_SENSOR_PIM447_H_
#define ZEPHYR_INCLUDE_DRIVERS_SENSOR_PIM447_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <device.h>
#include <drivers/sensor.h>

enum pim447_sensor_attribute {
	PIM447_SENSOR_ATTR_LED = SENSOR_ATTR_PRIV_START + 1,
	PIM447_SENSOR_ATTR_LED_R = SENSOR_ATTR_PRIV_START + 2,
	PIM447_SENSOR_ATTR_LED_G = SENSOR_ATTR_PRIV_START + 3,
	PIM447_SENSOR_ATTR_LED_B = SENSOR_ATTR_PRIV_START + 4,
	PIM447_SENSOR_ATTR_LED_W = SENSOR_ATTR_PRIV_START + 5
};

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_SENSOR_PIM447_H_ */
