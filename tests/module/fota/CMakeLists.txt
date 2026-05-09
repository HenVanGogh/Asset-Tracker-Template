#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(fota_module_test)

test_runner_generate(src/fota_module_test.c)

set(ASSET_TRACKER_TEMPLATE_DIR ../../..)

target_sources(app
	PRIVATE
	src/fota_module_test.c
	${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/fota/fota.c
)

zephyr_include_directories(${ASSET_TRACKER_TEMPLATE_DIR}/app/src/common)
zephyr_include_directories(${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/fota)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)

zephyr_include_directories(${NRF_DIR}/include/net)

target_link_options(app PRIVATE --whole-archive)

# Options that cannot be passed through Kconfig fragments
target_compile_definitions(app PRIVATE
	-DCONFIG_APP_FOTA_LOG_LEVEL=0
	-DCONFIG_APP_FOTA_URL_MAX_LEN=256
	-DCONFIG_APP_FOTA_SEC_TAG=-1
	-DCONFIG_APP_FOTA_THREAD_STACK_SIZE=2500
	-DCONFIG_APP_FOTA_MESSAGE_QUEUE_SIZE=5
	-DCONFIG_APP_FOTA_MSG_PROCESSING_TIMEOUT_SECONDS=180
	-DCONFIG_APP_FOTA_WATCHDOG_TIMEOUT_SECONDS=210
	-DCONFIG_DOWNLOADER_MAX_HOSTNAME_SIZE=128
	-DCONFIG_DOWNLOADER_MAX_FILENAME_SIZE=256
	-DCONFIG_DOWNLOADER_TRANSPORT_PARAMS_SIZE=64
	-DCONFIG_DOWNLOADER_STACK_SIZE=4096
)
