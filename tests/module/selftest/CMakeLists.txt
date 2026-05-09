#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(selftest_module_test)

set(ASSET_TRACKER_TEMPLATE_DIR ../../..)

test_runner_generate(src/selftest_module_test.c)

# Test source dir must come BEFORE Zephyr include dirs so that our
# stub <nrf_modem_at.h> is picked up.
zephyr_include_directories(src)

target_sources(app
  PRIVATE
  src/selftest_module_test.c
  ${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/selftest/selftest.c
)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(${ASSET_TRACKER_TEMPLATE_DIR}/app/src/common)
zephyr_include_directories(${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/selftest)
zephyr_include_directories(${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/network)
zephyr_include_directories(${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/led)

target_link_options(app PRIVATE --whole-archive)

# selftest.c uses sscanf/snprintf but doesn't include <stdio.h> explicitly;
# the real target gets it transitively via modem headers. Force-include it.
target_compile_options(app PRIVATE -include stdio.h)

target_compile_definitions(app PRIVATE
	-DCONFIG_APP_SELFTEST_LOG_LEVEL=4
	-DCONFIG_APP_SELFTEST_SIM_WAIT_SECONDS=4
	-DCONFIG_APP_SELFTEST_LED_DURATION_SECONDS=2
	-DCONFIG_APP_SELFTEST_THREAD_STACK_SIZE=4096
	-DCONFIG_LTE_LC_PSM_MODULE=1
	-DCONFIG_LTE_LC_EDRX_MODULE=1
	-DCONFIG_LTE_LC_CONN_EVAL_MODULE=1
)
