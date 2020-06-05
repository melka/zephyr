# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_library_sources_ifdef(CONFIG_PIM447 pim447.c)
zephyr_library_sources_ifdef(CONFIG_PIM447_TRIGGER pim447_trigger.c)
