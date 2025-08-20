#
# Copyright (c) 2025 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/uart_sensor.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/.)
