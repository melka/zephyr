/*
 * Copyright (c) 2021 Pete Johanson
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT rpi_pico_spi

#define LOG_LEVEL CONFIG_SPI_LOG_LEVEL
#include <logging/log.h>
LOG_MODULE_REGISTER(spi_rpi);

#include "spi_context.h"
#include <errno.h>
#include <device.h>
#include <drivers/spi.h>

#include <drivers/pinctrl.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include <pico.h>
#include <hardware/spi.h>
#include <hardware/gpio.h>
#pragma GCC diagnostic pop

/* Device constant configuration parameters */
struct spi_rpi_pico_config {
	spi_inst_t *const spi_dev;
	uint frequency;
	const struct pinctrl_dev_config *pcfg;
};

/* Device run time data */
struct spi_rpi_pico_data {
	struct spi_context ctx;
	bool initialized;
};

static int spi_rpi_pico_configure(const struct device *dev,
			      const struct spi_config *config)
{
	const struct spi_rpi_pico_config *cfg = dev->config;
	struct spi_rpi_pico_data *data = dev->data;
	spi_inst_t *spi_dev = cfg->spi_dev;
	spi_cpol_t cpol = SPI_CPOL_0;
	spi_cpha_t cpha = SPI_CPHA_0;
	uint data_bits;
	uint rate_set;

	LOG_DBG("");

	if (data->initialized && spi_context_configured(&data->ctx, config)) {
		return 0;
	}

	if (config->operation & SPI_HALF_DUPLEX) {
		LOG_ERR("Half-duplex not supported");
		return -ENOTSUP;
	}

	if ((config->operation & SPI_TRANSFER_LSB) != 0U) {
		LOG_ERR("LSB transfer not supported");
		return -ENOTSUP;
	}

	if (SPI_OP_MODE_GET(config->operation) != SPI_OP_MODE_MASTER) {
		/* Slave mode is not implemented. */
		return -ENOTSUP;
	}


	if ((config->operation & SPI_MODE_CPOL) != 0U) {
		cpol = SPI_CPOL_1;
	}

	if ((config->operation & SPI_MODE_CPHA) != 0U) {
		cpha = SPI_CPHA_1;
	}

	data_bits = SPI_WORD_SIZE_GET(config->operation);
	if (data_bits < 4 || data_bits > 16) {
		LOG_ERR("Data bits not in a valid range: %d", data_bits);
		return -ENOTSUP;
	}


	LOG_DBG("ready to init %d", config->frequency);
	rate_set = spi_init(spi_dev, config->frequency);

	LOG_DBG("SPI initialized with rate: %d", rate_set);

	spi_set_slave(spi_dev, false);
	spi_set_format(spi_dev, data_bits, cpol, cpha, SPI_MSB_FIRST);

	data->initialized = true;
	data->ctx.config = config;

	return 0;
}

static bool spi_rpi_pico_transfer_ongoing(struct spi_rpi_pico_data *data)
{
	return spi_context_tx_on(&data->ctx) || spi_context_rx_on(&data->ctx);
}

static void spi_rpi_pico_shift_master(spi_inst_t *spi_dev, struct spi_rpi_pico_data *data)
{
	if (spi_context_tx_buf_on(&data->ctx) && spi_context_rx_buf_on(&data->ctx)) {
		size_t len = MIN(16, spi_context_max_continuous_chunk(&data->ctx));

		spi_write_read_blocking(spi_dev, data->ctx.tx_buf, data->ctx.rx_buf, len);
		spi_context_update_rx(&data->ctx, 1, len);
		spi_context_update_tx(&data->ctx, 1, len);
	} else if (spi_context_tx_buf_on(&data->ctx)) {
		size_t len = MIN(16, spi_context_max_continuous_chunk(&data->ctx));

		spi_write_blocking(spi_dev, data->ctx.tx_buf, len);
		spi_context_update_tx(&data->ctx, 1, len);
	} else if (spi_context_rx_buf_on(&data->ctx)) {
		size_t len = MIN(16, spi_context_max_continuous_chunk(&data->ctx));

		spi_read_blocking(spi_dev, 0U, data->ctx.rx_buf, len);
		spi_context_update_rx(&data->ctx, 1, len);
	} else {
		return;
	}
}

static int spi_rpi_pico_transceive_sync(const struct device *dev,
			       const struct spi_config *config,
			       const struct spi_buf_set *tx_bufs,
			       const struct spi_buf_set *rx_bufs)
{
	const struct spi_rpi_pico_config *cfg = dev->config;
	struct spi_rpi_pico_data *data = dev->data;
	spi_inst_t *spi_dev = cfg->spi_dev;
	int err;

	LOG_DBG("");

	spi_context_lock(&data->ctx, false, NULL, config);

	LOG_DBG("locked");

	err = spi_rpi_pico_configure(dev, config);
	if (err != 0) {
		goto done;
	}

	spi_context_cs_control(&data->ctx, true);

	spi_context_buffers_setup(&data->ctx, tx_bufs, rx_bufs, 1);

	do {
		spi_rpi_pico_shift_master(spi_dev, data);
	} while (spi_rpi_pico_transfer_ongoing(data));


	spi_context_cs_control(&data->ctx, false);

done:
	LOG_DBG("done");
	spi_context_release(&data->ctx, 0);
	return 0;
}

static int spi_rpi_pico_release(const struct device *dev,
			    const struct spi_config *config)
{
	struct spi_rpi_pico_data *data = dev->data;

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

static int spi_rpi_pico_init(const struct device *dev)
{
	const struct spi_rpi_pico_config *config = dev->config;
	struct spi_rpi_pico_data *data = dev->data;
	int err;

	LOG_DBG("");

	err = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	if (err < 0) {
		return err;
	}

	err = spi_context_cs_configure_all(&data->ctx);
	if (err < 0) {
		return err;
	}

	spi_context_unlock_unconditionally(&data->ctx);


	return 0;
}

static const struct spi_driver_api spi_rpi_pico_driver_api = {
	.transceive = spi_rpi_pico_transceive_sync,
	.release = spi_rpi_pico_release,
};

#define SPI_RPI_PICO_DEFINE_CONFIG(n)					\

#define SPI_RPI_PICO_DEVICE_INIT(n)						\
	PINCTRL_DT_INST_DEFINE(n);					\
	static const struct spi_rpi_pico_config spi_rpi_pico_config_##n = {		\
		.spi_dev = (spi_inst_t *)DT_INST_REG_ADDR(n),	\
		.frequency = DT_INST_PROP(n, clock_frequency),		\
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),		\
	}; \
	static struct spi_rpi_pico_data spi_rpi_pico_dev_data_##n = {		\
		SPI_CONTEXT_INIT_LOCK(spi_rpi_pico_dev_data_##n, ctx),	\
		SPI_CONTEXT_INIT_SYNC(spi_rpi_pico_dev_data_##n, ctx),	\
		SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_DRV_INST(n), ctx)	\
	};								\
	DEVICE_DT_INST_DEFINE(n, &spi_rpi_pico_init, NULL,			\
			    &spi_rpi_pico_dev_data_##n,			\
			    &spi_rpi_pico_config_##n, POST_KERNEL,		\
			    CONFIG_SPI_INIT_PRIORITY,			\
			    &spi_rpi_pico_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SPI_RPI_PICO_DEVICE_INIT)
