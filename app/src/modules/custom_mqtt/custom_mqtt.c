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
#include <cJSON.h>
#include <date_time.h>
#include <arpa/inet.h>
#include <math.h>

#include "custom_mqtt.h"
#include "custom_mqtt_config.h"
#include "app_common.h"
#include "network.h"

#if defined(CONFIG_APP_LOCATION)
#include "location.h"
#endif

#if defined(CONFIG_APP_ENVIRONMENTAL)
#include "environmental.h"
#endif

#if defined(CONFIG_APP_POWER)
#include "power.h"
#endif

#if defined(CONFIG_APP_BUTTON)
#include "button.h"
#endif

/* Register log module */
LOG_MODULE_REGISTER(custom_mqtt, CONFIG_APP_CUSTOM_MQTT_LOG_LEVEL);

/* MQTT client configuration */
#define MQTT_BROKER_HOSTNAME CONFIG_APP_CUSTOM_MQTT_BROKER_HOSTNAME
#define MQTT_BROKER_PORT CONFIG_APP_CUSTOM_MQTT_BROKER_PORT
#define MQTT_CLIENT_ID "thingy91x-asset-tracker"
#define MQTT_USERNAME CONFIG_APP_CUSTOM_MQTT_USERNAME
#define MQTT_PASSWORD CONFIG_APP_CUSTOM_MQTT_PASSWORD
#define MQTT_PUB_TOPIC CONFIG_APP_CUSTOM_MQTT_PUBLISH_TOPIC
#define MQTT_SUB_TOPIC CONFIG_APP_CUSTOM_MQTT_SUBSCRIBE_TOPIC
#define MQTT_KEEPALIVE CONFIG_APP_CUSTOM_MQTT_KEEPALIVE_SECONDS

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

/* MQTT client buffers */
static uint8_t mqtt_client_id[] = MQTT_CLIENT_ID;

/* Security tag for TLS */
static sec_tag_t sec_tag_list[] = { CONFIG_APP_CUSTOM_MQTT_SEC_TAG };

/* Register zbus subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(custom_mqtt_subscriber);

/* Subscribe to channels */
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, custom_mqtt_subscriber, 0);
#if defined(CONFIG_APP_LOCATION)
ZBUS_CHAN_ADD_OBS(LOCATION_CHAN, custom_mqtt_subscriber, 0);
#endif
#if defined(CONFIG_APP_ENVIRONMENTAL)
ZBUS_CHAN_ADD_OBS(ENVIRONMENTAL_CHAN, custom_mqtt_subscriber, 0);
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
static int custom_mqtt_connect(void);
static int custom_mqtt_disconnect(void);
static int mqtt_publish_data(const char *data, size_t len);

/* Data validation helpers */
static bool validate_sensor_data(double value, double min, double max);
static bool validate_json_string(const char *json_str);
static int safe_publish_json(cJSON *json, const char *data_type);

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
#if defined(CONFIG_APP_BUTTON) && defined(MQTT_BUTTON_POWER_MEASUREMENT_ENABLED)
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
		LOG_INF("MQTT message received on topic: %.*s",
			evt->param.publish.message.topic.topic.size,
			evt->param.publish.message.topic.topic.utf8);
		
		/* Validate payload size */
		if (evt->param.publish.message.payload.len >= MQTT_PAYLOAD_BUF_SIZE) {
			LOG_WRN("Received message too large: %u bytes, truncating",
				evt->param.publish.message.payload.len);
		}
		
		/* Copy received data to message with bounds checking */
		size_t len = MIN(evt->param.publish.message.payload.len, 
				 MQTT_PAYLOAD_BUF_SIZE - 1);
		
		if (evt->param.publish.message.payload.data != NULL && len > 0) {
			memcpy(mqtt_ctx.payload_buf, evt->param.publish.message.payload.data, len);
			mqtt_ctx.payload_buf[len] = '\0';
			
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
					cJSON *command = cJSON_GetObjectItem(received_json, "command");
					if (command && cJSON_IsString(command)) {
						LOG_INF("Processing command: %s", command->valuestring);
						cJSON_AddStringToObject(response, "command_processed", command->valuestring);
						cJSON_AddStringToObject(response, "status", "command_received");
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
			
			msg.type = CUSTOM_MQTT_EVT_DATA_RECEIVED;
			msg.data_received.data = (char *)mqtt_ctx.payload_buf;
			msg.data_received.len = len;
			zbus_chan_pub(&CUSTOM_MQTT_CHAN, &msg, K_NO_WAIT);
		} else {
			LOG_WRN("Received message with invalid payload");
		}
		break;

	case MQTT_EVT_PUBACK:
		LOG_DBG("MQTT publish acknowledged (message_id: %u)", evt->param.puback.message_id);
		/* Reset failure counter on successful publish */
		if (mqtt_ctx.publish_failures > 0) {
			mqtt_ctx.publish_failures = MAX(0, mqtt_ctx.publish_failures - 1);
		}
		break;

	case MQTT_EVT_SUBACK:
		LOG_INF("MQTT subscription acknowledged (message_id: %u)", evt->param.suback.message_id);
		break;

	case MQTT_EVT_UNSUBACK:
		LOG_INF("MQTT unsubscription acknowledged");
		break;

	case MQTT_EVT_PINGRESP:
		LOG_DBG("MQTT ping response received");
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

static void data_send_work_handler(struct k_work *work)
{
	if (mqtt_ctx.state == MQTT_STATE_CONNECTED) {
		k_mutex_lock(&mqtt_ctx.data_mutex, K_FOREVER);
		
		cJSON *json = cJSON_CreateObject();
		if (json) {
			cJSON_AddStringToObject(json, "device_id", MQTT_CLIENT_ID);
			cJSON_AddStringToObject(json, "type", "heartbeat");
			cJSON_AddNumberToObject(json, "timestamp", k_uptime_get());
			cJSON_AddNumberToObject(json, "uptime_ms", k_uptime_get());
			cJSON_AddStringToObject(json, "firmware_version", "v0.0.0-dev");
			cJSON_AddNumberToObject(json, "sequence", mqtt_ctx.publish_sequence + 1);
			
		/* Add diagnostic information */
		cJSON *diagnostics = cJSON_CreateObject();
		if (diagnostics) {
			cJSON_AddNumberToObject(diagnostics, "publish_failures", mqtt_ctx.publish_failures);
			cJSON_AddNumberToObject(diagnostics, "total_publishes", mqtt_ctx.publish_sequence);
			cJSON_AddBoolToObject(diagnostics, "network_connected", mqtt_ctx.network_connected);
			cJSON_AddNumberToObject(diagnostics, "mqtt_state", mqtt_ctx.state);
			cJSON_AddItemToObject(json, "diagnostics", diagnostics);
		}
		
		char *json_string = cJSON_Print(json);
			if (json_string) {
				int ret = mqtt_publish_data(json_string, strlen(json_string));
				if (ret == 0) {
					LOG_INF("Heartbeat message sent (seq: %u, failures: %u)", 
						mqtt_ctx.publish_sequence, mqtt_ctx.publish_failures);
				} else {
					LOG_ERR("Failed to send heartbeat: %d", ret);
				}
				cJSON_free(json_string);
			}
			cJSON_Delete(json);
		}
		
		k_mutex_unlock(&mqtt_ctx.data_mutex);
		
		/* Schedule next heartbeat */
		k_work_schedule(&mqtt_ctx.data_send_work, K_SECONDS(MQTT_HEARTBEAT_INTERVAL_SEC));
	}
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
	mqtt_ctx.client.client_id.utf8 = mqtt_client_id;
	mqtt_ctx.client.client_id.size = strlen(mqtt_client_id);
	mqtt_ctx.client.protocol_version = MQTT_VERSION_3_1_1;
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

	LOG_INF("Configuring TLS settings");
	/* Configure TLS */
	mqtt_ctx.client.transport.type = MQTT_TRANSPORT_SECURE;
	
	struct mqtt_sec_config *tls_config = &mqtt_ctx.client.transport.tls.config;
	tls_config->peer_verify = TLS_PEER_VERIFY_NONE;  /* Disable peer verification for now */
	tls_config->cipher_list = NULL;
	
	/* Use security tags if configured, otherwise use system CA certificates */
	if (CONFIG_APP_CUSTOM_MQTT_SEC_TAG > 0) {
		tls_config->sec_tag_count = ARRAY_SIZE(sec_tag_list);
		tls_config->sec_tag_list = sec_tag_list;
		LOG_INF("Using security tag: %d", CONFIG_APP_CUSTOM_MQTT_SEC_TAG);
	} else {
		tls_config->sec_tag_count = 0;
		tls_config->sec_tag_list = NULL;
		LOG_INF("Using system CA certificates");
	}
	
	tls_config->hostname = MQTT_BROKER_HOSTNAME;

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

static bool validate_json_string(const char *json_str)
{
	if (!json_str) {
		return false;
	}
	
	/* Basic validation - check if it's a valid JSON */
	cJSON *test = cJSON_Parse(json_str);
	if (!test) {
		LOG_ERR("Invalid JSON string");
		return false;
	}
	
	cJSON_Delete(test);
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
		if (validate_json_string(json_string)) {
			if (mqtt_ctx.state == MQTT_STATE_CONNECTED) {
				ret = mqtt_publish_data(json_string, strlen(json_string));
				if (ret == 0) {
					LOG_DBG("Successfully published %s data", data_type ? data_type : "JSON");
				} else {
					LOG_ERR("Failed to publish %s data: %d", data_type ? data_type : "JSON", ret);
				}
			} else {
				LOG_WRN("MQTT not connected, cannot publish %s data", data_type ? data_type : "JSON");
				ret = -ENOTCONN;
			}
		} else {
			LOG_ERR("JSON validation failed for %s data", data_type ? data_type : "JSON");
			ret = -EINVAL;
		}
		cJSON_free(json_string);
	} else {
		LOG_ERR("Failed to serialize %s JSON data", data_type ? data_type : "JSON");
	}
	
	return ret;
}

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
	if (ret < 0) {
		LOG_ERR("MQTT input error during connection: %d", ret);
		smf_set_state(&sm_ctx, &mqtt_states[MQTT_STATE_ERROR]);
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
	
	subscribe_topic.topic.utf8 = MQTT_SUB_TOPIC;
	subscribe_topic.topic.size = strlen(MQTT_SUB_TOPIC);
	subscribe_topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
	
	subscription_list.list = &subscribe_topic;
	subscription_list.list_count = 1;
	subscription_list.message_id = 1;
	
	int ret = mqtt_subscribe(&mqtt_ctx.client, &subscription_list);
	if (ret) {
		LOG_ERR("Failed to subscribe to topic: %d", ret);
	} else {
		LOG_INF("Subscribed to topic: %s", MQTT_SUB_TOPIC);
	}
	
	/* Send initial connection message */
	k_sleep(K_MSEC(1000)); /* Give subscription time to complete */
	
	cJSON *json = cJSON_CreateObject();
	if (json) {
		cJSON_AddStringToObject(json, "device_id", MQTT_CLIENT_ID);
		cJSON_AddStringToObject(json, "status", "connected");
		cJSON_AddNumberToObject(json, "timestamp", k_uptime_get());
		cJSON_AddStringToObject(json, "message", "Device connected to MQTT broker");
		
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
	
	/* Start periodic data sending */
	k_work_schedule(&mqtt_ctx.data_send_work, K_SECONDS(10));
}

static void connected_run(void *obj)
{
	/* Poll MQTT client for events */
	int ret = mqtt_input(&mqtt_ctx.client);
	if (ret < 0 && ret != -EAGAIN) {
		LOG_ERR("MQTT input error: %d", ret);
		smf_set_state(&sm_ctx, &mqtt_states[MQTT_STATE_ERROR]);
		return;
	}
	
	/* Maintain MQTT connection */
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
	
	/* Reset failure counters for exponential backoff */
	static uint32_t reconnect_delay = MQTT_RECONNECT_BASE_DELAY_SEC;
	
	if (mqtt_ctx.publish_failures > MQTT_MAX_PUBLISH_FAILURES) {
		/* Exponential backoff for repeated failures */
		reconnect_delay = MIN(reconnect_delay * 2, MQTT_RECONNECT_MAX_DELAY_SEC);
	} else {
		reconnect_delay = MQTT_RECONNECT_BASE_DELAY_SEC; /* Reset on success */
	}
	
	LOG_WRN("MQTT error state, will retry connection in %u seconds", reconnect_delay);
	
	/* Schedule reconnection attempt */
	k_work_schedule(&mqtt_ctx.connect_work, K_SECONDS(reconnect_delay));
}

static void error_run(void *obj)
{
	/* Wait for reconnection attempt */
}

/* Message processing functions */
#if defined(CONFIG_APP_LOCATION)
static void process_location_data(const struct location_msg *msg)
{
	cJSON *json = cJSON_CreateObject();
	cJSON *location = cJSON_CreateObject();
	
	if (json == NULL || location == NULL) {
		LOG_ERR("Failed to create JSON objects");
		goto cleanup;
	}

	/* Validate location data */
	if (msg->gnss_data.latitude < -90.0 || msg->gnss_data.latitude > 90.0 ||
	    msg->gnss_data.longitude < -180.0 || msg->gnss_data.longitude > 180.0) {
		LOG_WRN("Invalid GPS coordinates: lat=%.6f, lng=%.6f, skipping",
			msg->gnss_data.latitude, msg->gnss_data.longitude);
		goto cleanup;
	}

	if (msg->gnss_data.accuracy > MQTT_GPS_ACCURACY_MAX_METERS) {
		LOG_WRN("GPS accuracy too low: %.2f m, skipping", msg->gnss_data.accuracy);
		goto cleanup;
	}

	/* Add device info and type */
	cJSON_AddStringToObject(json, "device_id", MQTT_CLIENT_ID);
	cJSON_AddStringToObject(json, "type", "location");
	cJSON_AddNumberToObject(json, "timestamp", k_uptime_get());
	cJSON_AddNumberToObject(json, "sequence", mqtt_ctx.publish_sequence + 1);
	
	/* Add location data */
	cJSON_AddNumberToObject(location, "lat", msg->gnss_data.latitude);
	cJSON_AddNumberToObject(location, "lng", msg->gnss_data.longitude);
	cJSON_AddNumberToObject(location, "acc", msg->gnss_data.accuracy);
	cJSON_AddItemToObject(json, "data", location);
	
	char *json_string = cJSON_Print(json);
	if (json_string != NULL) {
		if (mqtt_ctx.state == MQTT_STATE_CONNECTED) {
			int ret = mqtt_publish_data(json_string, strlen(json_string));
			if (ret == 0) {
				LOG_INF("Location data published: lat=%.6f, lng=%.6f, acc=%.2f",
					msg->gnss_data.latitude, msg->gnss_data.longitude, 
					msg->gnss_data.accuracy);
			}
		}
		cJSON_free(json_string);
	}

cleanup:
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
	
	/* Add power data - use the correct field name from power_msg struct */
	cJSON_AddNumberToObject(power_data, "percentage", round(msg->percentage * 10) / 10.0);
	
#if defined(CONFIG_APP_POWER_TIMESTAMP)
	if (msg->timestamp > 0) {
		cJSON_AddNumberToObject(power_data, "timestamp", msg->timestamp);
	}
#endif
	
	cJSON_AddItemToObject(json, "data", power_data);
	
	/* Use safe publish function */
	int ret = safe_publish_json(json, "power");
	if (ret == 0) {
		LOG_INF("Power data published: %.1f%%", msg->percentage);
	}

cleanup:
	if (json) {
		cJSON_Delete(json);
	}
}
#endif

#if defined(CONFIG_APP_BUTTON) && defined(MQTT_BUTTON_POWER_MEASUREMENT_ENABLED)
static void process_button_msg(const struct button_msg *msg)
{
	LOG_INF("Button %d %s detected", msg->button_number, 
		msg->type == BUTTON_PRESS_SHORT ? "short press" : "long press");
	
	/* Only handle button 1 short press for power measurement trigger */
	if (msg->button_number == 1 && msg->type == BUTTON_PRESS_SHORT) {
		LOG_INF("Requesting power measurement via button press");
		
		/* Request power measurement directly */
		int ret = power_sample_request();
		if (ret != 0) {
			LOG_ERR("Failed to request power measurement: %d", ret);
		} else {
			LOG_DBG("Power measurement request sent successfully");
		}
	}
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
		ret = zbus_sub_wait_msg(&custom_mqtt_subscriber, &chan, &msg_data, K_MSEC(1000));
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
#if defined(CONFIG_APP_LOCATION)
			else if (chan == &LOCATION_CHAN) {
				struct location_msg msg;
				ret = zbus_chan_read(&LOCATION_CHAN, &msg, K_MSEC(100));
				if (ret == 0) {
					k_mutex_lock(&mqtt_ctx.data_mutex, K_FOREVER);
					process_location_data(&msg);
					k_mutex_unlock(&mqtt_ctx.data_mutex);
				} else {
					LOG_WRN("Failed to read LOCATION_CHAN: %d", ret);
				}
			}
#endif
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
#if defined(CONFIG_APP_BUTTON) && defined(MQTT_BUTTON_POWER_MEASUREMENT_ENABLED)
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
	
	/* Initialize work queue */
	k_work_init_delayable(&mqtt_ctx.connect_work, connect_work_handler);
	k_work_init_delayable(&mqtt_ctx.data_send_work, data_send_work_handler);
	
	/* Initialize counters */
	mqtt_ctx.publish_sequence = 0;
	mqtt_ctx.publish_failures = 0;
	mqtt_ctx.data_validation_enabled = true;
	
	LOG_INF("Custom MQTT module initialized");
	
	return 0;
}

SYS_INIT(custom_mqtt_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
