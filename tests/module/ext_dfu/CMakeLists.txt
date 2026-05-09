#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ext_dfu_module_test)

set(ATT_DIR ../../..)

zephyr_include_directories(src)
test_runner_generate(src/ext_dfu_module_test.c)

target_sources(app
  PRIVATE
  src/ext_dfu_module_test.c
  src/stubs.c
  ${ATT_DIR}/app/src/modules/ext_dfu/ext_dfu.c
)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(${ATT_DIR}/app/src/common)
zephyr_include_directories(${ATT_DIR}/app/src/modules/ext_dfu)

target_link_options(app PRIVATE --whole-archive)

target_compile_definitions(app PRIVATE
	-DCONFIG_APP_EXT_DFU_LOG_LEVEL=4
	-DCONFIG_DOWNLOADER_MAX_HOSTNAME_SIZE=128
	-DCONFIG_DOWNLOADER_MAX_FILENAME_SIZE=256
	-DCONFIG_DOWNLOADER_TRANSPORT_PARAMS_SIZE=64
	-DCONFIG_DOWNLOADER_STACK_SIZE=4096
	# pm_config.h IDs/sizes that ext_dfu.c references; faked by the test stubs.
	-DPM_NRF5340_DFU_ID=1
	-DPM_NRF52840_DFU_ID=2
	-DPM_NRF5340_DFU_SIZE=262144
	-DPM_NRF52840_DFU_SIZE=262144
)
