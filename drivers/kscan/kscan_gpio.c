/*
 * Copyright (c) 2020 Peter Johanson <peter@peterjohanson.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT gpio_kscan

#include <device.h>
#include <drivers/kscan.h>
#include <drivers/gpio.h>
#include <logging/log.h>

LOG_MODULE_REGISTER(kscan_gpio, CONFIG_KSCAN_LOG_LEVEL);

#define MATRIX_NODE_ID DT_DRV_INST(0)
#define MATRIX_ROWS DT_PROP_LEN(MATRIX_NODE_ID, row_gpios)
#define MATRIX_COLS DT_PROP_LEN(MATRIX_NODE_ID, col_gpios)

struct kscan_gpio_scan_item {
	struct device *device;
	gpio_pin_t pin;
	gpio_flags_t flags;
};

struct kscan_gpio_config {
};

struct kscan_gpio_data {
	struct k_timer timer;
	kscan_callback_t callback;
	struct k_work work;
	bool matrix_state[MATRIX_ROWS][MATRIX_COLS];
	struct kscan_gpio_scan_item rows[MATRIX_ROWS];
	struct kscan_gpio_scan_item cols[MATRIX_COLS];
	struct device *dev;
};

#if DT_ENUM_IDX(MATRIX_NODE_ID, diode_direction) == 0
static int kscan_gpio_read(struct device *dev)
{
	struct kscan_gpio_data *data = dev->driver_data;

	static bool read_state[MATRIX_ROWS][MATRIX_COLS];

	for (int r = 0; r < MATRIX_ROWS; r++) {
		struct kscan_gpio_scan_item row = data->rows[r];

		gpio_pin_set(row.device, row.pin, 1);
		for (int c = 0; c < MATRIX_COLS; c++) {
			struct kscan_gpio_scan_item col = data->cols[c];

			read_state[r][c] =
				gpio_pin_get(col.device, col.pin) > 0;
		}

		gpio_pin_set(row.device, row.pin, 0);
	}

	for (int r = 0; r < MATRIX_ROWS; r++) {
		for (int c = 0; c < MATRIX_COLS; c++) {
			bool pressed = read_state[r][c];

			if (pressed != data->matrix_state[r][c]) {
				data->matrix_state[r][c] = pressed;
				data->callback(dev, r, c, pressed);
			}
		}
	}

	return 0;
}

#elif DT_ENUM_IDX(MATRIX_NODE_ID, diode_direction) == 1

static int kscan_gpio_read(struct device *dev)
{
	struct kscan_gpio_data *data = dev->driver_data;
	static bool read_state[MATRIX_ROWS][MATRIX_COLS];

	for (int c = 0; c < MATRIX_COLS; c++) {
		struct kscan_gpio_scan_item col = data->cols[c];

		gpio_pin_set(col.device, col.pin, 1);

		for (int r = 0; r < MATRIX_ROWS; r++) {
			struct kscan_gpio_scan_item row = data->rows[r];

			read_state[r][c] =
				gpio_pin_get(row.device, row.pin) > 0;
		}

		gpio_pin_set(col.device, col.pin, 0);
	}

	for (int r = 0; r < MATRIX_ROWS; r++) {
		for (int c = 0; c < MATRIX_COLS; c++) {
			bool pressed = read_state[r][c];

			if (pressed != data->matrix_state[r][c]) {
				data->matrix_state[r][c] = pressed;
				data->callback(dev, r, c, pressed);
			}
		}
	}

	return 0;
}

#endif

static void kscan_gpio_timer_handler(struct k_timer *timer)
{
	struct kscan_gpio_data *data =
		CONTAINER_OF(timer, struct kscan_gpio_data, timer);

	k_work_submit(&data->work);
}

static void kscan_gpio_work_handler(struct k_work *work)
{
	struct kscan_gpio_data *data =
		CONTAINER_OF(work, struct kscan_gpio_data, work);

	kscan_gpio_read(data->dev);
}

#define INIT_ROW_SCAN_ITEM(idx) \
	data->rows[idx].device = device_get_binding(DT_GPIO_LABEL_BY_IDX(MATRIX_NODE_ID, row_gpios, idx)); \
	if (data->rows[idx].device == NULL) { \
		LOG_ERR("Unable to find row GPIO device"); \
		return -EINVAL; \
	} \
	data->rows[idx].pin = DT_GPIO_PIN_BY_IDX(MATRIX_NODE_ID, row_gpios, idx); \
	data->rows[idx].flags = DT_GPIO_FLAGS_BY_IDX(MATRIX_NODE_ID, row_gpios, idx); \
	if (gpio_pin_configure(data->rows[idx].device, data->rows[idx].pin, row_gpio_dir | data->rows[idx].flags) < 0) { \
		LOG_ERR("Unable to configure row GPIO pin"); \
		return -EINVAL; \
	}

#define INIT_COL_SCAN_ITEM(idx) \
	data->cols[idx].device = device_get_binding(DT_GPIO_LABEL_BY_IDX(MATRIX_NODE_ID, col_gpios, idx)); \
	if (data->cols[idx].device == NULL) { \
		LOG_ERR("Unable to find column GPIO device\n"); \
		return -EINVAL; \
	} \
	data->cols[idx].pin = DT_GPIO_PIN_BY_IDX(MATRIX_NODE_ID, col_gpios, idx); \
	data->cols[idx].flags = DT_GPIO_FLAGS_BY_IDX(MATRIX_NODE_ID, col_gpios, idx); \
	if (gpio_pin_configure(data->cols[idx].device, data->cols[idx].pin, col_gpio_dir | data->cols[idx].flags) < 0) { \
		LOG_ERR("Unable to configure col GPIO pin"); \
		return -EINVAL; \
	}

static int kscan_gpio_configure(struct device *dev, kscan_callback_t callback)
{
	struct kscan_gpio_data *data = dev->driver_data;

	if (!callback) {
		return -EINVAL;
	}

	data->callback = callback;

	return 0;
}

static int kscan_gpio_enable_callback(struct device *dev)
{
	struct kscan_gpio_data *data = dev->driver_data;

	k_timer_start(&data->timer,
		      K_MSEC(CONFIG_KSCAN_GPIO_POLL_PERIOD),
		      K_MSEC(CONFIG_KSCAN_GPIO_POLL_PERIOD));

	return 0;
}

static int kscan_gpio_disable_callback(struct device *dev)
{
	struct kscan_gpio_data *data = dev->driver_data;

	k_timer_stop(&data->timer);

	return 0;
}


static int kscan_gpio_init(struct device *dev)
{
	struct kscan_gpio_data *data = dev->driver_data;

#if DT_ENUM_IDX(MATRIX_NODE_ID, diode_direction) == 0
	gpio_flags_t col_gpio_dir = GPIO_INPUT;
	gpio_flags_t row_gpio_dir = GPIO_OUTPUT_INACTIVE;
#else
	gpio_flags_t col_gpio_dir = GPIO_OUTPUT_INACTIVE;
	gpio_flags_t row_gpio_dir = GPIO_INPUT;
#endif

#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, col_gpios, 0)
	INIT_COL_SCAN_ITEM(0)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, col_gpios, 1)
	INIT_COL_SCAN_ITEM(1)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, col_gpios, 2)
	INIT_COL_SCAN_ITEM(2)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, col_gpios, 3)
	INIT_COL_SCAN_ITEM(3)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, col_gpios, 3)
	INIT_COL_SCAN_ITEM(3)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, col_gpios, 4)
	INIT_COL_SCAN_ITEM(4)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, col_gpios, 5)
	INIT_COL_SCAN_ITEM(5)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, col_gpios, 6)
	INIT_COL_SCAN_ITEM(6)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, col_gpios, 7)
	INIT_COL_SCAN_ITEM(7)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, col_gpios, 8)
	INIT_COL_SCAN_ITEM(8)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, col_gpios, 9)
	INIT_COL_SCAN_ITEM(9)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, col_gpios, 10)
	INIT_COL_SCAN_ITEM(10)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, col_gpios, 11)
	INIT_COL_SCAN_ITEM(11)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, col_gpios, 12)
	INIT_COL_SCAN_ITEM(12)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, col_gpios, 13)
	INIT_COL_SCAN_ITEM(13)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, col_gpios, 14)
	INIT_COL_SCAN_ITEM(14)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, col_gpios, 15)
	INIT_COL_SCAN_ITEM(15)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, col_gpios, 16)
	INIT_COL_SCAN_ITEM(16)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, col_gpios, 17)
	INIT_COL_SCAN_ITEM(17)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, col_gpios, 18)
	INIT_COL_SCAN_ITEM(18)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, col_gpios, 19)
	INIT_COL_SCAN_ITEM(19)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, row_gpios, 0)
	INIT_ROW_SCAN_ITEM(0)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, row_gpios, 1)
	INIT_ROW_SCAN_ITEM(1)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, row_gpios, 2)
	INIT_ROW_SCAN_ITEM(2)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, row_gpios, 3)
	INIT_ROW_SCAN_ITEM(3)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, row_gpios, 3)
	INIT_ROW_SCAN_ITEM(3)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, row_gpios, 4)
	INIT_ROW_SCAN_ITEM(4)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, row_gpios, 5)
	INIT_ROW_SCAN_ITEM(5)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, row_gpios, 6)
	INIT_ROW_SCAN_ITEM(6)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, row_gpios, 7)
	INIT_ROW_SCAN_ITEM(7)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, row_gpios, 8)
	INIT_ROW_SCAN_ITEM(8)
#endif
#if DT_PROP_HAS_IDX(MATRIX_NODE_ID, row_gpios, 9)
	INIT_ROW_SCAN_ITEM(9)
#endif

	data->dev = dev;

	k_work_init(&data->work, kscan_gpio_work_handler);
	k_timer_init(&data->timer, kscan_gpio_timer_handler, NULL);

	return 0;
}


static const struct kscan_driver_api gpio_driver_api = {
	.config = kscan_gpio_configure,
	.enable_callback = kscan_gpio_enable_callback,
	.disable_callback = kscan_gpio_disable_callback,
};

static const struct kscan_gpio_config kscan_gpio_config = {
};

static struct kscan_gpio_data kscan_gpio_data;

DEVICE_AND_API_INIT(kscan_gpio, DT_INST_LABEL(0), kscan_gpio_init,
		    &kscan_gpio_data, &kscan_gpio_config,
		    POST_KERNEL, CONFIG_KSCAN_INIT_PRIORITY,
		    &gpio_driver_api);
