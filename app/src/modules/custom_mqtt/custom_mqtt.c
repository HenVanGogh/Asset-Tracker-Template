/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/smf.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/hostname.h>
#include <zephyr/data/json.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/settings/settings.h>
#include <cJSON.h>
#include <date_time.h>
#include <arpa/inet.h>
#include <math.h>
#include <zephyr/app_version.h>

#include "custom_mqtt.h"
#include "custom_mqtt_config.h"
#include "app_common.h"
#include "network.h"

#if defined(CONFIG_APP_LOCATION)
#include "location.h"
#include <nrf_modem_at.h>
#include <modem/location.h>
#endif

#if defined(CONFIG_APP_ENVIRONMENTAL)
#include "environmental.h"
#endif

#if defined(CONFIG_APP_POWER)
#include "power.h"
#endif

#if defined(CONFIG_APP_UART_SENSOR)
#include "uart_sensor.h"
#endif

#if defined(CONFIG_APP_BUTTON)
#include "button.h"
#endif

/* Register log module */
LOG_MODULE_REGISTER(custom_mqtt, CONFIG_APP_CUSTOM_MQTT_LOG_LEVEL);

/* MQTT client configuration — compile-time defaults used to init runtime config */
#define MQTT_BROKER_HOSTNAME_DEFAULT CONFIG_APP_CUSTOM_MQTT_BROKER_HOSTNAME
#define MQTT_BROKER_PORT_DEFAULT     CONFIG_APP_CUSTOM_MQTT_BROKER_PORT
#define MQTT_CLIENT_ID_DEFAULT       CONFIG_APP_CUSTOM_MQTT_DEVICE_ID
#define MQTT_USERNAME_DEFAULT        CONFIG_APP_CUSTOM_MQTT_USERNAME
#define MQTT_PASSWORD_DEFAULT        CONFIG_APP_CUSTOM_MQTT_PASSWORD
#define MQTT_PUB_TOPIC_DEFAULT       CONFIG_APP_CUSTOM_MQTT_PUBLISH_TOPIC
#define MQTT_SUB_TOPIC_DEFAULT       CONFIG_APP_CUSTOM_MQTT_SUBSCRIBE_TOPIC
#define MQTT_KEEPALIVE               CONFIG_APP_CUSTOM_MQTT_KEEPALIVE_SECONDS

/* Power mode enum — runtime switchable, persisted in settings */
enum power_mode {
	POWER_MODE_NORMAL = 0,
	POWER_MODE_HIGH   = 1,
};

/* Runtime-configurable MQTT parameters (loaded from flash on boot, saved on change) */
#define MQTT_RT_HOST_MAX  128
#define MQTT_RT_STR_MAX    64

static struct {
	char host[MQTT_RT_HOST_MAX];
	uint16_t port;
	char username[MQTT_RT_STR_MAX];
	char password[MQTT_RT_STR_MAX];
	char client_id[MQTT_RT_STR_MAX];
	char pub_topic[MQTT_RT_TOPIC_MAX];
	char sub_topic[MQTT_RT_TOPIC_MAX];
	bool tls_enabled;
	uint32_t sec_tag;
	enum power_mode power_mode;
} mqtt_rt_cfg;

/* Security tag — always compiled in, used when tls_enabled is true at runtime */
static sec_tag_t rt_sec_tag_list[1];

/* Convenience aliases — all broker/auth/id access goes through mqtt_rt_cfg */
#define MQTT_BROKER_HOSTNAME mqtt_rt_cfg.host
#define MQTT_BROKER_PORT     mqtt_rt_cfg.port
#define MQTT_USERNAME        mqtt_rt_cfg.username
#define MQTT_PASSWORD        mqtt_rt_cfg.password
#define MQTT_CLIENT_ID       mqtt_rt_cfg.client_id
#define MQTT_PUB_TOPIC       mqtt_rt_cfg.pub_topic
#define MQTT_SUB_TOPIC       mqtt_rt_cfg.sub_topic

/* Buffer sizes */
#define MQTT_RX_BUF_SIZE 512
#define MQTT_TX_BUF_SIZE 512
#define MQTT_PAYLOAD_BUF_SIZE CONFIG_APP_CUSTOM_MQTT_PAYLOAD_BUFFER_MAX_SIZE

/* MQTT client state machine states */
enum mqtt_state {
	MQTT_STATE_IDLE,
	MQTT_STATE_CONNECTING,
	MQTT_STATE_CONNECTED,
	MQTT_STATE_DISCONNECTING,
	MQTT_STATE_ERROR
};

/* MQTT module context */
static struct {
	struct mqtt_client client;
	struct sockaddr_storage broker_addr;
	uint8_t rx_buffer[MQTT_RX_BUF_SIZE];
	uint8_t tx_buffer[MQTT_TX_BUF_SIZE];
	uint8_t payload_buf[MQTT_PAYLOAD_BUF_SIZE];
	enum mqtt_state state;
	struct k_work_delayable connect_work;
	struct k_work_delayable data_send_work;
#if defined(CONFIG_APP_LOCATION)
	struct k_work_delayable location_trigger_work;
#endif
	bool network_connected;
	struct mqtt_utf8 username;
	struct mqtt_utf8 password;
	struct k_mutex data_mutex;
	uint32_t publish_sequence;
	uint32_t publish_failures;
	bool data_validation_enabled;
} mqtt_ctx;

/* State machine context */
static struct smf_ctx sm_ctx;

/* MQTT restart work — disconnects + reconnects asynchronously */
static struct k_work_delayable mqtt_restart_work;
static void mqtt_restart_work_fn(struct k_work *work);
static void mqtt_inactivity_work_fn(struct k_work *work);

/* Reconnect delay (module-level so it resets properly between error episodes) */
static uint32_t mqtt_reconnect_delay_sec = MQTT_RECONNECT_BASE_DELAY_SEC;

/* Inactivity watchdog — reboots if no MQTT activity for MQTT_INACTIVITY_WATCHDOG_SEC */
static struct k_work_delayable mqtt_inactivity_work;
static int64_t last_mqtt_activity_ms;    /* k_uptime_get() of last MQTT I/O */
static bool mqtt_inactive_reboot_flag;   /* set before reboot, cleared after reporting */

/* ---- Zephyr Settings handlers ---- */

/* MQTT config settings: app/mqtt/{host,port,user,pass,client_id} */
static int app_mqtt_settings_set(const char *key, size_t len,
				  settings_read_cb read_cb, void *cb_arg)
{
	int rc;

	if (strcmp(key, "host") == 0) {
		rc = read_cb(cb_arg, mqtt_rt_cfg.host, sizeof(mqtt_rt_cfg.host) - 1);
		if (rc >= 0) {
			mqtt_rt_cfg.host[rc] = '\0';
		}
	} else if (strcmp(key, "port") == 0) {
		uint16_t v;
		if (read_cb(cb_arg, &v, sizeof(v)) == sizeof(v)) {
			mqtt_rt_cfg.port = v;
		}
	} else if (strcmp(key, "user") == 0) {
		rc = read_cb(cb_arg, mqtt_rt_cfg.username, sizeof(mqtt_rt_cfg.username) - 1);
		if (rc >= 0) {
			mqtt_rt_cfg.username[rc] = '\0';
		}
	} else if (strcmp(key, "pass") == 0) {
		rc = read_cb(cb_arg, mqtt_rt_cfg.password, sizeof(mqtt_rt_cfg.password) - 1);
		if (rc >= 0) {
			mqtt_rt_cfg.password[rc] = '\0';
		}
	} else if (strcmp(key, "client_id") == 0) {
		rc = read_cb(cb_arg, mqtt_rt_cfg.client_id, sizeof(mqtt_rt_cfg.client_id) - 1);
		if (rc >= 0) {
			mqtt_rt_cfg.client_id[rc] = '\0';
		}
	} else if (strcmp(key, "pub_topic") == 0) {
		rc = read_cb(cb_arg, mqtt_rt_cfg.pub_topic, sizeof(mqtt_rt_cfg.pub_topic) - 1);
		if (rc >= 0) {
			mqtt_rt_cfg.pub_topic[rc] = '\0';
		}
	} else if (strcmp(key, "sub_topic") == 0) {
		rc = read_cb(cb_arg, mqtt_rt_cfg.sub_topic, sizeof(mqtt_rt_cfg.sub_topic) - 1);
		if (rc >= 0) {
			mqtt_rt_cfg.sub_topic[rc] = '\0';
		}
	} else if (strcmp(key, "tls") == 0) {
		uint8_t v;
		if (read_cb(cb_arg, &v, sizeof(v)) == sizeof(v)) {
			mqtt_rt_cfg.tls_enabled = (v != 0);
		}
	} else if (strcmp(key, "sec_tag") == 0) {
		uint32_t v;
		if (read_cb(cb_arg, &v, sizeof(v)) == (ssize_t)sizeof(v)) {
			mqtt_rt_cfg.sec_tag = v;
		}
	} else if (strcmp(key, "power_mode") == 0) {
		uint8_t v;
		if (read_cb(cb_arg, &v, sizeof(v)) == sizeof(v)) {
			mqtt_rt_cfg.power_mode = (v == 1) ? POWER_MODE_HIGH : POWER_MODE_NORMAL;
		}
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(app_mqtt_stg, "app/mqtt", NULL,
				app_mqtt_settings_set, NULL, NULL);

/* Sensor config settings: app/sensor/{poll,scan_retry,expected,nus_fail} */
static int app_sensor_settings_set(const char *key, size_t len,
				    settings_read_cb read_cb, void *cb_arg)
{
#if defined(CONFIG_APP_UART_SENSOR)
	uint32_t v32;
	uint8_t v8;

	if (strcmp(key, "poll") == 0) {
		if (read_cb(cb_arg, &v32, sizeof(v32)) == (ssize_t)sizeof(v32)) {
			uart_sensor_esl_set_poll_interval((int)v32);
		}
	} else if (strcmp(key, "scan_retry") == 0) {
		if (read_cb(cb_arg, &v32, sizeof(v32)) == (ssize_t)sizeof(v32)) {
			uart_sensor_esl_set_scan_retry_interval((int)v32);
		}
	} else if (strcmp(key, "expected") == 0) {
		if (read_cb(cb_arg, &v8, sizeof(v8)) == (ssize_t)sizeof(v8)) {
			/* Set directly — don't call esl_discovery_check yet (module may not be
			 * initialized). uart_sensor_esl_set_expected_tags() guards this. */
			uart_sensor_esl_set_expected_tags((int)v8);
		}
	} else if (strcmp(key, "nus_fail") == 0) {
		if (read_cb(cb_arg, &v8, sizeof(v8)) == (ssize_t)sizeof(v8)) {
			uart_sensor_esl_set_nus_failures((int)v8);
		}
	}
#endif
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(app_sensor_stg, "app/sensor", NULL,
				app_sensor_settings_set, NULL, NULL);

/* Watchdog settings: app/watchdog/mqtt_inactive */
static int app_watchdog_settings_set(const char *key, size_t len,
				      settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(key, "mqtt_inactive") == 0) {
		uint8_t v = 0;
		if (read_cb(cb_arg, &v, sizeof(v)) == (ssize_t)sizeof(v)) {
			mqtt_inactive_reboot_flag = (v != 0);
		}
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(app_watchdog_stg, "app/watchdog", NULL,
				app_watchdog_settings_set, NULL, NULL);

/* Security tag for TLS — compile-time default, overridden by runtime config */
#if defined(CONFIG_APP_CUSTOM_MQTT_USE_TLS)
static sec_tag_t sec_tag_list[] = { CONFIG_APP_CUSTOM_MQTT_SEC_TAG };
#endif

/* Declare external ZBUS channels defined in other modules */
ZBUS_CHAN_DECLARE(TIMER_CHAN);

/* Register zbus subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(custom_mqtt_subscriber);

/* Subscribe to channels */
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, custom_mqtt_subscriber, 0);
#if defined(CONFIG_APP_ENVIRONMENTAL)
ZBUS_CHAN_ADD_OBS(ENVIRONMENTAL_CHAN, custom_mqtt_subscriber, 0);
#endif
#if defined(CONFIG_APP_POWER)
ZBUS_CHAN_ADD_OBS(POWER_CHAN, custom_mqtt_subscriber, 0);
#endif
#if defined(CONFIG_APP_UART_SENSOR)
ZBUS_CHAN_ADD_OBS(UART_SENSOR_CHAN, custom_mqtt_subscriber, 0);
#endif
#if defined(CONFIG_APP_BUTTON)
ZBUS_CHAN_ADD_OBS(BUTTON_CHAN, custom_mqtt_subscriber, 0);
#endif

/* Define zbus channel */
ZBUS_CHAN_DEFINE(CUSTOM_MQTT_CHAN,
		 struct custom_mqtt_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.type = CUSTOM_MQTT_EVT_DISCONNECTED));

/* Forward declarations */
static void mqtt_evt_handler(struct mqtt_client *const client,
			      const struct mqtt_evt *evt);
static void connect_work_handler(struct k_work *work);
static void data_send_work_handler(struct k_work *work);
static void location_trigger_work_handler(struct k_work *work);
static int custom_mqtt_connect(void);
static int custom_mqtt_disconnect(void);
static int mqtt_publish_data(const char *data, size_t len);

/* Data validation helpers */
static bool validate_sensor_data(double value, double min, double max);
static int safe_publish_json(cJSON *json, const char *data_type);

/* Location trigger functionality */
#if defined(CONFIG_APP_LOCATION)
static void trigger_location_request(void);
#endif

/* Message processing functions */
static void process_network_msg(const struct network_msg *msg);
#if defined(CONFIG_APP_LOCATION)
static void process_location_data(const struct location_msg *msg);
#endif
#if defined(CONFIG_APP_ENVIRONMENTAL)
static void process_environmental_data(const struct environmental_msg *msg);
#endif
#if defined(CONFIG_APP_POWER)
static void process_power_data(const struct power_msg *msg);
#endif
#if defined(CONFIG_APP_UART_SENSOR)
static void process_uart_sensor_data(const struct uart_sensor_msg *msg);
static bool uart_debug_echo_active; /* Runtime toggle: publish raw UART RX to MQTT */
#endif
#if defined(CONFIG_APP_BUTTON)
static void process_button_msg(const struct button_msg *msg);
#endif

/* State machine forward declarations */
static void idle_entry(void *obj);
static void idle_run(void *obj);
static void connecting_entry(void *obj);
static void connecting_run(void *obj);
static void connected_entry(void *obj);
static void connected_run(void *obj);
static void disconnecting_entry(void *obj);
static void disconnecting_run(void *obj);
static void error_entry(void *obj);
static void error_run(void *obj);

/* State machine states */
static const struct smf_state mqtt_states[] = {
	[MQTT_STATE_IDLE] = SMF_CREATE_STATE(idle_entry, idle_run, NULL, NULL, NULL),
	[MQTT_STATE_CONNECTING] = SMF_CREATE_STATE(connecting_entry, connecting_run, NULL, NULL, NULL),
	[MQTT_STATE_CONNECTED] = SMF_CREATE_STATE(connected_entry, connected_run, NULL, NULL, NULL),
	[MQTT_STATE_DISCONNECTING] = SMF_CREATE_STATE(disconnecting_entry, disconnecting_run, NULL, NULL, NULL),
	[MQTT_STATE_ERROR] = SMF_CREATE_STATE(error_entry, error_run, NULL, NULL, NULL),
};

static void mqtt_evt_handler(struct mqtt_client *const client,
			      const struct mqtt_evt *evt)
{
	struct custom_mqtt_msg msg = {0};

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result == 0) {
			LOG_INF("MQTT client connected");
			last_mqtt_activity_ms = k_uptime_get();
			mqtt_ctx.state = MQTT_STATE_CONNECTED;
			msg.type = CUSTOM_MQTT_EVT_CONNECTED;
			zbus_chan_pub(&CUSTOM_MQTT_CHAN, &msg, K_NO_WAIT);
		} else {
			LOG_ERR("MQTT connection failed: %d", evt->result);
			mqtt_ctx.state = MQTT_STATE_ERROR;
			msg.type = CUSTOM_MQTT_EVT_ERROR;
			msg.error.err_code = evt->result;
			zbus_chan_pub(&CUSTOM_MQTT_CHAN, &msg, K_NO_WAIT);
		}
		break;

	case MQTT_EVT_DISCONNECT:
		LOG_INF("MQTT client disconnected");
		mqtt_ctx.state = MQTT_STATE_IDLE;
		msg.type = CUSTOM_MQTT_EVT_DISCONNECTED;
		zbus_chan_pub(&CUSTOM_MQTT_CHAN, &msg, K_NO_WAIT);
		break;

	case MQTT_EVT_PUBLISH:
		last_mqtt_activity_ms = k_uptime_get();
		LOG_INF(">>> MQTT_EVT_PUBLISH received! <<<");
		LOG_INF("MQTT message received on topic: %.*s",
			evt->param.publish.message.topic.topic.size,
			evt->param.publish.message.topic.topic.utf8);
		LOG_INF("Message ID: %u, QoS: %d, Payload len: %u",
			evt->param.publish.message_id,
			evt->param.publish.message.topic.qos,
			evt->param.publish.message.payload.len);
		
		/* Handle payload based on QoS level */
		size_t len = 0;
		int ret = 0;
		
		/* For all QoS levels, try to read from socket if payload length > 0 */
		if (evt->param.publish.message.payload.len > 0) {
			LOG_INF("Reading payload (%u bytes) from socket...", 
				evt->param.publish.message.payload.len);
			
			/* Try blocking read first */
			ret = mqtt_read_publish_payload_blocking(client, mqtt_ctx.payload_buf, 
								 MQTT_PAYLOAD_BUF_SIZE - 1);
			LOG_INF("mqtt_read_publish_payload_blocking returned: %d", ret);
			
			if (ret >= 0) {
				len = ret;
				mqtt_ctx.payload_buf[len] = '\0';
				LOG_INF("Successfully read %zu bytes from socket", len);
			} else if (evt->param.publish.message.payload.data) {
				/* Fallback: try to read from event structure (QoS 0 sometimes provides this) */
				LOG_WRN("Socket read failed (%d), trying event structure", ret);
				len = MIN(evt->param.publish.message.payload.len, MQTT_PAYLOAD_BUF_SIZE - 1);
				memcpy(mqtt_ctx.payload_buf, evt->param.publish.message.payload.data, len);
				mqtt_ctx.payload_buf[len] = '\0';
				LOG_INF("Read %zu bytes from event structure", len);
			} else {
				LOG_ERR("Failed to read payload - socket error %d and no event data", ret);
			}
		}
		
		/* Send acknowledgment for QoS 1/2 messages AFTER reading payload */
		if (evt->param.publish.message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
			struct mqtt_puback_param param = {
				.message_id = evt->param.publish.message_id
			};
			ret = mqtt_publish_qos1_ack(client, &param);
			if (ret) {
				LOG_ERR("Failed to send PUBACK: %d", ret);
			} else {
				LOG_INF("Sent PUBACK for message_id %u", param.message_id);
			}
		} else if (evt->param.publish.message.topic.qos == MQTT_QOS_2_EXACTLY_ONCE) {
			LOG_WRN("QoS 2 not fully implemented");
		}
		
		if (len > 0) {
			LOG_INF("Received message (%zu bytes): %s", len, (char *)mqtt_ctx.payload_buf);

			/* Process command and send response */
			cJSON *response = cJSON_CreateObject();
			if (response) {
				cJSON_AddStringToObject(response, "device_id", MQTT_CLIENT_ID);
				cJSON_AddNumberToObject(response, "timestamp", k_uptime_get());
				cJSON_AddStringToObject(response, "received_message", (char *)mqtt_ctx.payload_buf);
				cJSON_AddNumberToObject(response, "response_sequence", mqtt_ctx.publish_sequence + 1);
				
				/* Parse command if it's JSON */
				cJSON *received_json = cJSON_Parse((char *)mqtt_ctx.payload_buf);
				if (received_json) {
					/* Check for "type": "uart_passthrough" format first */
					cJSON *type = cJSON_GetObjectItem(received_json, "type");
					cJSON *command = cJSON_GetObjectItem(received_json, "command");
					if (!command) {
						command = cJSON_GetObjectItem(received_json, "cmd");
					}
					
					if (type && cJSON_IsString(type) && 
					    strcmp(type->valuestring, "uart_passthrough") == 0 &&
					    command && cJSON_IsString(command)) {
						/* Handle uart_passthrough: validate and forward as ESL command */
						const char *cmd = command->valuestring;
						LOG_DBG("UART passthrough request: %s", cmd);

						/* Route through uart_sensor_esl_command() which auto-prefixes esl_c */
#if defined(CONFIG_APP_UART_SENSOR)
						int ret = uart_sensor_esl_command(cmd);
						if (ret != 0) {
							LOG_WRN("Failed to send ESL command: %s (err %d)", cmd, ret);
						}
#else
						LOG_WRN("UART passthrough not available — UART sensor disabled");
#endif
						/* Clean up and skip publishing response */
						cJSON_Delete(received_json);
						cJSON_Delete(response);
						goto publish_skip;
					} else if (command && cJSON_IsString(command)) {
						LOG_INF("Processing command: %s", command->valuestring);
						
						if (strcmp(command->valuestring, "get_location") == 0) {
							LOG_INF("Location request received via MQTT");
#if defined(CONFIG_APP_LOCATION)
							trigger_location_request();
							cJSON_AddStringToObject(response, "status", "location_requested");
#else
							LOG_WRN("Location module not enabled");
							cJSON_AddStringToObject(response, "status", "location_not_available");
#endif
						} else if (strcmp(command->valuestring, "get_status") == 0) {
							LOG_INF("Status request received via MQTT");
							
							/* Return gateway status only - no UART forwarding */
							cJSON_AddStringToObject(response, "status", "online");
							cJSON_AddNumberToObject(response, "uptime_ms", k_uptime_get());
							cJSON_AddNumberToObject(response, "mqtt_state", mqtt_ctx.state);
							cJSON_AddBoolToObject(response, "network_connected", mqtt_ctx.network_connected);
						} else if (strcmp(command->valuestring, "uart_command") == 0) {
							cJSON *args = cJSON_GetObjectItem(received_json, "args");
							if (args && cJSON_IsString(args)) {
								LOG_INF("UART command via MQTT: %s", args->valuestring);
#if defined(CONFIG_APP_UART_SENSOR)
								/* Route through esl_command() for proper esl_c prefix */
								uart_sensor_esl_command(args->valuestring);
								cJSON_AddStringToObject(response, "status", "esl_command_sent");
#else
								cJSON_AddStringToObject(response, "status", "uart_not_available");
#endif
							} else {
								cJSON_AddStringToObject(response, "status", "missing_args");
							}
#if defined(CONFIG_APP_UART_SENSOR)
						/* ---- ESL BLE Management Commands ---- */
						} else if (strcmp(command->valuestring, "esl_scan") == 0) {
							LOG_INF("ESL scan command via MQTT");
							cJSON *action = cJSON_GetObjectItem(received_json, "action");
							if (action && cJSON_IsString(action) &&
							    strcmp(action->valuestring, "stop") == 0) {
								uart_sensor_esl_scan_stop();
								cJSON_AddStringToObject(response, "status", "esl_scan_stopped");
							} else {
								uart_sensor_esl_scan_start();
								cJSON_AddStringToObject(response, "status", "esl_scan_started");
							}

						} else if (strcmp(command->valuestring, "esl_list_tags") == 0) {
							LOG_INF("ESL list tags via MQTT");
							int count = uart_sensor_esl_get_tag_count();
							cJSON_AddStringToObject(response, "status", "ok");
							cJSON_AddNumberToObject(response, "tag_count", count);
							/* Request fresh data from AP */
							uart_sensor_esl_command("acl list");

						} else if (strcmp(command->valuestring, "esl_nus_status") == 0) {
							cJSON *id_json = cJSON_GetObjectItem(received_json, "id");
							if (id_json && cJSON_IsNumber(id_json)) {
								uint16_t id = (uint16_t)id_json->valueint;
								uart_sensor_esl_nus_status(id);
								cJSON_AddStringToObject(response, "status", "nus_status_requested");
								cJSON_AddNumberToObject(response, "esl_id", id);
							} else {
								cJSON_AddStringToObject(response, "status", "missing_id");
							}

						} else if (strcmp(command->valuestring, "esl_nus_sensors") == 0) {
							cJSON *id_json = cJSON_GetObjectItem(received_json, "id");
							if (id_json && cJSON_IsNumber(id_json)) {
								uint16_t id = (uint16_t)id_json->valueint;
								uart_sensor_esl_nus_sensors(id);
								cJSON_AddStringToObject(response, "status", "nus_sensors_requested");
								cJSON_AddNumberToObject(response, "esl_id", id);
							} else {
								cJSON_AddStringToObject(response, "status", "missing_id");
							}

						} else if (strcmp(command->valuestring, "esl_poll") == 0) {
							cJSON *action = cJSON_GetObjectItem(received_json, "action");
							if (action && cJSON_IsString(action) &&
							    strcmp(action->valuestring, "stop") == 0) {
								uart_sensor_esl_poll_stop();
								cJSON_AddStringToObject(response, "status", "esl_poll_stopped");
							} else {
								uart_sensor_esl_poll_start();
								cJSON_AddStringToObject(response, "status", "esl_poll_started");
							}

						} else if (strcmp(command->valuestring, "esl_command") == 0) {
							cJSON *args = cJSON_GetObjectItem(received_json, "args");
							if (args && cJSON_IsString(args)) {
								LOG_INF("Raw ESL command: %s", args->valuestring);
								uart_sensor_esl_command(args->valuestring);
								cJSON_AddStringToObject(response, "status", "esl_command_sent");
							} else {
								cJSON_AddStringToObject(response, "status", "missing_args");
							}

						} else if (strcmp(command->valuestring, "esl_raw") == 0) {
							/* Simpler raw ESL command: {"command":"esl_raw","cmd":"reset_ap"} */
							cJSON *raw_cmd = cJSON_GetObjectItem(received_json, "cmd");
							if (!raw_cmd) {
								raw_cmd = cJSON_GetObjectItem(received_json, "args");
							}
							if (raw_cmd && cJSON_IsString(raw_cmd)) {
								LOG_INF("esl_raw: forwarding '%s'", raw_cmd->valuestring);
								uart_sensor_esl_command(raw_cmd->valuestring);
								cJSON_AddStringToObject(response, "status", "esl_raw_sent");
								cJSON_AddStringToObject(response, "cmd_sent", raw_cmd->valuestring);
							} else {
								cJSON_AddStringToObject(response, "status", "missing_cmd");
								cJSON_AddStringToObject(response, "hint",
									"{\"command\":\"esl_raw\",\"cmd\":\"reset_ap\"}");
							}

						} else if (strcmp(command->valuestring, "uart_debug_echo") == 0) {
							/* Toggle UART debug echo — off by default, not for production.
							 * When on, ALL unrecognized UART RX lines are published to MQTT
							 * as {"type":"uart_debug","line":"..."}. */
							cJSON *action = cJSON_GetObjectItem(received_json, "action");
							bool enable = true;
							if (action && cJSON_IsString(action) &&
							    strcmp(action->valuestring, "stop") == 0) {
								enable = false;
							}
							uart_debug_echo_active = enable;
							uart_sensor_set_debug_echo(enable);
							LOG_INF("UART debug echo %s via MQTT",
								enable ? "ENABLED" : "disabled");
							cJSON_AddStringToObject(response, "status",
								enable ? "uart_debug_echo_enabled"
								       : "uart_debug_echo_disabled");
							cJSON_AddBoolToObject(response, "uart_debug_echo_active", enable);

						} else if (strcmp(command->valuestring, "esl_status") == 0) {
							LOG_INF("ESL AP status via MQTT");
							cJSON_AddStringToObject(response, "status", "ok");
							cJSON_AddNumberToObject(response, "tag_count",
								uart_sensor_esl_get_tag_count());
							cJSON_AddNumberToObject(response, "uptime_ms", k_uptime_get());
							/* Also ask the AP for its status */
							uart_sensor_esl_command("acl list");

						} else if (strcmp(command->valuestring, "esl_nus_reset") == 0) {
							cJSON *id_json = cJSON_GetObjectItem(received_json, "id");
							if (id_json && cJSON_IsNumber(id_json)) {
								char cmd[32];
								snprintf(cmd, sizeof(cmd), "nus reset %d", id_json->valueint);
								uart_sensor_esl_command(cmd);
								cJSON_AddStringToObject(response, "status", "nus_reset_sent");
								cJSON_AddNumberToObject(response, "esl_id", id_json->valueint);
							} else {
								cJSON_AddStringToObject(response, "status", "missing_id");
							}

						} else if (strcmp(command->valuestring, "esl_nus_led") == 0) {
							cJSON *id_json = cJSON_GetObjectItem(received_json, "id");
							if (id_json && cJSON_IsNumber(id_json)) {
								char cmd[32];
								snprintf(cmd, sizeof(cmd), "nus led %d", id_json->valueint);
								uart_sensor_esl_command(cmd);
								cJSON_AddStringToObject(response, "status", "nus_led_sent");
								cJSON_AddNumberToObject(response, "esl_id", id_json->valueint);
							} else {
								cJSON_AddStringToObject(response, "status", "missing_id");
							}

						} else if (strcmp(command->valuestring, "esl_get_tags") == 0) {
							LOG_INF("ESL get tags info via MQTT");
							int tc = uart_sensor_esl_get_tag_count();
							cJSON_AddStringToObject(response, "status", "ok");
							cJSON_AddNumberToObject(response, "tag_count", tc);
							cJSON *tags_arr = cJSON_AddArrayToObject(response, "tags");
							if (tags_arr) {
								for (int ti = 0; ti < tc; ti++) {
									struct esl_tag_info tinfo;
									if (uart_sensor_esl_get_tag_info((uint16_t)ti, &tinfo) == 0) {
										cJSON *t = cJSON_CreateObject();
										if (t) {
											char eid[16];
											snprintf(eid, sizeof(eid), "ESL_0x%04X", tinfo.esl_addr);
											cJSON_AddStringToObject(t, "esl_id", eid);
											cJSON_AddStringToObject(t, "mac", tinfo.mac);
											cJSON_AddNumberToObject(t, "battery_mv", tinfo.battery_mv);
											cJSON_AddNumberToObject(t, "uptime_s", tinfo.uptime_s);
											cJSON_AddNumberToObject(t, "temperature", tinfo.temperature);
											cJSON_AddNumberToObject(t, "flags", tinfo.flags);
											cJSON_AddBoolToObject(t, "connected", tinfo.connected);
											cJSON_AddItemToArray(tags_arr, t);
										}
									}
								}
							}

						} else if (strcmp(command->valuestring, "esl_set_expected_tags") == 0) {
							cJSON *cnt = cJSON_GetObjectItem(received_json, "count");
							if (cnt && cJSON_IsNumber(cnt)) {
								int ret = uart_sensor_esl_set_expected_tags(cnt->valueint);
								if (ret == 0) {
									cJSON_AddStringToObject(response, "status", "ok");
									cJSON_AddNumberToObject(response, "expected_tags", cnt->valueint);
									uint8_t v8 = (uint8_t)cnt->valueint;
									settings_save_one("app/sensor/expected", &v8, sizeof(v8));
									LOG_INF("Expected ESL tags set to %d via MQTT", cnt->valueint);
								} else {
									cJSON_AddStringToObject(response, "status", "invalid_count");
								}
							} else {
								cJSON_AddStringToObject(response, "status", "missing_count");
								cJSON_AddStringToObject(response, "hint",
									"{\"command\":\"esl_set_expected_tags\",\"count\":2}");
							}

						} else if (strcmp(command->valuestring, "sensor_get_config") == 0) {
							cJSON_AddStringToObject(response, "status", "ok");
							cJSON_AddNumberToObject(response, "poll_interval_s",
								uart_sensor_esl_get_poll_interval());
							cJSON_AddNumberToObject(response, "scan_retry_interval_s",
								uart_sensor_esl_get_scan_retry_interval());
							cJSON_AddNumberToObject(response, "expected_tags",
								uart_sensor_esl_get_expected_tags());
							cJSON_AddNumberToObject(response, "nus_max_failures",
								uart_sensor_esl_get_nus_failures());

						} else if (strcmp(command->valuestring, "sensor_set_poll_interval") == 0) {
							cJSON *v = cJSON_GetObjectItem(received_json, "interval_s");
							if (v && cJSON_IsNumber(v)) {
								int ret = uart_sensor_esl_set_poll_interval(v->valueint);
								if (ret == 0) {
									uint32_t val = (uint32_t)v->valueint;
									settings_save_one("app/sensor/poll", &val, sizeof(val));
									cJSON_AddStringToObject(response, "status", "ok");
									cJSON_AddNumberToObject(response, "poll_interval_s", v->valueint);
								} else {
									cJSON_AddStringToObject(response, "status", "invalid_value");
									cJSON_AddStringToObject(response, "hint", "Range: 10-86400 s");
								}
							} else {
								cJSON_AddStringToObject(response, "status", "missing_interval_s");
								cJSON_AddStringToObject(response, "hint",
									"{\"command\":\"sensor_set_poll_interval\",\"interval_s\":600}");
							}

						} else if (strcmp(command->valuestring, "sensor_set_scan_retry_interval") == 0) {
							cJSON *v = cJSON_GetObjectItem(received_json, "interval_s");
							if (v && cJSON_IsNumber(v)) {
								int ret = uart_sensor_esl_set_scan_retry_interval(v->valueint);
								if (ret == 0) {
									uint32_t val = (uint32_t)v->valueint;
									settings_save_one("app/sensor/scan_retry", &val, sizeof(val));
									cJSON_AddStringToObject(response, "status", "ok");
									cJSON_AddNumberToObject(response, "scan_retry_interval_s", v->valueint);
								} else {
									cJSON_AddStringToObject(response, "status", "invalid_value");
								}
							} else {
								cJSON_AddStringToObject(response, "status", "missing_interval_s");
								cJSON_AddStringToObject(response, "hint",
									"{\"command\":\"sensor_set_scan_retry_interval\",\"interval_s\":60}");
							}

						} else if (strcmp(command->valuestring, "sensor_set_nus_failures") == 0) {
							cJSON *v = cJSON_GetObjectItem(received_json, "count");
							if (v && cJSON_IsNumber(v)) {
								int ret = uart_sensor_esl_set_nus_failures(v->valueint);
								if (ret == 0) {
									uint8_t val = (uint8_t)v->valueint;
									settings_save_one("app/sensor/nus_fail", &val, sizeof(val));
									cJSON_AddStringToObject(response, "status", "ok");
									cJSON_AddNumberToObject(response, "nus_max_failures", v->valueint);
								} else {
									cJSON_AddStringToObject(response, "status", "invalid_value");
									cJSON_AddStringToObject(response, "hint", "Range: 1-20");
								}
							} else {
								cJSON_AddStringToObject(response, "status", "missing_count");
								cJSON_AddStringToObject(response, "hint",
									"{\"command\":\"sensor_set_nus_failures\",\"count\":3}");
							}

						} else if (strcmp(command->valuestring, "esl_get_name") == 0) {
							/* Request human-readable name from one or all ESL tags.
							 * Optional "esl_id": numeric ESL address (decimal).
							 * Omit or set to 65535 to query all known tags.
							 * Response arrives as sensor_name MQTT message. */
							cJSON *id_j = cJSON_GetObjectItem(received_json, "esl_id");
							uint16_t target = 0xFFFF; /* default: all */
							if (id_j && cJSON_IsNumber(id_j)) {
								target = (uint16_t)id_j->valueint;
							}
							int ret = uart_sensor_esl_get_name(target);
							if (ret == 0) {
								cJSON_AddStringToObject(response, "status", "get_name_sent");
								if (target == 0xFFFF) {
									cJSON_AddStringToObject(response, "target", "all");
								} else {
									cJSON_AddNumberToObject(response, "esl_id", target);
								}
							} else {
								cJSON_AddStringToObject(response, "status", "error");
								cJSON_AddNumberToObject(response, "error_code", ret);
							}

						} else if (strcmp(command->valuestring, "ap_set_time") == 0) {
							/* Force-push current LTE UTC time to the nRF5340 AP.
							 * Normally happens automatically on boot; use this to
							 * re-sync after an AP reboot or manual override. */
							int ret = uart_sensor_esl_set_ap_time();
							if (ret == 0) {
								cJSON_AddStringToObject(response, "status", "ap_set_time_sent");
							} else {
								cJSON_AddStringToObject(response, "status", "error");
								cJSON_AddStringToObject(response, "hint",
									"LTE time may not be available yet");
								cJSON_AddNumberToObject(response, "error_code", ret);
							}

#endif /* CONFIG_APP_UART_SENSOR */

						} else if (strcmp(command->valuestring, "mqtt_get_config") == 0) {
							cJSON_AddStringToObject(response, "status", "ok");
							cJSON_AddStringToObject(response, "host", mqtt_rt_cfg.host);
							cJSON_AddNumberToObject(response, "port", mqtt_rt_cfg.port);
							cJSON_AddStringToObject(response, "username", mqtt_rt_cfg.username);
							cJSON_AddStringToObject(response, "client_id", mqtt_rt_cfg.client_id);
							/* password intentionally omitted */
							cJSON_AddStringToObject(response, "pub_topic", mqtt_rt_cfg.pub_topic);
							cJSON_AddStringToObject(response, "sub_topic", mqtt_rt_cfg.sub_topic);
							cJSON_AddBoolToObject(response, "tls_enabled", mqtt_rt_cfg.tls_enabled);
							cJSON_AddNumberToObject(response, "sec_tag", mqtt_rt_cfg.sec_tag);
							cJSON_AddStringToObject(response, "power_mode",
								mqtt_rt_cfg.power_mode == POWER_MODE_HIGH ? "high" : "normal");
							cJSON_AddStringToObject(response, "version", APP_VERSION_STRING);

						} else if (strcmp(command->valuestring, "mqtt_set_broker") == 0) {
							cJSON *host_j = cJSON_GetObjectItem(received_json, "host");
							cJSON *port_j = cJSON_GetObjectItem(received_json, "port");
							bool changed = false;
							if (host_j && cJSON_IsString(host_j) && strlen(host_j->valuestring) > 0) {
								strncpy(mqtt_rt_cfg.host, host_j->valuestring,
									sizeof(mqtt_rt_cfg.host) - 1);
								settings_save_one("app/mqtt/host", mqtt_rt_cfg.host,
										  strlen(mqtt_rt_cfg.host));
								changed = true;
							}
							if (port_j && cJSON_IsNumber(port_j)) {
								mqtt_rt_cfg.port = (uint16_t)port_j->valueint;
								settings_save_one("app/mqtt/port", &mqtt_rt_cfg.port,
										  sizeof(mqtt_rt_cfg.port));
								changed = true;
							}
							if (changed) {
								cJSON_AddStringToObject(response, "status", "saved");
								cJSON_AddStringToObject(response, "host", mqtt_rt_cfg.host);
								cJSON_AddNumberToObject(response, "port", mqtt_rt_cfg.port);
								cJSON_AddStringToObject(response, "note",
									"Send mqtt_restart to reconnect with new broker");
							} else {
								cJSON_AddStringToObject(response, "status", "no_changes");
								cJSON_AddStringToObject(response, "hint",
									"{\"command\":\"mqtt_set_broker\",\"host\":\"192.168.1.1\",\"port\":1883}");
							}

						} else if (strcmp(command->valuestring, "mqtt_set_auth") == 0) {
							cJSON *user_j = cJSON_GetObjectItem(received_json, "username");
							cJSON *pass_j = cJSON_GetObjectItem(received_json, "password");
							bool changed = false;
							if (user_j && cJSON_IsString(user_j)) {
								strncpy(mqtt_rt_cfg.username, user_j->valuestring,
									sizeof(mqtt_rt_cfg.username) - 1);
								settings_save_one("app/mqtt/user", mqtt_rt_cfg.username,
										  strlen(mqtt_rt_cfg.username));
								changed = true;
							}
							if (pass_j && cJSON_IsString(pass_j)) {
								strncpy(mqtt_rt_cfg.password, pass_j->valuestring,
									sizeof(mqtt_rt_cfg.password) - 1);
								settings_save_one("app/mqtt/pass", mqtt_rt_cfg.password,
										  strlen(mqtt_rt_cfg.password));
								changed = true;
							}
							if (changed) {
								cJSON_AddStringToObject(response, "status", "saved");
								cJSON_AddStringToObject(response, "note",
									"Send mqtt_restart to reconnect with new credentials");
							} else {
								cJSON_AddStringToObject(response, "status", "no_changes");
								cJSON_AddStringToObject(response, "hint",
									"{\"command\":\"mqtt_set_auth\",\"username\":\"user\",\"password\":\"pass\"}");
							}

						} else if (strcmp(command->valuestring, "mqtt_set_client_id") == 0) {
							cJSON *id_j = cJSON_GetObjectItem(received_json, "id");
							if (id_j && cJSON_IsString(id_j) && strlen(id_j->valuestring) > 0) {
								strncpy(mqtt_rt_cfg.client_id, id_j->valuestring,
									sizeof(mqtt_rt_cfg.client_id) - 1);
								settings_save_one("app/mqtt/client_id", mqtt_rt_cfg.client_id,
										  strlen(mqtt_rt_cfg.client_id));
								cJSON_AddStringToObject(response, "status", "saved");
								cJSON_AddStringToObject(response, "client_id", mqtt_rt_cfg.client_id);
								cJSON_AddStringToObject(response, "note",
									"Send mqtt_restart to reconnect with new client ID");
							} else {
								cJSON_AddStringToObject(response, "status", "missing_id");
								cJSON_AddStringToObject(response, "hint",
									"{\"command\":\"mqtt_set_client_id\",\"id\":\"my_device\"}");
							}

						} else if (strcmp(command->valuestring, "mqtt_restart") == 0) {
							cJSON_AddStringToObject(response, "status", "restarting");
							cJSON_AddStringToObject(response, "host", mqtt_rt_cfg.host);
							cJSON_AddNumberToObject(response, "port", mqtt_rt_cfg.port);
							/* Disconnect + reconnect after 2 s (gives time for response to send) */
							k_work_reschedule(&mqtt_restart_work, K_SECONDS(2));

						} else if (strcmp(command->valuestring, "mqtt_set_topics") == 0) {
							cJSON *pub_j = cJSON_GetObjectItem(received_json, "pub_topic");
							cJSON *sub_j = cJSON_GetObjectItem(received_json, "sub_topic");
							bool changed = false;
							if (pub_j && cJSON_IsString(pub_j) && strlen(pub_j->valuestring) > 0) {
								strncpy(mqtt_rt_cfg.pub_topic, pub_j->valuestring,
									sizeof(mqtt_rt_cfg.pub_topic) - 1);
								settings_save_one("app/mqtt/pub_topic", mqtt_rt_cfg.pub_topic,
										  strlen(mqtt_rt_cfg.pub_topic));
								changed = true;
							}
							if (sub_j && cJSON_IsString(sub_j) && strlen(sub_j->valuestring) > 0) {
								strncpy(mqtt_rt_cfg.sub_topic, sub_j->valuestring,
									sizeof(mqtt_rt_cfg.sub_topic) - 1);
								settings_save_one("app/mqtt/sub_topic", mqtt_rt_cfg.sub_topic,
										  strlen(mqtt_rt_cfg.sub_topic));
								changed = true;
							}
							if (changed) {
								cJSON_AddStringToObject(response, "status", "saved");
								cJSON_AddStringToObject(response, "pub_topic", mqtt_rt_cfg.pub_topic);
								cJSON_AddStringToObject(response, "sub_topic", mqtt_rt_cfg.sub_topic);
								cJSON_AddStringToObject(response, "note",
									"Send mqtt_restart to reconnect with new topics");
							} else {
								cJSON_AddStringToObject(response, "status", "no_changes");
								cJSON_AddStringToObject(response, "hint",
									"{\"command\":\"mqtt_set_topics\",\"pub_topic\":\"gw/dev1/data\",\"sub_topic\":\"gw/dev1/cmd\"}");
							}

						} else if (strcmp(command->valuestring, "mqtt_set_tls") == 0) {
							cJSON *en_j = cJSON_GetObjectItem(received_json, "enabled");
							cJSON *tag_j = cJSON_GetObjectItem(received_json, "sec_tag");
							if (en_j && cJSON_IsBool(en_j)) {
								mqtt_rt_cfg.tls_enabled = cJSON_IsTrue(en_j);
								uint8_t v = mqtt_rt_cfg.tls_enabled ? 1 : 0;
								settings_save_one("app/mqtt/tls", &v, sizeof(v));
							}
							if (tag_j && cJSON_IsNumber(tag_j)) {
								mqtt_rt_cfg.sec_tag = (uint32_t)tag_j->valueint;
								settings_save_one("app/mqtt/sec_tag", &mqtt_rt_cfg.sec_tag,
										  sizeof(mqtt_rt_cfg.sec_tag));
							}
							cJSON_AddStringToObject(response, "status", "saved");
							cJSON_AddBoolToObject(response, "tls_enabled", mqtt_rt_cfg.tls_enabled);
							cJSON_AddNumberToObject(response, "sec_tag", mqtt_rt_cfg.sec_tag);
							cJSON_AddStringToObject(response, "note",
								"Send mqtt_restart to reconnect with new TLS settings");

						} else if (strcmp(command->valuestring, "set_power_mode") == 0) {
							cJSON *mode_j = cJSON_GetObjectItem(received_json, "mode");
							if (mode_j && cJSON_IsString(mode_j)) {
								if (strcmp(mode_j->valuestring, "high") == 0) {
									mqtt_rt_cfg.power_mode = POWER_MODE_HIGH;
								} else {
									mqtt_rt_cfg.power_mode = POWER_MODE_NORMAL;
								}
								uint8_t v = (uint8_t)mqtt_rt_cfg.power_mode;
								settings_save_one("app/mqtt/power_mode", &v, sizeof(v));
								cJSON_AddStringToObject(response, "status", "ok");
								cJSON_AddStringToObject(response, "power_mode",
									mqtt_rt_cfg.power_mode == POWER_MODE_HIGH ? "high" : "normal");
								LOG_INF("Power mode changed to %s",
									mqtt_rt_cfg.power_mode == POWER_MODE_HIGH ? "HIGH" : "NORMAL");
								/* Reschedule heartbeat with new interval */
								uint32_t hb = (mqtt_rt_cfg.power_mode == POWER_MODE_HIGH)
									     ? MQTT_HIGH_POWER_HEARTBEAT_SEC
									     : MQTT_HEARTBEAT_INTERVAL_SEC;
								k_work_reschedule(&mqtt_ctx.data_send_work, K_SECONDS(hb));
								/* Notify main module about trigger interval change via ZBUS */
								int new_interval = (mqtt_rt_cfg.power_mode == POWER_MODE_HIGH)
										   ? MQTT_HIGH_POWER_TRIGGER_INTERVAL_SEC
										   : CONFIG_APP_MODULE_TRIGGER_TIMEOUT_SECONDS;
								int pub_ret = zbus_chan_pub(&TIMER_CHAN, &new_interval, K_MSEC(500));
								if (pub_ret) {
									LOG_WRN("Failed to notify main of interval change: %d", pub_ret);
								}
							} else {
								cJSON_AddStringToObject(response, "status", "invalid_mode");
								cJSON_AddStringToObject(response, "hint",
									"{\"command\":\"set_power_mode\",\"mode\":\"high\"}");
							}

						} else {
							/* Unknown command — do NOT forward to UART blindly.
							 * Only structured ESL commands (esl_scan, esl_command, etc.)
							 * and uart_passthrough are forwarded. */
							LOG_WRN("Unknown MQTT command (rejected): %s", command->valuestring);
							cJSON_AddStringToObject(response, "status", "unknown_command");
						}
						
						cJSON_AddStringToObject(response, "command_processed", command->valuestring);
					}
					cJSON_Delete(received_json);
				} else {
					cJSON_AddStringToObject(response, "status", "message_received");
				}
				
				char *response_string = cJSON_Print(response);
				if (response_string) {
					mqtt_publish_data(response_string, strlen(response_string));
					cJSON_free(response_string);
				}
				cJSON_Delete(response);
			}
			
publish_skip:
			
			msg.type = CUSTOM_MQTT_EVT_DATA_RECEIVED;
			msg.data_received.data = (char *)mqtt_ctx.payload_buf;
			msg.data_received.len = len;
			zbus_chan_pub(&CUSTOM_MQTT_CHAN, &msg, K_NO_WAIT);
		} else {
			LOG_WRN("Received message with no payload or read failed");
		}
		break;

	case MQTT_EVT_PUBACK:
		LOG_DBG("MQTT publish acknowledged (message_id: %u)", evt->param.puback.message_id);
		last_mqtt_activity_ms = k_uptime_get();
		/* Reset failure counter on successful publish */
		if (mqtt_ctx.publish_failures > 0) {
			mqtt_ctx.publish_failures = MAX(0, mqtt_ctx.publish_failures - 1);
		}
		break;

	case MQTT_EVT_SUBACK:
		LOG_INF(">>> MQTT_EVT_SUBACK received! <<<");
		LOG_INF("MQTT subscription acknowledged (message_id: %u)", evt->param.suback.message_id);
		LOG_INF("Successfully subscribed to topic: %s", MQTT_SUB_TOPIC);
		last_mqtt_activity_ms = k_uptime_get();
		break;

	case MQTT_EVT_UNSUBACK:
		LOG_INF("MQTT unsubscription acknowledged");
		break;

	case MQTT_EVT_PINGRESP:
		LOG_DBG("MQTT ping response received");
		last_mqtt_activity_ms = k_uptime_get();
		break;

	default:
		LOG_WRN("Unhandled MQTT event: %d", evt->type);
		break;
	}
}

static void connect_work_handler(struct k_work *work)
{
	if (mqtt_ctx.state == MQTT_STATE_IDLE) {
		LOG_INF("Connection work triggered, attempting MQTT connection");
		/* Try to connect regardless of network_connected flag */
		smf_set_state(&sm_ctx, &mqtt_states[MQTT_STATE_CONNECTING]);
	} else {
		LOG_DBG("Connection work triggered but MQTT not in idle state (%d)", mqtt_ctx.state);
	}
}

static void mqtt_restart_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	LOG_INF("MQTT restart: applying new config (host=%s port=%u user=%s)",
		mqtt_rt_cfg.host, mqtt_rt_cfg.port, mqtt_rt_cfg.username);
	if (mqtt_ctx.state == MQTT_STATE_CONNECTED ||
	    mqtt_ctx.state == MQTT_STATE_CONNECTING) {
		custom_mqtt_disconnect();
	}
	mqtt_ctx.state = MQTT_STATE_IDLE;
	k_work_reschedule(&mqtt_ctx.connect_work, K_SECONDS(3));
}

static void mqtt_inactivity_work_fn(struct k_work *work)
{
	int64_t now = k_uptime_get();
	int64_t elapsed_ms = now - last_mqtt_activity_ms;

	if (elapsed_ms >= (int64_t)MQTT_INACTIVITY_WATCHDOG_SEC * 1000) {
		LOG_ERR("No MQTT activity for %lld s — saving reboot flag and rebooting",
			elapsed_ms / 1000);
		uint8_t flag = 1;

		settings_save_one("app/watchdog/mqtt_inactive", &flag, sizeof(flag));
		/* Brief delay to let flash write complete */
		k_sleep(K_MSEC(500));
		sys_reboot(SYS_REBOOT_COLD);
	} else {
		/* Not yet expired — reschedule to check at expiry time */
		int64_t remaining_ms = (int64_t)MQTT_INACTIVITY_WATCHDOG_SEC * 1000 - elapsed_ms;

		k_work_reschedule(&mqtt_inactivity_work, K_MSEC(remaining_ms));
	}
}

static void data_send_work_handler(struct k_work *work)
{
	if (mqtt_ctx.state != MQTT_STATE_CONNECTED) {
		return;
	}

	cJSON *json = cJSON_CreateObject();

	if (json) {
		cJSON_AddStringToObject(json, "type", "heartbeat");
		cJSON_AddNumberToObject(json, "uptime_ms", k_uptime_get());
		cJSON_AddStringToObject(json, "firmware_version", APP_VERSION_STRING);
		cJSON_AddNumberToObject(json, "sequence", mqtt_ctx.publish_sequence + 1);
		cJSON_AddStringToObject(json, "power_mode",
					mqtt_rt_cfg.power_mode == POWER_MODE_HIGH ? "high" : "normal");

		cJSON *diagnostics = cJSON_CreateObject();

		if (diagnostics) {
			cJSON_AddNumberToObject(diagnostics, "publish_failures",
						mqtt_ctx.publish_failures);
			cJSON_AddNumberToObject(diagnostics, "total_publishes",
						mqtt_ctx.publish_sequence);
			cJSON_AddBoolToObject(diagnostics, "network_connected",
					   mqtt_ctx.network_connected);
			cJSON_AddItemToObject(json, "diagnostics", diagnostics);
		}

		int ret = safe_publish_json(json, "heartbeat");

		if (ret == 0) {
			LOG_INF("Heartbeat sent (seq: %u, failures: %u)",
				mqtt_ctx.publish_sequence, mqtt_ctx.publish_failures);
		} else {
			LOG_ERR("Failed to send heartbeat: %d", ret);
		}
		cJSON_Delete(json);
	}

	/* Schedule next heartbeat — interval depends on power mode */
	uint32_t hb_interval = (mqtt_rt_cfg.power_mode == POWER_MODE_HIGH)
				? MQTT_HIGH_POWER_HEARTBEAT_SEC
				: MQTT_HEARTBEAT_INTERVAL_SEC;
	k_work_schedule(&mqtt_ctx.data_send_work, K_SECONDS(hb_interval));
}

static int custom_mqtt_connect(void)
{
	struct sockaddr_in *broker4 = (struct sockaddr_in *)&mqtt_ctx.broker_addr;
	
	/* Configure broker address */
	broker4->sin_family = AF_INET;
	broker4->sin_port = htons(MQTT_BROKER_PORT);
	
	/* Resolve hostname */
	LOG_INF("Starting DNS resolution for %s", MQTT_BROKER_HOSTNAME);
	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct addrinfo *result;
	
	int ret = getaddrinfo(MQTT_BROKER_HOSTNAME, NULL, &hints, &result);
	if (ret != 0) {
		LOG_ERR("Failed to resolve hostname %s: %d", MQTT_BROKER_HOSTNAME, ret);
		return ret;
	}
	
	char ip_str[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &((struct sockaddr_in *)result->ai_addr)->sin_addr, ip_str, INET_ADDRSTRLEN);
	LOG_INF("DNS resolved %s to %s", MQTT_BROKER_HOSTNAME, ip_str);
	
	broker4->sin_addr = ((struct sockaddr_in *)result->ai_addr)->sin_addr;
	freeaddrinfo(result);

	LOG_INF("Initializing MQTT client");
	/* Initialize MQTT client */
	mqtt_client_init(&mqtt_ctx.client);

	LOG_INF("Configuring MQTT client parameters");
	/* Set up client configuration */
	mqtt_ctx.client.broker = &mqtt_ctx.broker_addr;
	mqtt_ctx.client.evt_cb = mqtt_evt_handler;
	mqtt_ctx.client.client_id.utf8 = (uint8_t *)mqtt_rt_cfg.client_id;
	mqtt_ctx.client.client_id.size = strlen(mqtt_rt_cfg.client_id);
	mqtt_ctx.client.protocol_version = MQTT_VERSION_3_1_1;
	mqtt_ctx.client.clean_session = 0; /* Persistent session — broker queues QoS1 commands during PSM sleep and delivers on reconnect */
	mqtt_ctx.client.rx_buf = mqtt_ctx.rx_buffer;
	mqtt_ctx.client.rx_buf_size = sizeof(mqtt_ctx.rx_buffer);
	mqtt_ctx.client.tx_buf = mqtt_ctx.tx_buffer;
	mqtt_ctx.client.tx_buf_size = sizeof(mqtt_ctx.tx_buffer);
	mqtt_ctx.client.keepalive = MQTT_KEEPALIVE;

	LOG_INF("Setting MQTT credentials");
	/* Set username and password if provided */
	if (strlen(MQTT_USERNAME) > 0) {
		mqtt_ctx.username.utf8 = MQTT_USERNAME;
		mqtt_ctx.username.size = strlen(MQTT_USERNAME);
		mqtt_ctx.client.user_name = &mqtt_ctx.username;
		
		if (strlen(MQTT_PASSWORD) > 0) {
			mqtt_ctx.password.utf8 = MQTT_PASSWORD;
			mqtt_ctx.password.size = strlen(MQTT_PASSWORD);
			mqtt_ctx.client.password = &mqtt_ctx.password;
		} else {
			mqtt_ctx.client.password = NULL;
		}
		
		LOG_INF("Using authentication with username: %s", MQTT_USERNAME);
	} else {
		mqtt_ctx.client.user_name = NULL;
		mqtt_ctx.client.password = NULL;
		LOG_INF("Using anonymous connection (no credentials)");
	}

	LOG_INF("Configuring transport settings");
	/* Configure Transport — use runtime TLS flag (overrides compile-time default) */
	if (mqtt_rt_cfg.tls_enabled) {
		mqtt_ctx.client.transport.type = MQTT_TRANSPORT_SECURE;

		struct mqtt_sec_config *tls_config = &mqtt_ctx.client.transport.tls.config;
		tls_config->peer_verify = TLS_PEER_VERIFY_NONE;
		tls_config->cipher_list = NULL;

		if (mqtt_rt_cfg.sec_tag > 0) {
			rt_sec_tag_list[0] = (sec_tag_t)mqtt_rt_cfg.sec_tag;
			tls_config->sec_tag_count = 1;
			tls_config->sec_tag_list = rt_sec_tag_list;
			LOG_INF("Using runtime security tag: %u", mqtt_rt_cfg.sec_tag);
		} else {
			tls_config->sec_tag_count = 0;
			tls_config->sec_tag_list = NULL;
			LOG_INF("TLS enabled but no security tag configured");
		}

		tls_config->hostname = MQTT_BROKER_HOSTNAME;
		LOG_INF("Using TLS transport");
	} else {
		mqtt_ctx.client.transport.type = MQTT_TRANSPORT_NON_SECURE;
		LOG_INF("Using non-secure transport (TCP)");
	}

	LOG_INF("Starting MQTT connection to %s:%d", MQTT_BROKER_HOSTNAME, MQTT_BROKER_PORT);
	LOG_INF("Client ID: %s, Username: %s", MQTT_CLIENT_ID, MQTT_USERNAME);
	
	ret = mqtt_connect(&mqtt_ctx.client);
	LOG_INF("mqtt_connect() returned: %d", ret);
	if (ret) {
		LOG_ERR("Failed to connect to MQTT broker: %d", ret);
		return ret;
	}

	LOG_INF("MQTT connection initiated successfully");
	return 0;
}

static int custom_mqtt_disconnect(void)
{
	LOG_INF("Disconnecting from MQTT broker");
	return mqtt_disconnect(&mqtt_ctx.client, NULL);
}

static int mqtt_publish_data(const char *data, size_t len)
{
	struct mqtt_publish_param param;
	int ret;

	if (!data || len == 0) {
		LOG_ERR("Invalid data parameters");
		return -EINVAL;
	}

	if (mqtt_ctx.state != MQTT_STATE_CONNECTED) {
		LOG_WRN("MQTT not connected, cannot publish");
		return -ENOTCONN;
	}

	k_mutex_lock(&mqtt_ctx.data_mutex, K_FOREVER);

	param.message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
	param.message.topic.topic.utf8 = MQTT_PUB_TOPIC;
	param.message.topic.topic.size = strlen(MQTT_PUB_TOPIC);
	param.message.payload.data = (uint8_t *)data;
	param.message.payload.len = len;
	param.message_id = ++mqtt_ctx.publish_sequence;
	param.dup_flag = 0;
	param.retain_flag = 0;

	LOG_DBG("Publishing %zu bytes to topic %s (seq: %u)", len, MQTT_PUB_TOPIC, 
		mqtt_ctx.publish_sequence);

	ret = mqtt_publish(&mqtt_ctx.client, &param);
	if (ret) {
		mqtt_ctx.publish_failures++;
		LOG_ERR("Failed to publish data: %d (failures: %u)", ret, mqtt_ctx.publish_failures);
	}

	k_mutex_unlock(&mqtt_ctx.data_mutex);
	return ret;
}

/* Data validation and helper functions */
static bool validate_sensor_data(double value, double min, double max)
{
	if (!isfinite(value)) {
		LOG_WRN("Invalid sensor value: not finite");
		return false;
	}
	
	if (value < min || value > max) {
		LOG_WRN("Sensor value %.2f out of range [%.2f, %.2f]", value, min, max);
		return false;
	}
	
	return true;
}

static int safe_publish_json(cJSON *json, const char *data_type)
{
	char *json_string;
	int ret = -ENOMEM;
	
	if (!json) {
		LOG_ERR("NULL JSON object for %s data", data_type ? data_type : "unknown");
		return -EINVAL;
	}
	
	/* Add common fields */
	cJSON_AddStringToObject(json, "device_id", MQTT_CLIENT_ID);
	cJSON_AddNumberToObject(json, "timestamp", k_uptime_get());
	
	json_string = cJSON_Print(json);
	if (json_string) {
		if (mqtt_ctx.state == MQTT_STATE_CONNECTED) {
			ret = mqtt_publish_data(json_string, strlen(json_string));
			if (ret == 0) {
				LOG_DBG("Published %s", data_type ? data_type : "JSON");
				last_mqtt_activity_ms = k_uptime_get();
			} else {
				LOG_ERR("Failed to publish %s: %d", data_type ? data_type : "JSON", ret);
			}
		} else {
			LOG_WRN("MQTT not connected, cannot publish %s", data_type ? data_type : "JSON");
			ret = -ENOTCONN;
		}
		cJSON_free(json_string);
	} else {
		LOG_ERR("Failed to serialize %s JSON", data_type ? data_type : "JSON");
	}
	
	return ret;
}
/* Location trigger functionality */
#if defined(CONFIG_APP_LOCATION)

static void trigger_location_request(void)
{
	static bool first_request = true;
	
	/* Add startup delay for the first location request to ensure system stability */
	if (first_request) {
		int64_t uptime = k_uptime_get();
		if (uptime < 10000) { /* Less than 10 seconds uptime */
			LOG_INF("System uptime too short (%lld ms), delaying location request", uptime);
			return;
		}
		first_request = false;
	}
	
	LOG_INF("Triggering location request via direct API (bypassing ZBUS)");
	
	/* Use direct API call instead of ZBUS to avoid buffer exhaustion */
	int err = location_trigger_update_direct();
	if (err) {
		LOG_ERR("Failed to trigger location request: %d", err);
	} else {
		LOG_INF("Location search triggered successfully");
	}
}

static void location_trigger_work_handler(struct k_work *work)
{
	static uint32_t location_request_count = 0;
	
	if (mqtt_ctx.state == MQTT_STATE_CONNECTED && mqtt_ctx.network_connected) {
		location_request_count++;
		LOG_INF("Triggering periodic location update #%u", location_request_count);
		trigger_location_request();
	} else {
		LOG_DBG("Skipping location update - MQTT not ready (state: %d, network: %d)", 
			mqtt_ctx.state, mqtt_ctx.network_connected);
	}
	
	/* Schedule next location update */
	k_work_schedule(&mqtt_ctx.location_trigger_work, 
			K_SECONDS(MQTT_LOCATION_UPDATE_INTERVAL_SEC));
}
#endif

#if defined(CONFIG_APP_LOCATION)
/* Public API function to receive location data directly (avoiding ZBUS circular dependency) */
int custom_mqtt_send_location_data(const struct location_msg *location_data)
{
	if (!location_data) {
		LOG_ERR("Invalid location data pointer");
		return -EINVAL;
	}
	
	if (mqtt_ctx.state != MQTT_STATE_CONNECTED) {
		LOG_WRN("MQTT not connected, cannot process location data");
		return -ENOTCONN;
	}
	
	LOG_INF("Received location data via direct API call");
	
	k_mutex_lock(&mqtt_ctx.data_mutex, K_FOREVER);
	process_location_data(location_data);
	k_mutex_unlock(&mqtt_ctx.data_mutex);
	
	return 0;
}
#endif

/* State machine implementations */
static void idle_entry(void *obj)
{
	LOG_DBG("Entering MQTT idle state");
	mqtt_ctx.state = MQTT_STATE_IDLE;
}

static void idle_run(void *obj)
{
	/* Check network status and attempt connection */
	if (mqtt_ctx.network_connected) {
		LOG_INF("Network available, transitioning to connecting state");
		smf_set_state(&sm_ctx, &mqtt_states[MQTT_STATE_CONNECTING]);
	} else {
		/* Periodically log that we're waiting for network */
		static uint32_t network_wait_count = 0;
		if (++network_wait_count % 30 == 0) {  /* Log every 30 seconds */
			LOG_INF("Waiting for network connection... (%d)", network_wait_count);
		}
	}
}

static void connecting_entry(void *obj)
{
	LOG_INF("Entering MQTT connecting state");
	mqtt_ctx.state = MQTT_STATE_CONNECTING;
	
	int ret = custom_mqtt_connect();
	if (ret != 0) {
		LOG_ERR("MQTT connection failed with error: %d", ret);
		smf_set_state(&sm_ctx, &mqtt_states[MQTT_STATE_ERROR]);
	} else {
		LOG_INF("MQTT connection initiated, waiting for response");
	}
}

static void connecting_run(void *obj)
{
	/* Poll MQTT client for events - this is crucial for processing CONNACK */
	int ret = mqtt_input(&mqtt_ctx.client);
	if (ret < 0 && ret != -EAGAIN) {
		LOG_ERR("MQTT input error during connection: %d", ret);
		smf_set_state(&sm_ctx, &mqtt_states[MQTT_STATE_ERROR]);
		return;
	}
	
	/* Check if we transitioned to connected state via CONNACK event */
	if (mqtt_ctx.state == MQTT_STATE_CONNECTED) {
		LOG_INF("CONNACK received, transitioning to connected state");
		smf_set_state(&sm_ctx, &mqtt_states[MQTT_STATE_CONNECTED]);
		return;
	}
	
	/* Also call mqtt_live to maintain the connection */
	ret = mqtt_live(&mqtt_ctx.client);
	if (ret < 0 && ret != -EAGAIN) {
		LOG_ERR("MQTT live error: %d", ret);
		smf_set_state(&sm_ctx, &mqtt_states[MQTT_STATE_ERROR]);
	}
}

static void connected_entry(void *obj)
{
	LOG_INF("Entering MQTT connected state");
	mqtt_ctx.state = MQTT_STATE_CONNECTED;
	
	/* Subscribe to command topic */
	struct mqtt_subscription_list subscription_list;
	struct mqtt_topic subscribe_topic;
	
	LOG_INF(">>> Attempting to subscribe to topic: %s <<<", MQTT_SUB_TOPIC);
	
	subscribe_topic.topic.utf8 = MQTT_SUB_TOPIC;
	subscribe_topic.topic.size = strlen(MQTT_SUB_TOPIC);
	subscribe_topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
	
	subscription_list.list = &subscribe_topic;
	subscription_list.list_count = 1;
	subscription_list.message_id = 1;
	
	int ret = mqtt_subscribe(&mqtt_ctx.client, &subscription_list);
	if (ret) {
		LOG_ERR("Failed to send SUBSCRIBE request: %d", ret);
	} else {
		LOG_INF("SUBSCRIBE request sent for topic: %s (waiting for SUBACK)", MQTT_SUB_TOPIC);
	}
	
	/* Send initial connection message */
	k_sleep(K_MSEC(1000)); /* Give subscription time to complete */

	/* Successful connection — reset reconnect delay back to base */
	mqtt_reconnect_delay_sec = MQTT_RECONNECT_BASE_DELAY_SEC;
	mqtt_ctx.publish_failures = 0;

	/* Mark activity and start inactivity watchdog */
	last_mqtt_activity_ms = k_uptime_get();
	k_work_reschedule(&mqtt_inactivity_work, K_SECONDS(MQTT_INACTIVITY_WATCHDOG_SEC));

	cJSON *json = cJSON_CreateObject();
	if (json) {
		cJSON_AddStringToObject(json, "device_id", MQTT_CLIENT_ID);
		cJSON_AddStringToObject(json, "status", "connected");
		cJSON_AddNumberToObject(json, "timestamp", k_uptime_get());
		cJSON_AddStringToObject(json, "message", "Device connected to MQTT broker");
		cJSON_AddStringToObject(json, "build", APP_VERSION_STRING);
		
		char *json_string = cJSON_Print(json);
		if (json_string) {
			ret = mqtt_publish_data(json_string, strlen(json_string));
			if (ret == 0) {
				LOG_INF("Initial connection message sent");
			} else {
				LOG_ERR("Failed to send initial message: %d", ret);
			}
			cJSON_free(json_string);
		}
		cJSON_Delete(json);
	}

	/* If device rebooted due to MQTT inactivity, report it once and clear the flag */
	if (mqtt_inactive_reboot_flag) {
		cJSON *rb_json = cJSON_CreateObject();

		if (rb_json) {
			cJSON_AddStringToObject(rb_json, "device_id", MQTT_CLIENT_ID);
			cJSON_AddStringToObject(rb_json, "type", "reboot_reason");
			cJSON_AddStringToObject(rb_json, "reason", "mqtt_inactivity");
			cJSON_AddNumberToObject(rb_json, "watchdog_timeout_sec",
						MQTT_INACTIVITY_WATCHDOG_SEC);
			cJSON_AddNumberToObject(rb_json, "timestamp", k_uptime_get());

			char *rb_str = cJSON_Print(rb_json);

			if (rb_str) {
				int rc = mqtt_publish_data(rb_str, strlen(rb_str));

				if (rc == 0) {
					LOG_WRN("Reboot-reason message sent (mqtt_inactivity)");
					/* Clear flag in flash — reported successfully */
					uint8_t flag = 0;

					settings_save_one("app/watchdog/mqtt_inactive",
							  &flag, sizeof(flag));
					mqtt_inactive_reboot_flag = false;
				} else {
					LOG_ERR("Failed to send reboot-reason: %d (will retry next connect)", rc);
				}
				cJSON_free(rb_str);
			}
			cJSON_Delete(rb_json);
		}
	}

	/* Start periodic data sending */
	k_work_schedule(&mqtt_ctx.data_send_work, K_SECONDS(10));
	
#if defined(CONFIG_APP_LOCATION)
	/* Temporarily disable automatic location updates to debug heap issues */
	LOG_INF("Location support enabled but automatic updates disabled for debugging");
	// k_work_schedule(&mqtt_ctx.location_trigger_work, K_SECONDS(60));
#endif
}

static void connected_run(void *obj)
{
	int ret;

	/* Use poll() so we only call mqtt_input when data is actually available.
	 * This avoids unnecessary blocking and keeps the thread responsive. */
	struct zsock_pollfd fds = {
		.fd = mqtt_ctx.client.transport.tcp.sock,
		.events = ZSOCK_POLLIN,
	};

	ret = zsock_poll(&fds, 1, 0); /* non-blocking check */
	if (ret > 0 && (fds.revents & ZSOCK_POLLIN)) {
		ret = mqtt_input(&mqtt_ctx.client);
		if (ret < 0 && ret != -EAGAIN) {
			LOG_ERR("MQTT input error: %d", ret);
			smf_set_state(&sm_ctx, &mqtt_states[MQTT_STATE_ERROR]);
			return;
		}
	}

	/* Maintain MQTT connection (keepalive pings) */
	ret = mqtt_live(&mqtt_ctx.client);
	if (ret < 0 && ret != -EAGAIN) {
		LOG_ERR("MQTT live error: %d", ret);
		smf_set_state(&sm_ctx, &mqtt_states[MQTT_STATE_ERROR]);
		return;
	}

	if (!mqtt_ctx.network_connected) {
		LOG_INF("Network disconnected, transitioning to disconnecting state");
		smf_set_state(&sm_ctx, &mqtt_states[MQTT_STATE_DISCONNECTING]);
	}
}

static void disconnecting_entry(void *obj)
{
	LOG_DBG("Entering MQTT disconnecting state");
	mqtt_ctx.state = MQTT_STATE_DISCONNECTING;

	/* Cancel ongoing work */
	k_work_cancel_delayable(&mqtt_ctx.data_send_work);
	k_work_cancel_delayable(&mqtt_inactivity_work);
#if defined(CONFIG_APP_LOCATION)
	k_work_cancel_delayable(&mqtt_ctx.location_trigger_work);
#endif

	custom_mqtt_disconnect();
}

static void disconnecting_run(void *obj)
{
	/* State transition handled in MQTT event handler */
}

static void error_entry(void *obj)
{
	LOG_DBG("Entering MQTT error state");
	mqtt_ctx.state = MQTT_STATE_ERROR;

	/* Cancel any pending work */
	k_work_cancel_delayable(&mqtt_ctx.data_send_work);
	k_work_cancel_delayable(&mqtt_inactivity_work);
#if defined(CONFIG_APP_LOCATION)
	k_work_cancel_delayable(&mqtt_ctx.location_trigger_work);
#endif

	/* Exponential backoff using module-level variable (resets on successful connect) */
	if (mqtt_ctx.publish_failures > MQTT_MAX_PUBLISH_FAILURES) {
		mqtt_reconnect_delay_sec = MIN(mqtt_reconnect_delay_sec * 2,
					       MQTT_RECONNECT_MAX_DELAY_SEC);
	} else {
		mqtt_reconnect_delay_sec = MQTT_RECONNECT_BASE_DELAY_SEC;
	}

	LOG_WRN("MQTT error state, will retry connection in %u seconds", mqtt_reconnect_delay_sec);

	/* Schedule reconnection attempt */
	k_work_schedule(&mqtt_ctx.connect_work, K_SECONDS(mqtt_reconnect_delay_sec));
}

static void error_run(void *obj)
{
	/* Wait for reconnection attempt */
}

/* Message processing functions */
#if defined(CONFIG_APP_LOCATION)
static void process_location_data(const struct location_msg *msg)
{
	cJSON *json = NULL;
	cJSON *location_data = NULL;
	cJSON *location_details = NULL;
	char *json_string = NULL;
	int ret = 0;
	
	if (!msg) {
		LOG_ERR("Invalid location message");
		return;
	}
	
	LOG_DBG("Processing location message type %d", msg->type);
	
	/* Only process actual location data */
	if (msg->type != LOCATION_GNSS_DATA) {
		LOG_DBG("Received location message type %d, not publishing", msg->type);
		return;
	}
	
	/* Additional safety check for GNSS data validity */
	if (!isfinite(msg->gnss_data.latitude) || !isfinite(msg->gnss_data.longitude) || 
	    !isfinite(msg->gnss_data.accuracy)) {
		LOG_ERR("Location data contains invalid values, skipping");
		return;
	}
	
	/* Validate location data with production-level checks */
	if (!validate_sensor_data(msg->gnss_data.latitude, -90.0, 90.0)) {
		LOG_WRN("Invalid latitude: %.6f, skipping location data", 
			msg->gnss_data.latitude);
		return;
	}
	
	if (!validate_sensor_data(msg->gnss_data.longitude, -180.0, 180.0)) {
		LOG_WRN("Invalid longitude: %.6f, skipping location data", 
			msg->gnss_data.longitude);
		return;
	}
	
	if ((double)msg->gnss_data.accuracy > MQTT_GPS_ACCURACY_MAX_METERS || 
	    msg->gnss_data.accuracy <= 0.0f) {
		LOG_WRN("GPS accuracy out of range: %.2f m, skipping", 
			(double)msg->gnss_data.accuracy);
		return;
	}
	
	/* Create JSON structure */
	json = cJSON_CreateObject();
	location_data = cJSON_CreateObject();
	location_details = cJSON_CreateObject();
	
	if (!json || !location_data || !location_details) {
		LOG_ERR("Failed to create JSON objects for location data");
		goto cleanup;
	}
	
	/* Add main message structure */
	cJSON_AddStringToObject(json, "device_id", MQTT_CLIENT_ID);
	cJSON_AddStringToObject(json, "type", "location");
	cJSON_AddNumberToObject(json, "timestamp", k_uptime_get());
	cJSON_AddNumberToObject(json, "sequence", ++mqtt_ctx.publish_sequence);
	
	/* Add location coordinates with proper precision */
	cJSON_AddNumberToObject(location_data, "latitude", 
		round(msg->gnss_data.latitude * 1000000.0) / 1000000.0);
	cJSON_AddNumberToObject(location_data, "longitude", 
		round(msg->gnss_data.longitude * 1000000.0) / 1000000.0);
	cJSON_AddNumberToObject(location_data, "accuracy", 
		round((double)msg->gnss_data.accuracy * 10.0) / 10.0);
	
	/* Add location method information */
	cJSON_AddStringToObject(location_details, "method", "gnss");
	
	/* Add timestamp if available */
	if (msg->gnss_data.datetime.valid) {
		int64_t unix_time_ms = 0;
		date_time_now(&unix_time_ms);
		cJSON_AddNumberToObject(location_details, "fix_timestamp", unix_time_ms);
		cJSON_AddBoolToObject(location_details, "time_valid", true);
	} else {
		cJSON_AddBoolToObject(location_details, "time_valid", false);
	}
	
	/* Add additional GNSS details if available */
	#if defined(CONFIG_LOCATION_DATA_DETAILS)
	/* Check if location details are available by checking method */
	const struct location_data_details *details = &msg->gnss_data.details;
	if (details->gnss.satellites_tracked > 0) {
		cJSON_AddNumberToObject(location_details, "satellites_tracked", 
			details->gnss.satellites_tracked);
	}
	if (details->gnss.satellites_used > 0) {
		cJSON_AddNumberToObject(location_details, "satellites_used", 
			details->gnss.satellites_used);
	}
	if (details->elapsed_time_method > 0) {
		cJSON_AddNumberToObject(location_details, "ttff_ms", 
			details->elapsed_time_method);
	}
	#endif
	
	/* Assemble final JSON structure */
	cJSON_AddItemToObject(location_data, "details", location_details);
	location_details = NULL; /* Ownership transferred */
	cJSON_AddItemToObject(json, "data", location_data);
	location_data = NULL; /* Ownership transferred */
	
	/* Convert to string and validate */
	json_string = cJSON_Print(json);
	if (!json_string) {
		LOG_ERR("Failed to serialize location JSON");
		goto cleanup;
	}
	
	/* Publish if connected */
	if (mqtt_ctx.state == MQTT_STATE_CONNECTED) {
		ret = mqtt_publish_data(json_string, strlen(json_string));
		if (ret == 0) {
			LOG_INF("Location published: lat=%.6f, lng=%.6f, acc=%.1fm, method=gnss",
				msg->gnss_data.latitude, msg->gnss_data.longitude, 
				(double)msg->gnss_data.accuracy);
		} else {
			LOG_ERR("Failed to publish location data: %d", ret);
			mqtt_ctx.publish_failures++;
		}
	} else {
		LOG_WRN("MQTT not connected, discarding location data");
	}

cleanup:
	if (json_string) {
		cJSON_free(json_string);
	}
	if (location_details) {
		cJSON_Delete(location_details);
	}
	if (location_data) {
		cJSON_Delete(location_data);
	}
	if (json) {
		cJSON_Delete(json);
	}
}
#endif

#if defined(CONFIG_APP_ENVIRONMENTAL)
static void process_environmental_data(const struct environmental_msg *msg)
{
	/* Enhanced validation using helper functions */
	if (!validate_sensor_data(msg->temperature, MQTT_TEMP_MIN_CELSIUS, MQTT_TEMP_MAX_CELSIUS)) {
		return;
	}
	
	if (!validate_sensor_data(msg->humidity, MQTT_HUMIDITY_MIN_PERCENT, MQTT_HUMIDITY_MAX_PERCENT)) {
		return;
	}

	if (!validate_sensor_data(msg->pressure, MQTT_PRESSURE_MIN_PA, MQTT_PRESSURE_MAX_PA)) {
		return;
	}
	
	cJSON *json = cJSON_CreateObject();
	cJSON *env_data = cJSON_CreateObject();
	
	if (json == NULL || env_data == NULL) {
		LOG_ERR("Failed to create JSON objects for environmental data");
		goto cleanup;
	}
	
	cJSON_AddStringToObject(json, "type", "environmental");
	cJSON_AddNumberToObject(json, "sequence", mqtt_ctx.publish_sequence + 1);
	
	/* Add environmental data with limited precision to reduce noise */
	cJSON_AddNumberToObject(env_data, "temperature", round(msg->temperature * 100) / 100.0);
	cJSON_AddNumberToObject(env_data, "humidity", round(msg->humidity * 100) / 100.0);
	cJSON_AddNumberToObject(env_data, "pressure", round(msg->pressure * 10) / 10.0);
	
#if defined(CONFIG_APP_ENVIRONMENTAL_TIMESTAMP)
	if (msg->timestamp > 0) {
		cJSON_AddNumberToObject(env_data, "timestamp", msg->timestamp);
	}
#endif
	
	cJSON_AddItemToObject(json, "data", env_data);
	
	/* Use safe publish function */
	int ret = safe_publish_json(json, "environmental");
	if (ret == 0) {
		LOG_INF("Environmental data published: T=%.2f°C, H=%.2f%%, P=%.1fPa",
			msg->temperature, msg->humidity, msg->pressure);
	}

cleanup:
	if (json) {
		cJSON_Delete(json);
	}
}
#endif

#if defined(CONFIG_APP_POWER)
static void process_power_data(const struct power_msg *msg)
{
	/* If sensor data is invalid, publish a minimal status message */
	if (!msg->data_valid) {
		LOG_WRN("Power data invalid, publishing status only");
		cJSON *json = cJSON_CreateObject();
		if (json) {
			cJSON_AddStringToObject(json, "type", "power");
			cJSON_AddBoolToObject(json, "data_valid", false);
			safe_publish_json(json, "power");
			cJSON_Delete(json);
		}
		return;
	}

	/* Validate power data using helper function */
	if (!validate_sensor_data(msg->percentage, MQTT_BATTERY_MIN_PERCENT, MQTT_BATTERY_MAX_PERCENT)) {
		return;
	}
	
	cJSON *json = cJSON_CreateObject();
	cJSON *power_data = cJSON_CreateObject();
	
	if (json == NULL || power_data == NULL) {
		LOG_ERR("Failed to create JSON objects for power data");
		goto cleanup;
	}
	
	cJSON_AddStringToObject(json, "type", "power");
	cJSON_AddNumberToObject(json, "sequence", mqtt_ctx.publish_sequence + 1);
	
	/* Add comprehensive power data */
	cJSON_AddNumberToObject(power_data, "percentage", round(msg->percentage * 10) / 10.0);
	cJSON_AddNumberToObject(power_data, "voltage", round(msg->voltage * 1000) / 1000.0);
	cJSON_AddNumberToObject(power_data, "current_ma", round(msg->current_ma * 10) / 10.0);
	cJSON_AddNumberToObject(power_data, "temperature", round(msg->temperature * 10) / 10.0);
	cJSON_AddBoolToObject(power_data, "data_valid", msg->data_valid);
	cJSON_AddBoolToObject(power_data, "charging", msg->charging);
	
#if defined(CONFIG_APP_POWER_TIMESTAMP)
	if (msg->timestamp > 0) {
		cJSON_AddNumberToObject(power_data, "timestamp", msg->timestamp);
	}
#endif
	
	cJSON_AddItemToObject(json, "data", power_data);
	
	/* Use safe publish function */
	int ret = safe_publish_json(json, "power");
	if (ret == 0) {
		LOG_INF("Power data published: %.1f%%, %.3fV, %.1fmA, %.1f°C, charging=%s", 
			msg->percentage, msg->voltage, msg->current_ma, msg->temperature,
			msg->charging ? "yes" : "no");
	}

cleanup:
	if (json) {
		cJSON_Delete(json);
	}
}
#endif

#if defined(CONFIG_APP_UART_SENSOR)
static void process_uart_sensor_data(const struct uart_sensor_msg *msg)
{
	cJSON *json = NULL;
	cJSON *sensor_data = NULL;
	cJSON *data_quality = NULL;
	
	if (!msg) {
		LOG_ERR("Invalid UART sensor message");
		return;
	}
	
	/* Handle different message types */
	if (msg->type == UART_SENSOR_ERROR_RESPONSE) {
		LOG_WRN("UART sensor error received: %d - %s", msg->error_type, msg->error_details);
		
		/* Publish error to MQTT */
		cJSON *json = cJSON_CreateObject();
		if (json) {
			cJSON_AddStringToObject(json, "type", "uart_error");
			cJSON_AddNumberToObject(json, "error_code", msg->error_type);
			cJSON_AddStringToObject(json, "details", msg->error_details);
			cJSON_AddNumberToObject(json, "timestamp", k_uptime_get());
			safe_publish_json(json, "uart_error");
			cJSON_Delete(json);
		}
		return;
	}
	
	if (msg->type == UART_SENSOR_GENERIC_RESPONSE) {
		/* Only publish when debug echo is active — off by default in production. */
		if (uart_debug_echo_active) {
			cJSON *dbg = cJSON_CreateObject();
			if (dbg) {
				cJSON_AddStringToObject(dbg, "type", "uart_debug");
				cJSON_AddStringToObject(dbg, "line", msg->response_text);
				safe_publish_json(dbg, "uart_debug");
				cJSON_Delete(dbg);
			}
		} else {
			LOG_DBG("UART generic response (debug echo off): %s", msg->response_text);
		}
		return;
	}

	/* ---- ESL-specific events: publish structured data ---- */

	if (msg->type == UART_SENSOR_ESL_TAG_FOUND) {
		LOG_INF("ESL tag discovered: %s", msg->probe_id);
		cJSON *json = cJSON_CreateObject();
		if (json) {
			cJSON_AddStringToObject(json, "type", "esl_tag_found");
			cJSON_AddStringToObject(json, "mac", msg->probe_id);
			safe_publish_json(json, "esl_tag_found");
			cJSON_Delete(json);
		}
		return;
	}

	if (msg->type == UART_SENSOR_ESL_TAG_CONFIGURED) {
		LOG_INF("ESL tag configured: %s", msg->probe_id);
		cJSON *json = cJSON_CreateObject();
		if (json) {
			cJSON_AddStringToObject(json, "type", "esl_tag_configured");
			cJSON_AddStringToObject(json, "esl_id", msg->probe_id);
			safe_publish_json(json, "esl_tag_configured");
			cJSON_Delete(json);
		}
		return;
	}

	if (msg->type == UART_SENSOR_ESL_TAG_DISCONNECTED) {
		LOG_INF("ESL tag disconnected: %s", msg->tag_info.mac);
		cJSON *json = cJSON_CreateObject();
		if (json) {
			cJSON_AddStringToObject(json, "type", "esl_tag_disconnected");
			cJSON_AddStringToObject(json, "mac", msg->tag_info.mac);
			safe_publish_json(json, "esl_tag_disconnected");
			cJSON_Delete(json);
		}
		return;
	}

	if (msg->type == UART_SENSOR_ESL_NUS_RESPONSE) {
		LOG_INF("ESL NUS data: %s bat=%umV up=%us",
			msg->probe_id, msg->tag_info.battery_mv, msg->tag_info.uptime_s);
		cJSON *json = cJSON_CreateObject();
		if (json) {
			cJSON_AddStringToObject(json, "type", "esl_sensor_data");
			cJSON_AddStringToObject(json, "esl_id", msg->probe_id);
			/* Include human-readable name if known */
			if (msg->tag_info.name[0] != '\0') {
				cJSON_AddStringToObject(json, "name", msg->tag_info.name);
			}
			cJSON_AddNumberToObject(json, "battery_mv", msg->tag_info.battery_mv);
			cJSON_AddNumberToObject(json, "battery_pct",
				(double)roundf(msg->probe_battery * 10.0f) / 10.0);
			cJSON_AddNumberToObject(json, "uptime_s", msg->tag_info.uptime_s);
			cJSON_AddNumberToObject(json, "flags", msg->tag_info.flags);
			if (msg->tag_info.temperature != 0.0f) {
				cJSON_AddNumberToObject(json, "temperature",
					(double)roundf(msg->tag_info.temperature * 100.0f) / 100.0);
			}
			safe_publish_json(json, "esl_sensor_data");
			cJSON_Delete(json);
		}
		return;
	}

	if (msg->type == UART_SENSOR_ESL_AP_READY) {
		LOG_INF("nRF5340 ESL AP ready");
		/* Don't publish boot event — not useful to cloud */
		return;
	}

	if (msg->type == UART_SENSOR_ESL_TAG_CONNECTED) {
		LOG_INF("ESL tag BLE connected: %s", msg->probe_id);
		cJSON *json = cJSON_CreateObject();
		if (json) {
			cJSON_AddStringToObject(json, "type", "esl_tag_connected");
			cJSON_AddStringToObject(json, "mac", msg->probe_id);
			char esl_id[16];
			snprintf(esl_id, sizeof(esl_id), "ESL_0x%04X",
				 msg->tag_info.esl_addr);
			cJSON_AddStringToObject(json, "esl_id", esl_id);
			safe_publish_json(json, "esl_tag_connected");
			cJSON_Delete(json);
		}
		return;
	}

	if (msg->type == UART_SENSOR_ESL_NAME_RESPONSE) {
		LOG_INF("ESL sensor name: 0x%04X = '%s'",
			msg->tag_info.esl_addr, msg->tag_info.name);
		cJSON *json = cJSON_CreateObject();
		if (json) {
			char esl_id_str[16];

			snprintf(esl_id_str, sizeof(esl_id_str),
				 "0x%04X", msg->tag_info.esl_addr);
			cJSON_AddStringToObject(json, "type", "sensor_name");
			cJSON_AddStringToObject(json, "esl_addr", esl_id_str);
			cJSON_AddStringToObject(json, "name",
				msg->tag_info.name[0] ? msg->tag_info.name : "unknown");
			safe_publish_json(json, "sensor_name");
			cJSON_Delete(json);
		}
		return;
	}

	if (msg->type == UART_SENSOR_DATA_REQUEST) {
		/* Internal trigger from main.c — not data, silently ignore */
		return;
	}
	
	if (msg->type != UART_SENSOR_DATA_RESPONSE) {
		LOG_DBG("Ignoring unhandled UART sensor message type: %d", msg->type);
		return;
	}
	
	/* Create JSON payload */
	/* No valid sensor data yet — skip publishing */
	if (strlen(msg->probe_id) == 0) {
		LOG_DBG("No ESL sensor data yet (no tags) — skipping publish");
		return;
	}

	json = cJSON_CreateObject();
	sensor_data = cJSON_CreateObject();
	data_quality = cJSON_CreateObject();
	
	if (!json || !sensor_data || !data_quality) {
		LOG_ERR("Failed to create JSON objects");
		goto cleanup;
	}
	
	/* Add metadata */
	cJSON_AddStringToObject(json, "device_id", MQTT_CLIENT_ID);
	cJSON_AddStringToObject(json, "type", "uart_sensor");
	cJSON_AddNumberToObject(json, "sequence", mqtt_ctx.publish_sequence + 1);
	cJSON_AddNumberToObject(json, "timestamp", k_uptime_get());
	
	/* Add UART sensor data with proper precision - use probe name as-is */
	cJSON_AddNumberToObject(sensor_data, "temperature", round(msg->temperature * 100) / 100.0);
	cJSON_AddNumberToObject(sensor_data, "humidity", round(msg->humidity * 100) / 100.0);
	cJSON_AddStringToObject(sensor_data, "probe_name", msg->probe_id); /* Changed from probe_id to probe_name for clarity */
	cJSON_AddNumberToObject(sensor_data, "probe_battery", round(msg->probe_battery * 10) / 10.0);
	
	/* Add data quality indicators */
	cJSON_AddBoolToObject(data_quality, "temperature_valid", msg->data_quality.temperature_valid);
	cJSON_AddBoolToObject(data_quality, "humidity_valid", msg->data_quality.humidity_valid);
	cJSON_AddBoolToObject(data_quality, "battery_valid", msg->data_quality.battery_valid);
	cJSON_AddBoolToObject(data_quality, "probe_name_valid", msg->data_quality.probe_id_valid);
	
#if defined(CONFIG_APP_UART_SENSOR_TIMESTAMP)
	if (msg->timestamp > 0) {
		cJSON_AddNumberToObject(sensor_data, "sensor_timestamp", msg->timestamp);
	}
#endif
	
	/* Add error information if present */
	if (msg->error_type != UART_SENSOR_ERROR_NONE) {
		cJSON_AddNumberToObject(sensor_data, "error_type", msg->error_type);
		if (strlen(msg->error_details) > 0) {
			cJSON_AddStringToObject(sensor_data, "error_details", msg->error_details);
		}
	}
	
	/* Attach sub-objects to main JSON */
	cJSON_AddItemToObject(json, "data", sensor_data);
	cJSON_AddItemToObject(json, "quality", data_quality);
	
	/* Convert to string and publish */
	int ret = safe_publish_json(json, "uart_sensor");
	if (ret == 0) {
		LOG_INF("UART sensor data published: %s, T=%.1f°C, H=%.1f%%, Bat=%.1f%% (Quality: T:%s, H:%s, B:%s)", 
			msg->probe_id, (double)msg->temperature, (double)msg->humidity, (double)msg->probe_battery,
			msg->data_quality.temperature_valid ? "OK" : "ERR",
			msg->data_quality.humidity_valid ? "OK" : "ERR",
			msg->data_quality.battery_valid ? "OK" : "ERR");
	}

cleanup:
	if (json) {
		cJSON_Delete(json);
	}
}
#endif

#if defined(CONFIG_APP_BUTTON)
static void process_button_msg(const struct button_msg *msg)
{
	LOG_INF("Button %d %s detected", msg->button_number,
		msg->type == BUTTON_PRESS_SHORT ? "short press" : "long press");

	if (msg->button_number != 1) {
		return;
	}

	if (msg->type == BUTTON_PRESS_SHORT) {
		/* Short press: exactly one power sample + one ESL NUS sensors snapshot.
		 * Do NOT call process_power_data() directly — let POWER_CHAN publish once. */
		LOG_INF("Button: requesting power + ESL sensor snapshot");

		int ret = power_sample_request();
		if (ret != 0) {
			LOG_WRN("Failed to request power: %d", ret);
		}

#if defined(CONFIG_APP_UART_SENSOR)
		int n = uart_sensor_esl_get_tag_count();
		if (n > 0) {
			for (int i = 1; i <= n; i++) {
				ret = uart_sensor_esl_nus_sensors(i);
				if (ret != 0) {
					LOG_WRN("Failed to poll ESL tag %d sensors: %d", i, ret);
				}
			}
			LOG_INF("ESL NUS sensors poll triggered for %d tag(s)", n);
		} else {
			LOG_INF("No ESL tags known — only power data will be published");
		}
#endif
	}
	/* Long press: reserved for future use */
}
#endif

static void process_network_msg(const struct network_msg *msg)
{
	switch (msg->type) {
	case NETWORK_CONNECTED:
		LOG_INF("Network connected");
		mqtt_ctx.network_connected = true;
		k_work_schedule(&mqtt_ctx.connect_work, K_SECONDS(2));
		break;
		
	case NETWORK_DISCONNECTED:
		LOG_INF("Network disconnected");
		mqtt_ctx.network_connected = false;
		if (mqtt_ctx.state == MQTT_STATE_CONNECTED) {
			smf_set_state(&sm_ctx, &mqtt_states[MQTT_STATE_DISCONNECTING]);
		}
		break;
		
	default:
		break;
	}
}

	/* Main thread function */
static void custom_mqtt_thread(void)
{
	const struct zbus_channel *chan;
	int ret;

	LOG_INF("Custom MQTT module started");
	LOG_INF("MQTT Broker: %s:%d", MQTT_BROKER_HOSTNAME, MQTT_BROKER_PORT);
	LOG_INF("MQTT Username: %s", MQTT_USERNAME);
	LOG_INF("MQTT Topics - Publish: %s, Subscribe: %s", MQTT_PUB_TOPIC, MQTT_SUB_TOPIC);

	/* Initialize state machine */
	smf_set_initial(&sm_ctx, &mqtt_states[MQTT_STATE_IDLE]);

	/* Check if network is already connected at startup */
	struct network_msg network_status;
	int network_ret = zbus_chan_read(&NETWORK_CHAN, &network_status, K_MSEC(100));
	if (network_ret == 0) {
		if (network_status.type == NETWORK_CONNECTED) {
			LOG_INF("Network already connected at startup");
			mqtt_ctx.network_connected = true;
		}
	} else {
		LOG_DBG("Could not read initial network status: %d", network_ret);
		/* Assume network might be available and try to connect in a few seconds */
		k_work_schedule(&mqtt_ctx.connect_work, K_SECONDS(5));
	}

	while (1) {
		/* Wait for messages on subscribed channels */
		const void *msg_data;
		/* Short timeout — smf_run_state at bottom drives mqtt_input/mqtt_live.
		 * Must run frequently so incoming MQTT messages are read promptly. */
		ret = zbus_sub_wait_msg(&custom_mqtt_subscriber, &chan, &msg_data, K_MSEC(100));
		if (ret == 0) {
			/* Process messages with proper synchronization and retry logic */
			if (chan == &NETWORK_CHAN) {
				struct network_msg msg;
				ret = zbus_chan_read(&NETWORK_CHAN, &msg, K_MSEC(100));
				if (ret == 0) {
					k_mutex_lock(&mqtt_ctx.data_mutex, K_FOREVER);
					process_network_msg(&msg);
					k_mutex_unlock(&mqtt_ctx.data_mutex);
				} else {
					LOG_WRN("Failed to read NETWORK_CHAN: %d", ret);
				}
			}
#if defined(CONFIG_APP_ENVIRONMENTAL)
			else if (chan == &ENVIRONMENTAL_CHAN) {
				struct environmental_msg msg;
				ret = zbus_chan_read(&ENVIRONMENTAL_CHAN, &msg, K_MSEC(100));
				if (ret == 0) {
					k_mutex_lock(&mqtt_ctx.data_mutex, K_FOREVER);
					process_environmental_data(&msg);
					k_mutex_unlock(&mqtt_ctx.data_mutex);
				} else {
					/* Channel busy is common, only warn on other errors */
					if (ret != -EBUSY) {
						LOG_WRN("Failed to read ENVIRONMENTAL_CHAN: %d", ret);
					} else {
						LOG_DBG("ENVIRONMENTAL_CHAN busy, will retry");
					}
				}
			}
#endif
#if defined(CONFIG_APP_POWER)
			else if (chan == &POWER_CHAN) {
				struct power_msg msg;
				ret = zbus_chan_read(&POWER_CHAN, &msg, K_MSEC(100));
				if (ret == 0) {
					k_mutex_lock(&mqtt_ctx.data_mutex, K_FOREVER);
					process_power_data(&msg);
					k_mutex_unlock(&mqtt_ctx.data_mutex);
					LOG_DBG("ZBUS power data processed: %.1f%%", msg.percentage);
				} else {
					if (ret != -EBUSY) {
						LOG_WRN("Failed to read POWER_CHAN: %d", ret);
					} else {
						LOG_DBG("POWER_CHAN busy, will retry");
					}
				}
			}
#endif
#if defined(CONFIG_APP_UART_SENSOR)
			else if (chan == &UART_SENSOR_CHAN) {
				struct uart_sensor_msg msg;
				ret = zbus_chan_read(&UART_SENSOR_CHAN, &msg, K_MSEC(100));
				if (ret == 0) {
					k_mutex_lock(&mqtt_ctx.data_mutex, K_FOREVER);
					process_uart_sensor_data(&msg);
					k_mutex_unlock(&mqtt_ctx.data_mutex);
					LOG_DBG("ZBUS UART sensor data processed: %s, T=%.1f°C", 
						msg.probe_id, (double)msg.temperature);
				} else {
					if (ret != -EBUSY) {
						LOG_WRN("Failed to read UART_SENSOR_CHAN: %d", ret);
					} else {
						LOG_DBG("UART_SENSOR_CHAN busy, will retry");
					}
				}
			}
#endif
#if defined(CONFIG_APP_BUTTON)
			else if (chan == &BUTTON_CHAN) {
				struct button_msg msg;
				ret = zbus_chan_read(&BUTTON_CHAN, &msg, K_MSEC(100));
				if (ret == 0) {
					k_mutex_lock(&mqtt_ctx.data_mutex, K_FOREVER);
					process_button_msg(&msg);
					k_mutex_unlock(&mqtt_ctx.data_mutex);
				} else {
					if (ret != -EBUSY) {
						LOG_WRN("Failed to read BUTTON_CHAN: %d", ret);
					} else {
						LOG_DBG("BUTTON_CHAN busy, will retry");
					}
				}
			}
#endif
		}

		/* Run state machine */
		smf_run_state(&sm_ctx);
	}
}

/* Define and start the thread */
K_THREAD_DEFINE(custom_mqtt_thread_id, 
		CONFIG_APP_CUSTOM_MQTT_THREAD_STACK_SIZE,
		custom_mqtt_thread, 
		NULL, NULL, NULL, 
		K_PRIO_COOP(7), 0, 0);

/* Subscribe to channels */
static int custom_mqtt_init(void)
{
	/* Initialize mutex for thread safety */
	k_mutex_init(&mqtt_ctx.data_mutex);

	/* Initialize runtime MQTT config from compile-time Kconfig defaults */
	strncpy(mqtt_rt_cfg.host, MQTT_BROKER_HOSTNAME_DEFAULT, sizeof(mqtt_rt_cfg.host) - 1);
	mqtt_rt_cfg.port = MQTT_BROKER_PORT_DEFAULT;
	strncpy(mqtt_rt_cfg.username, MQTT_USERNAME_DEFAULT, sizeof(mqtt_rt_cfg.username) - 1);
	strncpy(mqtt_rt_cfg.password, MQTT_PASSWORD_DEFAULT, sizeof(mqtt_rt_cfg.password) - 1);
	strncpy(mqtt_rt_cfg.client_id, MQTT_CLIENT_ID_DEFAULT, sizeof(mqtt_rt_cfg.client_id) - 1);
	strncpy(mqtt_rt_cfg.pub_topic, MQTT_PUB_TOPIC_DEFAULT, sizeof(mqtt_rt_cfg.pub_topic) - 1);
	strncpy(mqtt_rt_cfg.sub_topic, MQTT_SUB_TOPIC_DEFAULT, sizeof(mqtt_rt_cfg.sub_topic) - 1);
	mqtt_rt_cfg.tls_enabled = IS_ENABLED(CONFIG_APP_CUSTOM_MQTT_USE_TLS);
#if defined(CONFIG_APP_CUSTOM_MQTT_USE_TLS)
	mqtt_rt_cfg.sec_tag = CONFIG_APP_CUSTOM_MQTT_SEC_TAG;
#else
	mqtt_rt_cfg.sec_tag = 0;
#endif
	mqtt_rt_cfg.power_mode = POWER_MODE_NORMAL;

	/* Load persisted settings — overrides Kconfig defaults if previously saved */
	settings_load();

	LOG_INF("MQTT config: host=%s port=%u user=%s",
		mqtt_rt_cfg.host, mqtt_rt_cfg.port, mqtt_rt_cfg.username);

	/* Initialize work queue items */
	k_work_init_delayable(&mqtt_ctx.connect_work, connect_work_handler);
	k_work_init_delayable(&mqtt_ctx.data_send_work, data_send_work_handler);
	k_work_init_delayable(&mqtt_restart_work, mqtt_restart_work_fn);
	k_work_init_delayable(&mqtt_inactivity_work, mqtt_inactivity_work_fn);
#if defined(CONFIG_APP_LOCATION)
	k_work_init_delayable(&mqtt_ctx.location_trigger_work, location_trigger_work_handler);
#endif

	/* Initialize counters */
	mqtt_ctx.publish_sequence = 0;
	mqtt_ctx.publish_failures = 0;
	mqtt_ctx.data_validation_enabled = true;

	LOG_INF("Custom MQTT module initialized — build: " __DATE__ " " __TIME__);

	return 0;
}

SYS_INIT(custom_mqtt_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
