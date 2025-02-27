/*
 * Copyright (c) 2019 Microchip Technology Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* This file is a temporary workaround for mapping of the generated information
 * to the current driver definitions.  This will be removed when the drivers
 * are modified to handle the generated information, or the mapping of
 * generated data matches the driver definitions.
 */

#define DT_RTC_0_NAME				DT_LABEL(DT_INST(0, microchip_xec_timer))

#define DT_KSCAN_0_NAME				DT_LABEL(DT_INST(0, microchip_xec_kscan))
