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


struct kscan_gpio_data {
	struct k_timer timer;
	kscan_callback_t callback;
	struct k_work work;
	bool matrix_state[MATRIX_ROWS][MATRIX_COLS];
	struct device *rows[MATRIX_ROWS];
	struct device *cols[MATRIX_COLS];
	struct device *dev;
};

struct kscan_gpio_matrix_item_config {
	char *label;
	gpio_pin_t pin;
	gpio_flags_t flags;
};

#define INIT_GPIO_ITEM_INT(node_id,p,f) \
	{ .label = DT_LABEL(node_id), .pin = p, .flags = f },

#define INIT_GPIO_ITEM(...) INIT_GPIO_ITEM_INT(__VA_ARGS__)

static const struct kscan_gpio_config {
	struct kscan_gpio_matrix_item_config rows[MATRIX_ROWS];
	struct kscan_gpio_matrix_item_config cols[MATRIX_ROWS];
} kscan_gpio_config = {
	.rows = {
		DT_FOREACH_PHA(MATRIX_NODE_ID, row_gpios, INIT_GPIO_ITEM)
	},
	.cols = {
		DT_FOREACH_PHA(MATRIX_NODE_ID, col_gpios, INIT_GPIO_ITEM)
	}
};


#if DT_ENUM_IDX(MATRIX_NODE_ID, diode_direction) == 0
static int kscan_gpio_read(struct device *dev)
{
	struct kscan_gpio_data *data = dev->driver_data;
	struct kscan_gpio_config *config = dev->config_info;

	static bool read_state[MATRIX_ROWS][MATRIX_COLS];

	for (int r = 0; r < MATRIX_ROWS; r++) {
		struct device *row_dev = data->rows[r];
		struct kscan_gpio_matrix_item_config row_cfg = config->rows[r];

		gpio_pin_set(row_dev, row_cfg.pin, 1);
		for (int c = 0; c < MATRIX_COLS; c++) {
			struct device *col_dev = data->cols[c];
			struct kscan_gpio_matrix_item_config
				col_cfg = config->cols[c];

			read_state[r][c] =
				gpio_pin_get(col_dev, col_cfg.pin) > 0;
		}

		gpio_pin_set(row_dev, row_cfg.pin, 0);
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
		struct device *col_dev = data->cols[c];
		struct kscan_gpio_matrix_item_config
			col_cfg = config->cols[c];

		gpio_pin_set(col_dev, col_cfg.pin, 1);

		for (int r = 0; r < MATRIX_ROWS; r++) {
			struct device *row_dev = data->rows[r];
			struct kscan_gpio_matrix_item_config row_cfg = config->rows[r];
			read_state[r][c] =
				gpio_pin_get(row_dev, row_cfg.pin) > 0;
		}

		gpio_pin_set(col_dev, col_cfg.pin, 0);
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
	const struct kscan_gpio_config *config = dev->config_info;

#if DT_ENUM_IDX(MATRIX_NODE_ID, diode_direction) == 0
	gpio_flags_t col_gpio_dir = GPIO_INPUT;
	gpio_flags_t row_gpio_dir = GPIO_OUTPUT_INACTIVE;
#else
	gpio_flags_t col_gpio_dir = GPIO_OUTPUT_INACTIVE;
	gpio_flags_t row_gpio_dir = GPIO_INPUT;
#endif

	for (int c = 0; c < MATRIX_COLS; c++) {
		data->cols[c] = device_get_binding(config->cols[c].label);
		if (data->cols[c] == NULL) {
			LOG_ERR("Unable to find column GPIO device\n");
			return -EINVAL;
		}
		if (gpio_pin_configure(data->cols[c], config->cols[c].pin, col_gpio_dir | config->cols[c].flags) < 0) {
			LOG_ERR("Unable to configure col GPIO pin");
			return -EINVAL;
		}
	}

	for (int r = 0; r < MATRIX_ROWS; r++) {
		data->rows[r] = device_get_binding(config->rows[r].label);
		if (data->rows[r] == NULL) {
			LOG_ERR("Unable to find row GPIO device\n");
			return -EINVAL;
		}
		if (gpio_pin_configure(data->rows[r], config->rows[r].pin, row_gpio_dir | config->rows[r].flags) < 0) {
			LOG_ERR("Unable to configure row GPIO pin");
			return -EINVAL;
		}
	}


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

static struct kscan_gpio_data kscan_gpio_data;

DEVICE_AND_API_INIT(kscan_gpio, DT_INST_LABEL(0), kscan_gpio_init,
		    &kscan_gpio_data, &kscan_gpio_config,
		    POST_KERNEL, CONFIG_KSCAN_INIT_PRIORITY,
		    &gpio_driver_api);
