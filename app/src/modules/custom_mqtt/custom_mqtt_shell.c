/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "custom_mqtt.h"

LOG_MODULE_REGISTER(custom_mqtt_shell, CONFIG_APP_CUSTOM_MQTT_LOG_LEVEL);

static int cmd_mqtt_status(const struct shell *shctx, size_t argc, char **argv)
{
	struct custom_mqtt_msg msg;
	int ret;

	ret = zbus_chan_read(&CUSTOM_MQTT_CHAN, &msg, K_NO_WAIT);
	if (ret) {
		shell_error(shctx, "Failed to read MQTT channel: %d", ret);
		return ret;
	}

	switch (msg.type) {
	case CUSTOM_MQTT_EVT_CONNECTED:
		shell_print(shctx, "MQTT Status: Connected");
		break;
	case CUSTOM_MQTT_EVT_DISCONNECTED:
		shell_print(shctx, "MQTT Status: Disconnected");
		break;
	case CUSTOM_MQTT_EVT_ERROR:
		shell_print(shctx, "MQTT Status: Error (code: %d)", msg.error.err_code);
		break;
	default:
		shell_print(shctx, "MQTT Status: Unknown (%d)", msg.type);
		break;
	}

	return 0;
}

static int cmd_mqtt_send(const struct shell *shctx, size_t argc, char **argv)
{
	struct custom_mqtt_msg msg;

	if (argc < 2) {
		shell_error(shctx, "Usage: mqtt send <message>");
		return -EINVAL;
	}

	msg.type = CUSTOM_MQTT_EVT_DATA_SEND;
	msg.data_send.data = argv[1];
	msg.data_send.len = strlen(argv[1]);

	int ret = zbus_chan_pub(&CUSTOM_MQTT_CHAN, &msg, K_MSEC(100));
	if (ret) {
		shell_error(shctx, "Failed to publish message: %d", ret);
		return ret;
	}

	shell_print(shctx, "Message sent: %s", argv[1]);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(mqtt_cmds,
	SHELL_CMD_ARG(status, NULL, "Show MQTT connection status", cmd_mqtt_status, 1, 0),
	SHELL_CMD_ARG(send, NULL, "Send message to MQTT broker", cmd_mqtt_send, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(mqtt, &mqtt_cmds, "Custom MQTT commands", NULL);
