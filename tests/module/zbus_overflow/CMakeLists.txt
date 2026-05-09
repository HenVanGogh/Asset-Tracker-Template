#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#
# ZBus overflow / saturation tests.
#
# These exercise the most common production failure modes:
#   * MSG_SUBSCRIBER net_buf pool exhaustion (publishers see -ENOMEM).
#   * SUBSCRIBER message queue overflow (oldest message dropped silently).
#   * Slow LISTENER blocking the publisher path.
#   * Multiple observers with mixed processing speeds.
#
# Run with: west twister -T tests/module/zbus_overflow

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zbus_overflow_test)

test_runner_generate(src/zbus_overflow_test.c)

target_sources(app PRIVATE src/zbus_overflow_test.c)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(../../../app/src/common)

target_link_options(app PRIVATE --whole-archive)
