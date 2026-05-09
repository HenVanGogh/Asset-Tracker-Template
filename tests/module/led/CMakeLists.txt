#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(led_module_test)

set(ASSET_TRACKER_TEMPLATE_DIR ../../..)

test_runner_generate(src/led_module_test.c)

target_sources(app
  PRIVATE
  src/led_module_test.c
  src/redef.c
  ${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/led/led.c
)

set_property(SOURCE ${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/led/led.c PROPERTY COMPILE_FLAGS
             "-include ${CMAKE_CURRENT_SOURCE_DIR}/src/redef.h")

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(../../../app/src/common)
zephyr_include_directories(../../../app/src/modules/led)

target_link_options(app PRIVATE --whole-archive)

# Force zbus.h included before runner sees led.h ZBUS_CHAN_DECLARE
target_compile_options(app PRIVATE -include zephyr/zbus/zbus.h)

target_compile_definitions(app PRIVATE
	-DCONFIG_APP_LED_LOG_LEVEL=4
)
