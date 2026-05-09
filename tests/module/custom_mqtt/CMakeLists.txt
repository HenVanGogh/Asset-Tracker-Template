#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#
# NOTE: custom_mqtt.c (~3.1k lines) talks to the modem MQTT helper, TLS
# credential store, settings subsystem, and many ZBus channels (network,
# location, fota, environmental, …). Compiling it cleanly under native_sim
# requires extensive stubbing.
#
# This skeleton verifies the module's PUBLIC ZBus contract — channel and
# message struct — so that downstream changes are caught by twister.
#
# To extend into full coverage:
#   1. Add `${ATT_DIR}/app/src/modules/custom_mqtt/custom_mqtt.c` here.
#   2. Provide stubs for: mqtt_helper / mqtt_client API, modem_key_mgmt,
#      settings_save/load, dns_resolve, and any ZBus channels referenced
#      (NETWORK_CHAN, LOCATION_CHAN, ENVIRONMENTAL_CHAN, FOTA_CHAN, …).
#   3. Drive CUSTOM_MQTT_EVT_DATA_SEND messages, capture mqtt_publish
#      calls, and assert payload formatting and topic correctness.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(custom_mqtt_module_test)

set(ATT_DIR ../../..)

test_runner_generate(src/custom_mqtt_module_test.c)

target_sources(app PRIVATE src/custom_mqtt_module_test.c)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(${ATT_DIR}/app/src/common)
zephyr_include_directories(${ATT_DIR}/app/src/modules/custom_mqtt)

target_link_options(app PRIVATE --whole-archive)
