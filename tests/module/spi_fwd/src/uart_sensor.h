/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Local stub for uart_sensor.h. spi_fwd.c only references two functions —
 * provide minimal declarations so the module compiles in isolation.
 */

#ifndef _UART_SENSOR_H_
#define _UART_SENSOR_H_

#include <zephyr/kernel.h>

void uart_sensor_spi_submit_work(struct k_work *work);
int  uart_sensor_spi_send_smp_fwd(const uint8_t *data, uint16_t len);

#endif /* _UART_SENSOR_H_ */
