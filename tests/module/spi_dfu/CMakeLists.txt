#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(spi_dfu_module_test)

set(ATT_DIR ../../..)

zephyr_include_directories(src)
test_runner_generate(src/spi_dfu_module_test.c)

target_sources(app
  PRIVATE
  src/spi_dfu_module_test.c
  src/stubs.c
  ${ATT_DIR}/app/src/modules/spi_dfu/spi_dfu.c
)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(${ATT_DIR}/app/src/common)
zephyr_include_directories(${ATT_DIR}/app/src/modules/spi_dfu)
zephyr_include_directories(${ATT_DIR}/app/src/modules/ext_dfu)
zephyr_include_directories(${ATT_DIR}/app/src/modules/uart_sensor)

target_link_options(app PRIVATE --whole-archive)

# spi_dfu.c uses vsnprintf without including <stdio.h> explicitly.
target_compile_options(app PRIVATE -include stdio.h)

target_compile_definitions(app PRIVATE
	-DCONFIG_APP_SPI_DFU_LOG_LEVEL=4
	-DPM_NRF5340_DFU_ID=1
	-DPM_NRF5340_DFU_SIZE=262144
	# uart_sensor.h char arrays need these from its header
	-DCONFIG_APP_UART_SENSOR_PROBE_ID_MAX_LEN=32
	-DCONFIG_APP_UART_SENSOR_PROBE_ID_MIN_LEN=6
	-DCONFIG_APP_UART_SENSOR_BATTERY_MIN_MV=2700
	-DCONFIG_APP_UART_SENSOR_BATTERY_MAX_MV=4200
	-DCONFIG_APP_UART_SENSOR_RX_BUFFER_SIZE=256
	-DCONFIG_APP_UART_SENSOR_LINE_BUFFER_SIZE=128
	-DCONFIG_APP_UART_SENSOR_MSG_QUEUE_SIZE=10
	-DCONFIG_APP_UART_SENSOR_THREAD_STACK_SIZE=2048
	-DCONFIG_APP_UART_SENSOR_THREAD_PRIORITY=5
	-DCONFIG_APP_UART_SENSOR_ZBUS_TIMEOUT_MS=1000
	-DCONFIG_APP_UART_SENSOR_PROCESSING_TIMEOUT_MS=5000
	-DCONFIG_APP_UART_SENSOR_PROTOCOL_VERSION=1
	-DCONFIG_APP_UART_SENSOR_MESSAGE_DELIMITER=10
	-DCONFIG_APP_UART_SENSOR_FIELD_SEPARATOR=44
	-DCONFIG_APP_UART_SENSOR_VALUE_SEPARATOR=58
	-DCONFIG_APP_UART_SENSOR_ESL_POLL_INTERVAL_SEC=60
	-DCONFIG_APP_UART_SENSOR_ESL_MAX_TAGS=10
)
