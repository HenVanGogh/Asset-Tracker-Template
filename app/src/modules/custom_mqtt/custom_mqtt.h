/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef CUSTOM_MQTT_H_
#define CUSTOM_MQTT_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Custom MQTT module message types.
 */
enum custom_mqtt_msg_type {
	/** Send device data to server. */
	CUSTOM_MQTT_EVT_DATA_SEND,
	/** Connection to broker established. */
	CUSTOM_MQTT_EVT_CONNECTED,
	/** Disconnected from broker. */
	CUSTOM_MQTT_EVT_DISCONNECTED,
	/** Error in MQTT operation. */
	CUSTOM_MQTT_EVT_ERROR,
	/** Data received from server. */
	CUSTOM_MQTT_EVT_DATA_RECEIVED,
};

/**
 * @brief Custom MQTT module message.
 */
struct custom_mqtt_msg {
	enum custom_mqtt_msg_type type;
	
	union {
		/** For DATA_SEND: Data to be sent to server */
		struct {
			char *data;
			size_t len;
		} data_send;
		
		/** For DATA_RECEIVED: Data received from server */
		struct {
			char *data;
			size_t len;
		} data_received;
		
		/** For ERROR events */
		struct {
			int err_code;
		} error;
	};
};

/* Declare zbus channel for custom MQTT */
ZBUS_CHAN_DECLARE(CUSTOM_MQTT_CHAN);

#if defined(CONFIG_APP_LOCATION)
/* Forward declaration for location data */
struct location_msg;

/**
 * @brief Send location data directly to MQTT module (avoids ZBUS circular dependency)
 * 
 * @param location_data Pointer to location message data
 * @return 0 on success, negative error code on failure
 */
int custom_mqtt_send_location_data(const struct location_msg *location_data);
#endif

#ifdef __cplusplus
}
#endif

#endif /* CUSTOM_MQTT_H_ */
