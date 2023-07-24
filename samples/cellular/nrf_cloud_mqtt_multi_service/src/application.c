/* Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <modem/location.h>
#include <date_time.h>
#include <stdio.h>
#include <net/nrf_cloud.h>
#include <net/nrf_cloud_codec.h>
#include <net/nrf_cloud_log.h>
#include <net/nrf_cloud_alert.h>
#if defined(CONFIG_NRF_CLOUD_COAP)
#include <net/nrf_cloud_coap.h>
#include "handle_fota.h"
#endif

#include "application.h"
#include "temperature.h"
#include "cloud_connection.h"
#include "message_queue.h"
#include "location_tracking.h"
#include "led_control.h"
#include "at_commands.h"

LOG_MODULE_REGISTER(application, CONFIG_MULTI_SERVICE_LOG_LEVEL);

/* Timer used to time the sensor sampling rate. */
static K_TIMER_DEFINE(sensor_sample_timer, NULL, NULL);

#if defined(CONFIG_AT_CMD_REQUESTS)
/* AT command request error handling */
#define AT_CMD_REQUEST_ERR_FORMAT "Error while processing AT command request: %d"
#define AT_CMD_REQUEST_ERR_MAX_LEN (sizeof(AT_CMD_REQUEST_ERR_FORMAT) + 20)
BUILD_ASSERT(CONFIG_AT_CMD_REQUEST_RESPONSE_BUFFER_LENGTH >= AT_CMD_REQUEST_ERR_MAX_LEN,
	     "Not enough AT command response buffer for printing error events.");
#endif /* CONFIG_AT_CMD_REQUESTS */

/* Temperature alert limits. */
#define TEMP_ALERT_LIMIT ((float)CONFIG_TEMP_ALERT_LIMIT)
#define TEMP_ALERT_HYSTERESIS 1.5f
#define TEMP_ALERT_LOWER_LIMIT (TEMP_ALERT_LIMIT - TEMP_ALERT_HYSTERESIS)

/**
 * @brief Transmit a collected GNSS sample to nRF Cloud.
 *
 * @param loc_gnss - GNSS location data.
 * @return int - 0 on success, negative error code otherwise.
 */
static int send_gnss(const struct location_event_data * const loc_gnss)
{
	if (!loc_gnss || (loc_gnss->method != LOCATION_METHOD_GNSS)) {
		return -EINVAL;
	}

	struct nrf_cloud_gnss_data gnss_pvt = {
		.type = NRF_CLOUD_GNSS_TYPE_PVT,
		.ts_ms = NRF_CLOUD_NO_TIMESTAMP,
		.pvt = {
			.lon		= loc_gnss->location.longitude,
			.lat		= loc_gnss->location.latitude,
			.accuracy	= loc_gnss->location.accuracy,
			.has_alt	= 0,
			.has_speed	= 0,
			.has_heading	= 0
		}
	};
	struct nrf_cloud_sensor_data data = {
		.data.ptr = &gnss_pvt,
		.data.len = sizeof(gnss_pvt),
		.data_type = NRF_CLOUD_DATA_TYPE_BLOCK,
		.type = NRF_CLOUD_SENSOR_GNSS,
		.app_id = NULL
	};

	LOG_INF("Sending GNSS location...");
	return send_device_message(&data);
}

/**
 * @brief Callback to receive periodic location updates from location_tracking.c and forward them
 * to nRF Cloud.
 *
 * Note that cellular positioning (MCELL/Multi-Cell and SCELL/Single-Cell) is sent to nRF
 * Cloud automatically (since the Location library and nRF Cloud must work together to
 * determine them in the first place). GNSS positions, on the other hand, must be
 * sent manually, since they are determined entirely on-device.
 *
 * @param location_data - The received location update.
 *
 */
static void on_location_update(const struct location_event_data * const location_data)
{
	LOG_INF("Location Updated: %.06f N %.06f W, accuracy: %.01f m, Method: %s",
		location_data->location.latitude,
		location_data->location.longitude,
		location_data->location.accuracy,
		location_method_str(location_data->method));

	/* If the position update was derived using GNSS, send it onward to nRF Cloud. */
	if (location_data->method == LOCATION_METHOD_GNSS) {
		LOG_INF("GNSS Position Update! Sending to nRF Cloud...");
		send_gnss(location_data);
	}
}

#if 0 // defined(CONFIG_AT_CMD_REQUESTS)
/**
 * @brief Receives general device messages from nRF Cloud, checks if they are AT command requests,
 * and performs them if so, transmitting the modem response back to nRF Cloud.
 *
 * Try sending {"appId":"MODEM", "messageType":"CMD", "data":"AT+CGMR"}
 * in the nRF Cloud Portal Terminal card.
 *
 * @param msg - The device message to check.
 */
static void handle_at_cmd_requests(const struct nrf_cloud_data *const dev_msg)
{
	char *cmd;
	struct nrf_cloud_obj msg_obj;
	int err = nrf_cloud_obj_input_decode(&msg_obj, dev_msg);

	if (err) {
		/* The message isn't JSON or otherwise couldn't be parsed. */
		LOG_DBG("A general topic device message of length %d could not be parsed.",
			dev_msg->len);
		return;
	}

	/* Confirm app ID and message type */
	err = nrf_cloud_obj_msg_check(&msg_obj, NRF_CLOUD_JSON_APPID_VAL_MODEM,
				      NRF_CLOUD_JSON_MSG_TYPE_VAL_CMD);
	if (err) {
		goto cleanup;
	}

	/* Get the command string */
	err = nrf_cloud_obj_str_get(&msg_obj, NRF_CLOUD_JSON_DATA_KEY, &cmd);
	if (err) {
		/* Missing or invalid command value will be treated as a blank command */
		cmd = "";
	}

	/* Execute the command and receive the result */
	char *response = execute_at_cmd_request(cmd);

	/* To re-use msg_obj for the response message we must first free its memory and
	 * reset its state.
	 * The cmd string will no longer be valid after msg_obj is freed.
	 */
	cmd = NULL;
	/* Free the object's allocated memory */
	err = nrf_cloud_obj_free(&msg_obj);
	if (err) {
		LOG_ERR("Failed to free AT CMD request");
		return;
	}

	/* Reset the object's state */
	err = nrf_cloud_obj_reset(&msg_obj);
	if (err) {
		LOG_ERR("Failed to reset AT CMD request message object for reuse");
		return;
	}

	err = create_timestamped_device_message(&msg_obj, NRF_CLOUD_JSON_APPID_VAL_MODEM,
						NRF_CLOUD_JSON_MSG_TYPE_VAL_DATA);
	if (err) {
		return;
	}

	/* Populate response with command result */
	err = nrf_cloud_obj_str_add(&msg_obj, NRF_CLOUD_JSON_DATA_KEY, response, false);
	if (err) {
		LOG_ERR("Failed to populate AT CMD response with modem response data");
		goto cleanup;
	}

	/* Send the response */
	err = send_device_message(&msg_obj);
	if (err) {
		LOG_ERR("Failed to send AT CMD request response, error: %d", err);
	}

	return;

cleanup:
	(void)nrf_cloud_obj_free(&msg_obj);
}
#else
static void handle_at_cmd_requests(const struct nrf_cloud_data *const dev_msg)
{
}
#endif /* CONFIG_AT_CMD_REQUESTS */

#if defined(CONFIG_NRF_CLOUD_COAP)
static void check_shadow(void)
{
	int err;
	char buf[512];

	buf[0] = '\0';
	LOG_INF("Checking for shadow delta...");
	err = nrf_cloud_coap_shadow_get(buf, sizeof(buf), true);
	if (err) {
		LOG_ERR("Failed to request shadow delta: %d", err);
	} else {
		size_t len = strlen(buf);

		LOG_INF("Delta: len:%zd, %s", len, len ? buf : "None");
		/* Do something with the shadow delta's JSON data, such
		 * as parse it and use the decoded information to change a
		 * behavior.
		 */

		/* Acknowledge it so we do not receive it again. */
		if (len) {
			err = nrf_cloud_coap_shadow_state_update(buf);
			if (err) {
				LOG_ERR("Failed to acknowledge delta: %d", err);
			} else {
				LOG_INF("Delta acknowledged");
			}
		}
	}
}
#endif /* CONFIG_NRF_CLOUD_COAP */

/** @brief Check whether temperature is acceptable.
 * If the device exceeds a temperature limit, send the temperature alert one time.
 * Once the temperature falls below a lower limit, re-enable the temperature alert
 * so it will be sent if limit is exceeded again.
 *
 * The difference between the two limits should be sufficient to prevent sending
 * new alerts if the temperature value oscillates between two nearby values.
 *
 * @param temp - The current device temperature.
 */
static void monitor_temperature(double temp)
{
	static bool temperature_alert_active;

	if ((temp > TEMP_ALERT_LIMIT) && !temperature_alert_active) {
		temperature_alert_active = true;
		(void)nrf_cloud_alert_send(ALERT_TYPE_TEMPERATURE, (float)temp,
					   "Temperature over limit!");
		LOG_INF("Temperature limit %f C exceeded: now %f C.",
			TEMP_ALERT_LIMIT, temp);
	} else if ((temp < TEMP_ALERT_LOWER_LIMIT) && temperature_alert_active) {
		temperature_alert_active = false;
		LOG_INF("Temperature now below limit: %f C.", temp);
	}
}

void main_application_thread_fn(void)
{
	if (IS_ENABLED(CONFIG_AT_CMD_REQUESTS)) {
		/* Register with connection.c to receive general device messages and check them for
		 * AT command requests.
		 */
		register_general_dev_msg_handler(handle_at_cmd_requests);
	}

	/* Wait for first connection before starting the application. */
	(void)await_cloud_ready(K_FOREVER);

	(void)nrf_cloud_alert_send(ALERT_TYPE_DEVICE_NOW_ONLINE, 0, NULL);

	/* Wait for the date and time to become known.
	 * This is needed both for location services and for sensor sample timestamping.
	 */
	LOG_INF("Waiting for modem to determine current date and time");
	if (!await_date_time_known(K_SECONDS(CONFIG_DATE_TIME_ESTABLISHMENT_TIMEOUT_SECONDS))) {
		LOG_WRN("Failed to determine valid date time. Proceeding anyways");
	} else {
		LOG_INF("Current date and time determined");
	}

	nrf_cloud_log_init();
	nrf_cloud_log_control_set(CONFIG_NRF_CLOUD_LOG_OUTPUT_LEVEL);
	/* Send a direct log to the nRF Cloud web portal indicating the sample has started up. */
	(void)nrf_cloud_log_send(LOG_LEVEL_INF, "nRF Cloud MQTT multi-service sample v%s",
			   CONFIG_APP_VERSION);

	/* Begin tracking location at the configured interval. */
	(void)start_location_tracking(on_location_update,
					CONFIG_LOCATION_TRACKING_SAMPLE_INTERVAL_SECONDS);

	int counter = 0;

	/* Begin sampling sensors. */
	while (true) {
		/* Start the sensor sample interval timer.
		 * We use a timer here instead of merely sleeping the thread, because the
		 * application thread can be preempted by other threads performing long tasks
		 * (such as periodic location acquisition), and we want to account for these
		 * delays when metering the sample send rate.
		 */
		k_timer_start(&sensor_sample_timer,
			K_SECONDS(CONFIG_SENSOR_SAMPLE_INTERVAL_SECONDS), K_FOREVER);

		if (IS_ENABLED(CONFIG_TEMP_TRACKING)) {
			struct nrf_cloud_sensor_data data = {
				.app_id = NULL,
				.type = NRF_CLOUD_SENSOR_TEMP,
				.data_type = NRF_CLOUD_DATA_TYPE_DOUBLE,
			};

			if (get_temperature(&data.double_val) == 0) {
				LOG_INF("Temperature is %d degrees C", (int)data.double_val);
				LOG_DBG("Sending temperature...");
				(void)send_device_message(&data);
				LOG_DBG("Monitor temperature...");
				monitor_temperature(data.double_val);
			}
		}

		if (IS_ENABLED(CONFIG_TEST_COUNTER)) {
			struct nrf_cloud_sensor_data data = {
				.app_id = "COUNT",
				.data_type = NRF_CLOUD_DATA_TYPE_INT,
				.int_val = counter++
			};

			LOG_INF("Sent test counter = %d", data.int_val);
			(void)send_device_message(&data);
		}

#if defined(CONFIG_NRF_CLOUD_COAP)
#if defined(CONFIG_NRF_CLOUD_COAP_FOTA)
		if (handle_fota_process() != -EAGAIN) {
			LOG_INF("FOTA check completed.");
		}
#endif /* CONFIG_NRF_CLOUD_COAP_FOTA */
		check_shadow();
#endif /* CONFIG_NRF_CLOUD_COAP */

		/* Wait out any remaining time on the sample interval timer. */
		k_timer_status_sync(&sensor_sample_timer);
	}
}
