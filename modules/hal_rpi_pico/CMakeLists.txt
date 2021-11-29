# SPDX-License-Identifier: Apache-2.0

if(CONFIG_HAS_RPI_PICO)
	zephyr_library()

	set(rp2_common_dir ${ZEPHYR_HAL_RPI_PICO_MODULE_DIR}/src/rp2_common)
	set(rp2040_dir ${ZEPHYR_HAL_RPI_PICO_MODULE_DIR}/src/rp2040)
	set(common_dir ${ZEPHYR_HAL_RPI_PICO_MODULE_DIR}/src/common)
	set(boot_stage_dir ${rp2_common_dir}/boot_stage2)

	# Compile the second stage bootloader as a separate executable

	add_executable(boot_stage2)

	target_sources_ifdef(CONFIG_RP2_FLASH_W25Q080 boot_stage2 PRIVATE
				${boot_stage_dir}/boot2_w25q080.S)
	target_sources_ifdef(CONFIG_RP2_FLASH_GENERIC_03H boot_stage2 PRIVATE
				${boot_stage_dir}/boot2_generic_03h.S)
	target_sources_ifdef(CONFIG_RP2_FLASH_IS25LP080 boot_stage2 PRIVATE
				${boot_stage_dir}/boot2_is25lp080.S)
	target_sources_ifdef(CONFIG_RP2_FLASH_W25X10CL boot_stage2 PRIVATE
				${boot_stage_dir}/boot2_w25x10cl.S)

	target_include_directories(boot_stage2 PUBLIC
		${CMAKE_CURRENT_LIST_DIR}
		${boot_stage_dir}/asminclude
		${rp2_common_dir}/pico_platform/include
		${rp2040_dir}/hardware_regs/include
		${common_dir}/pico_base/include
		)

	target_link_options(boot_stage2 PRIVATE
		"-nostartfiles"
		"--specs=nosys.specs"
		"LINKER:--script=${boot_stage_dir}/boot_stage2.ld"
		)

	# Generates a binary file from the compiled bootloader
	add_custom_target(bootloader_bin ALL DEPENDS boot_stage2.bin)
	add_custom_command(OUTPUT boot_stage2.bin
		DEPENDS boot_stage2
		COMMAND ${CMAKE_OBJCOPY} -Obinary $<TARGET_FILE:boot_stage2> boot_stage2.bin
		)

	# Checksums the binary, pads it, and generates an assembly file
	add_custom_target(boot_stage2_asm ALL DEPENDS boot_stage2.S)
	add_custom_command(OUTPUT boot_stage2.S
		DEPENDS boot_stage2.bin
		COMMAND ${Python3_EXECUTABLE} ${boot_stage_dir}/pad_checksum
		-s 0xffffffff boot_stage2.bin boot_stage2.S)

	# Pico sources and headers necessary for every build.
	# These contain definitions and implementation used mostly for
	# initializing the SoC, and therefore are always required.

	zephyr_library_sources(
		boot_stage2.S
		${rp2_common_dir}/hardware_clocks/clocks.c
		${rp2_common_dir}/hardware_pll/pll.c
		${rp2_common_dir}/hardware_xosc/xosc.c
		${rp2_common_dir}/hardware_watchdog/watchdog.c
		${rp2_common_dir}/pico_platform/platform.c
	)

	zephyr_include_directories(
		${rp2_common_dir}/hardware_base/include
		${rp2_common_dir}/hardware_clocks/include
		${rp2_common_dir}/hardware_watchdog/include
		${rp2_common_dir}/hardware_xosc/include
		${rp2_common_dir}/hardware_pll/include
		${rp2_common_dir}/hardware_irq/include
		${rp2_common_dir}/hardware_sync/include
		${rp2_common_dir}/hardware_timer/include
		${rp2_common_dir}/hardware_resets/include
		${rp2040_dir}/hardware_regs/include
		${rp2040_dir}/hardware_structs/include
		${common_dir}/pico_base/include
		${rp2_common_dir}/pico_platform/include
		${CMAKE_CURRENT_LIST_DIR}
	)

	zephyr_library_sources_ifdef(CONFIG_PICOSDK_USE_GPIO
					${rp2_common_dir}/hardware_gpio/gpio.c)
	zephyr_include_directories_ifdef(CONFIG_PICOSDK_USE_GPIO
					${rp2_common_dir}/hardware_gpio/include)

	zephyr_library_sources_ifdef(CONFIG_PICOSDK_USE_UART
					${rp2_common_dir}/hardware_uart/uart.c)
	zephyr_include_directories_ifdef(CONFIG_PICOSDK_USE_UART
					${rp2_common_dir}/hardware_uart/include)
endif()
