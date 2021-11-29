/*
 * Copyright (c) 2021, Yonatan Schachter
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _CONFIG_AUTOGEN_H_
#define _CONFIG_AUTOGEN_H_

/* WORKAROUNDS */

/* Use Zephyr's __asm macro instead of pico-sdk's asm */
#define asm __asm

/* pico-sdk uses static assertions, which fail the compilation */
#define static_assert(...)

/**
 * pico-sdk expects __CONCAT to be defined, but we can't use
 * Zephyr's sys/cdefs.h because this file is also included in
 * assembly files. Therefore, we have to manually define __CONCAT
 * only when it isn't defined, to avoid a conflict.
 */
#ifndef __CONCAT
#define __CAT(a, b) a ## b
#define __CONCAT(a, b) __CAT(a, b)
#endif /* __CONCAT */

/* Disable binary info */
#define PICO_NO_BINARY_INFO 1

/* Zephyr compatible way of forcing inline */
#ifndef __always_inline
#define __always_inline inline __attribute__((__always_inline__))
#endif /* __always_inline */

#endif
