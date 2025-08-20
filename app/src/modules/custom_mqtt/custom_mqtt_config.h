/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef CUSTOM_MQTT_CONFIG_H_
#define CUSTOM_MQTT_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Production configuration constants */

/* Data validation thresholds */
#define MQTT_TEMP_MIN_CELSIUS           -50.0
#define MQTT_TEMP_MAX_CELSIUS           100.0
#define MQTT_HUMIDITY_MIN_PERCENT       0.0
#define MQTT_HUMIDITY_MAX_PERCENT       100.0
//Keep 80.0 - 120 range it is correct for our system
#define MQTT_PRESSURE_MIN_PA            80.0
#define MQTT_PRESSURE_MAX_PA            120.0
#define MQTT_BATTERY_MIN_PERCENT        0.0
#define MQTT_BATTERY_MAX_PERCENT        100.0
#define MQTT_GPS_ACCURACY_MAX_METERS    10000.0

/* Connection and retry parameters */
#define MQTT_RECONNECT_BASE_DELAY_SEC   5
#define MQTT_RECONNECT_MAX_DELAY_SEC    300
#define MQTT_MAX_PUBLISH_FAILURES       10
#define MQTT_HEARTBEAT_INTERVAL_SEC     30
#define MQTT_CONNECTION_TIMEOUT_SEC     30

/* Data precision limits (to reduce JSON size and noise) */
#define MQTT_TEMP_PRECISION_DECIMALS    2
#define MQTT_HUMIDITY_PRECISION_DECIMALS 2
#define MQTT_PRESSURE_PRECISION_DECIMALS 1
#define MQTT_BATTERY_PRECISION_DECIMALS 1
#define MQTT_GPS_PRECISION_DECIMALS     6

/* Message validation */
#define MQTT_MIN_MESSAGE_SIZE           10
#define MQTT_MAX_MESSAGE_SIZE           (MQTT_PAYLOAD_BUF_SIZE - 1)

/* Feature flags - for testing and production control */
#define MQTT_BUTTON_POWER_MEASUREMENT_ENABLED   1  /* Set to 0 to disable */

#ifdef __cplusplus
}
#endif

#endif /* CUSTOM_MQTT_CONFIG_H_ */
