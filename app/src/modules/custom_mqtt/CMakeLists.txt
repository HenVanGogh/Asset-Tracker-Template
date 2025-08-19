#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

if(CONFIG_APP_CUSTOM_MQTT)
	target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/custom_mqtt.c)
	
	if(CONFIG_APP_CUSTOM_MQTT_SHELL)
		target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/custom_mqtt_shell.c)
	endif()
endif()
