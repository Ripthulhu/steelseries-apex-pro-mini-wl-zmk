# SPDX-License-Identifier: MIT

zephyr_library()
zephyr_library_include_directories(include)
zephyr_library_sources_ifdef(CONFIG_APEX_STM32_KSCAN src/kscan_apex_stm32.c)
zephyr_library_sources_ifdef(CONFIG_APEX_RECOVERY src/recovery.c)
