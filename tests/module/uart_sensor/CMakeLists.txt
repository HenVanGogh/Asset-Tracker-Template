#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#
# NOTE: uart_sensor.c (~2.3k lines) is too tightly coupled to a real SPI
# controller, devicetree GPIOs and ESL protocol state to compile cleanly
# under native_sim without an extensive stub layer. This skeleton therefore
# does NOT compile uart_sensor.c — instead it locks down the module's PUBLIC
# ZBus contract (channel definition + message struct) so that consumers
# (cloud, custom_mqtt, etc.) keep working when this header changes.
#
# To build deeper coverage:
#   1. Add `${ATT_DIR}/app/src/modules/uart_sensor/uart_sensor.c` to
#      target_sources below.
#   2. Provide stubs for: SPI driver (struct device + spi_transceive),
#      GPIO drdy line (gpio_pin_get_dt + gpio_add_callback), settings,
#      flash_area, and the BLE/ESL helpers it talks to.
#   3. Drive process_rx_frame() with hand-crafted SPI frames and assert
#      the resulting UART_SENSOR_CHAN messages.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(uart_sensor_module_test)

set(ATT_DIR ../../..)

test_runner_generate(src/uart_sensor_module_test.c)

target_sources(app PRIVATE src/uart_sensor_module_test.c)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(${ATT_DIR}/app/src/common)
zephyr_include_directories(${ATT_DIR}/app/src/modules/uart_sensor)

target_link_options(app PRIVATE --whole-archive)

target_compile_definitions(app PRIVATE
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
	-DCONFIG_APP_UART_SENSOR_BATTERY_MIN_MV=2700
	-DCONFIG_APP_UART_SENSOR_BATTERY_MAX_MV=4200
	-DCONFIG_APP_UART_SENSOR_PROBE_ID_MIN_LEN=6
	-DCONFIG_APP_UART_SENSOR_PROBE_ID_MAX_LEN=32
	-DCONFIG_APP_UART_SENSOR_ESL_POLL_INTERVAL_SEC=60
	-DCONFIG_APP_UART_SENSOR_ESL_MAX_TAGS=10
)
