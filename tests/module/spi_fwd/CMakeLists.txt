#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(spi_fwd_module_test)

set(ATT_DIR ../../..)

# Test source dir is added FIRST so our local stubs override real headers.
zephyr_include_directories(src)

test_runner_generate(src/spi_fwd_module_test.c)

target_sources(app
  PRIVATE
  src/spi_fwd_module_test.c
  src/stubs.c
  ${ATT_DIR}/app/src/modules/spi_fwd/spi_fwd.c
)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(${ZEPHYR_BASE}/../modules/lib/zcbor/include)
zephyr_include_directories(${ATT_DIR}/app/src/common)
zephyr_include_directories(${ATT_DIR}/app/src/modules/spi_fwd)
zephyr_include_directories(${ATT_DIR}/app/src/modules/uart_sensor)

target_link_options(app PRIVATE --whole-archive)

target_compile_definitions(app PRIVATE
	-DCONFIG_APP_SPI_FWD_LOG_LEVEL=4
	-DCONFIG_MCUMGR_SMP_CBOR_MAX_DECODING_LEVELS=3
	-DCONFIG_MCUMGR_SMP_CBOR_MAX_ENCODING_LEVELS=3
)
