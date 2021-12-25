/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 * Copyright (c) 2021 Yonatan Schachter
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief System/hardware module for Raspberry Pi RP2040 family processor
 *
 * This module provides routines to initialize and support board-level hardware
 * for the Raspberry Pi RP2040 family processor.
 */

#include <kernel.h>
#include <init.h>
#include <arch/arm/aarch32/cortex_m/cmsis.h>
#include <logging/log.h>

#include <pico/bootrom.h>

#ifdef CONFIG_RUNTIME_NMI
extern void z_arm_nmi_init(void);
#define NMI_INIT() z_arm_nmi_init()
#else
#define NMI_INIT()
#endif

LOG_MODULE_REGISTER(soc, CONFIG_SOC_LOG_LEVEL);

/* Overrides the weak ARM implementation:
   Set general purpose retention register and reboot */
void sys_arch_reboot(int type)
{
	if (type != 0) {
		reset_usb_boot(0,0);
	} else {
		NVIC_SystemReset();
	}
}

void rp2_init(void);

static int rpi_rp2040_init(const struct device *arg)
{
	uint32_t key;

	rp2_init();

	ARG_UNUSED(arg);

	key = irq_lock();

	/* Install default handler that simply resets the CPU
	 * if configured in the kernel, NOP otherwise
	 */
	NMI_INIT();

	irq_unlock(key);

	return 0;
}

SYS_INIT(rpi_rp2040_init, PRE_KERNEL_1, 0);
