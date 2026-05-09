/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * SPI SMP Forward — serve nRF9151 DFU via nRF5340 USB bridge.
 *
 * When a PC runs nrfutil/mcumgr over the nRF5340 CDC-ACM USB interface,
 * the nRF5340 packages each SMP request into a FRAME_SMP_FWD (0x07) SPI
 * frame and sends it to the nRF9151.  This module:
 *
 *   1. Receives the raw SMP payload from uart_sensor.c's process_rx_frame()
 *   2. Feeds it to the nRF9151's own MCUmgr SMP server
 *   3. Sends the SMP response back as a FRAME_SMP_FWD frame over SPI
 *   4. nRF5340 relays the response to the PC via USB CDC-ACM
 */

#ifndef SPI_FWD_H_
#define SPI_FWD_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Feed a raw SMP packet received via FRAME_SMP_FWD to MCUmgr.
 *
 * Called from uart_sensor.c when a type-0x07 SPI frame arrives.
 * MCUmgr processes the request asynchronously and delivers the response
 * via the registered transport output callback, which then schedules
 * uart_sensor_spi_send_smp_fwd() to return it to the nRF5340.
 *
 * @param data  Raw SMP payload (header + CBOR body).
 * @param len   Payload length in bytes.
 */
void spi_fwd_rx(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* SPI_FWD_H_ */
