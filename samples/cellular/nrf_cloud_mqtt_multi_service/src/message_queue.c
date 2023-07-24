/* Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <net/nrf_cloud.h>
#if defined(CONFIG_NRF_CLOUD_COAP)
#include <net/nrf_cloud_coap.h>
#endif
#include <date_time.h>
#include <zephyr/logging/log.h>
#include <net/nrf_cloud_codec.h>

#include "message_queue.h"
#include "cloud_connection.h"
#include "nrf_cloud_codec_internal.h"

#include "led_control.h"

LOG_MODULE_REGISTER(message_queue, CONFIG_MULTI_SERVICE_LOG_LEVEL);

/* Message Queue for enqueing outgoing messages during offline periods. */
K_MSGQ_DEFINE(device_message_queue,
	      sizeof(struct nrf_cloud_sensor_data *),
	      CONFIG_MAX_OUTGOING_MESSAGES,
	      sizeof(struct nrf_cloud_sensor_data *));

/* Tracks the number of consecutive message-send failures. A total count greater than
 * CONFIG_MAX_CONSECUTIVE_SEND_FAILURES will trigger a connection reset and cooldown.
 * Resets on every successful device message send.
 */
static int send_failure_count;

static void free_queued_dev_msg_message(struct nrf_cloud_sensor_data *msg);

static struct nrf_cloud_sensor_data *allocate_dev_msg_for_queue(struct nrf_cloud_sensor_data *
								msg_to_copy)
{
	if (!msg_to_copy) {
		return NULL;
	}
	LOG_DBG("type:%d, data_type:%d", msg_to_copy->type, msg_to_copy->data_type);

	struct nrf_cloud_sensor_data *new_msg = k_malloc(sizeof(struct nrf_cloud_sensor_data));

	if (new_msg == NULL) {
		LOG_ERR("Out of memory error");
		return NULL;
	}

	*new_msg = *msg_to_copy;
	if (msg_to_copy->data_type != NRF_CLOUD_DATA_TYPE_BLOCK) {
		return new_msg;
	}

	uint8_t *new_data = k_malloc(msg_to_copy->data.len);

	if (new_data == NULL) {
		LOG_ERR("Out of memory error");
		new_msg->data.ptr = NULL;
		new_msg->data.len = 0;
		k_free(new_msg);
		return NULL;
	}

	memcpy(new_data, msg_to_copy->data.ptr, msg_to_copy->data.len);
	new_msg->data.ptr = new_data;
	return new_msg;
}

static int enqueue_device_message(struct nrf_cloud_sensor_data *const msg, const bool create_copy)
{
	if (!msg) {
		return -EINVAL;
	}

	struct nrf_cloud_sensor_data *q_msg = msg;

	/* Acquire timestamp now, since data was just acquired */
	int err = date_time_now(&msg->ts_ms);

	if (err) {
		LOG_ERR("Failed to obtain current time, error %d", err);
		return -ETIME;
	}

	if (create_copy) {
		/* Allocate a new nrf_cloud_obj structure for the message queue.
		 * Copy the contents of msg_obj, which contains a pointer to the
		 * original message data, into the new structure.
		 */
		q_msg = allocate_dev_msg_for_queue(msg);
		if (!q_msg) {
			return -ENOMEM;
		}
	}

	/* Attempt to append data onto message queue. */
	LOG_DBG("Adding device message to queue");
	if (k_msgq_put(&device_message_queue, &q_msg, K_NO_WAIT)) {
		LOG_ERR("Device message rejected, outgoing message queue is full");
		if (create_copy) {
			free_queued_dev_msg_message(q_msg);
		}
		return -ENOMEM;
	}

	return 0;
}

static void free_queued_dev_msg_message(struct nrf_cloud_sensor_data *msg)
{
	if (msg == NULL) {
		return;
	}
	/* Free the data block attached to the msg */
	if (msg->data_type == NRF_CLOUD_DATA_TYPE_BLOCK) {
		LOG_DBG("Freeing msg block");
		k_free((uint8_t *)msg->data.ptr);
		msg->data.ptr = NULL;
		msg->data.len = 0;
	}
	/* Free the msg itself */
	LOG_DBG("Freeing msg");
	k_free(msg);
}

#if defined(CONFIG_NRF_CLOUD_MQTT)
/**
 * @brief Construct a device message object with automatically generated timestamp
 *
 * The resultant JSON object will be conformal to the General Message Schema described in the
 * application-protocols repo:
 *
 * https://github.com/nRFCloud/application-protocols
 *
 * @param msg - The object to contain the message
 * @param appid - The appId for the device message
 * @param data - The contents to encode
 * @return int - 0 on success, negative error code otherwise.
 */
static int encode_device_message(struct nrf_cloud_obj *msg,
				 const char *const appid,
				 struct nrf_cloud_sensor_data *const data)
{
	int err;

	/* Create message object */
	err = nrf_cloud_obj_msg_init(msg, appid, NRF_CLOUD_JSON_MSG_TYPE_VAL_DATA);
	if (err) {
		LOG_ERR("Failed to initialize message with appid %s", appid);
		return err;
	}

	/* Add timestamp to message object */
	err = nrf_cloud_obj_ts_add(msg, data->ts_ms);
	if (err) {
		LOG_ERR("Failed to add timestamp to data message with appid %s", appid);
		return err;
	}

	switch (data->data_type) {
	case NRF_CLOUD_DATA_TYPE_BLOCK:
		if ((data->type != NRF_CLOUD_SENSOR_GNSS) ||
		    (data->data.ptr == NULL) ||
		    (data->data.len != sizeof(struct nrf_cloud_gnss_data))) {
			err = -ENOTSUP;
			break;
		}
		err = nrf_cloud_obj_gnss_msg_create(msg,
						    (struct nrf_cloud_gnss_data *)data->data.ptr);
		break;
	case NRF_CLOUD_DATA_TYPE_STR:
		err = nrf_cloud_obj_str_add(msg, NRF_CLOUD_JSON_DATA_KEY,
					    data->str_val, false);
		break;
	case NRF_CLOUD_DATA_TYPE_INT:
		err = nrf_cloud_obj_num_add(msg, NRF_CLOUD_JSON_DATA_KEY,
					    (double)(data->int_val), false);
		break;
	case NRF_CLOUD_DATA_TYPE_DOUBLE:
		err = nrf_cloud_obj_num_add(msg, NRF_CLOUD_JSON_DATA_KEY,
					    data->double_val, false);
		break;
	}
	return err;
}

static void free_encoded_message(struct nrf_cloud_obj *msg_obj)
{
	LOG_DBG("Freeing nrf_cloud_obj body");
	/* Free the memory pointed to by the msg_obj struct */
	nrf_cloud_obj_free(msg_obj);
}
#endif /* CONFIG_NRF_CLOUD_MQTT */

/**
 * @brief Consume (attempt to send) a single device message from the device message queue.
 *	  Waits for nRF Cloud readiness before sending each message.
 *	  If the message fails to send, it will be reenqueued.
 *
 * @return int - 0 on success, otherwise negative error code.
 */
static int consume_device_message(void)
{
	struct nrf_cloud_sensor_data *queued_msg;
	int ret;
	const char *app_id;

	LOG_DBG("Consuming an enqueued device message");

	/* Wait until a message is available to send. */
	ret = k_msgq_get(&device_message_queue, &queued_msg, K_FOREVER);
	if (ret) {
		LOG_ERR("Failed to retrieve item from outgoing message queue, error: %d", ret);
		return -ret;
	}
	if (queued_msg == NULL) {
		return -ENOMSG;
	}
	if (queued_msg->app_id) {
		app_id = queued_msg->app_id;
	} else {
		app_id = nrf_cloud_sensor_app_id_lookup(queued_msg->type);
		if (app_id == NULL) {
			return -EINVAL;
		}
	}

	/* Wait until we are able to send it. */
	LOG_DBG("Waiting for valid connection before transmitting device message");
	(void)await_cloud_ready(K_FOREVER);

	/* Attempt to send it.
	 *
	 * Note, it is possible (and better) to batch-send device messages when more than one is
	 * queued up. We limit this sample to sending individual messages mainly to keep the sample
	 * simple and accessible. See the Asset Tracker V2 application for an example of batch
	 * message sending.
	 */
	LOG_DBG("Attempting to transmit enqueued device message type:%d, data_type:%d, app_id:%s",
		queued_msg->type, queued_msg->data_type, app_id);

#if defined(CONFIG_NRF_CLOUD_MQTT)
	NRF_CLOUD_OBJ_JSON_DEFINE(msg_obj);

	ret = encode_device_message(&msg_obj, app_id, queued_msg);
	if (ret) {
		LOG_ERR("Error encoding message: %d", ret);
		free_queued_dev_msg_message(queued_msg);
		return ret;
	}

	struct nrf_cloud_tx_data mqtt_msg = {
		.qos = MQTT_QOS_1_AT_LEAST_ONCE,
		.topic_type = NRF_CLOUD_TOPIC_MESSAGE,
		.obj = &msg_obj
	};

	/* Send message */
	ret = nrf_cloud_send(&mqtt_msg);

	free_encoded_message(&msg_obj);
	mqtt_msg.obj = NULL;

#elif defined(CONFIG_NRF_CLOUD_COAP)

	/* Send message */
	switch (queued_msg->data_type) {
	case NRF_CLOUD_DATA_TYPE_BLOCK:
		if ((queued_msg->type == NRF_CLOUD_SENSOR_GNSS) &&
		    (queued_msg->data.ptr != NULL) &&
		    (queued_msg->data.len == sizeof(struct nrf_cloud_gnss_data))) {
			ret = nrf_cloud_coap_location_send((const struct nrf_cloud_gnss_data *)
							   queued_msg->data.ptr);
		}
		break;
	case NRF_CLOUD_DATA_TYPE_STR:
		/* TODO -- implement public interface for coap string messages */
		break;
	case NRF_CLOUD_DATA_TYPE_DOUBLE:
		ret = nrf_cloud_coap_sensor_send(app_id, queued_msg->double_val, queued_msg->ts_ms);
		break;
	case NRF_CLOUD_DATA_TYPE_INT:
		ret = nrf_cloud_coap_sensor_send(app_id, (double)queued_msg->int_val,
						 queued_msg->ts_ms);
		break;
	}

#endif /* CONFIG_NRF_CLOUD_COAP */

	if (ret) {
		LOG_ERR("Transmission of enqueued device message failed, nrf_cloud_send "
			"gave error: %d. The message will be re-enqueued and tried again "
			"later.", ret);

		/* Re-enqueue the message for later retry.
		 * No need to create a copy since we already copied the
		 * message object struct when it was first enqueued.
		 */
		ret = enqueue_device_message(queued_msg, false);
		if (ret) {
			LOG_ERR("Could not re-enqueue message, discarding.");
			free_queued_dev_msg_message(queued_msg);
		}

		/* Increment the failure counter. */
		send_failure_count += 1;

		/* If we have failed too many times in a row, there is likely a bigger problem,
		 * and we should reset our connection to nRF Cloud, and wait for a few seconds.
		 */
		if (send_failure_count > CONFIG_MAX_CONSECUTIVE_SEND_FAILURES) {
			/* Disconnect. */
			disconnect_cloud();

			/* Wait for a few seconds before trying again. */
			k_sleep(K_SECONDS(CONFIG_CONSECUTIVE_SEND_FAILURE_COOLDOWN_SECONDS));
		}
	} else {
		/* Clean up the message receive from the queue */
		free_queued_dev_msg_message(queued_msg);

		LOG_DBG("Enqueued device message consumed successfully");

		/* Either overwrite the existing pattern with a short success pattern, or just
		 * disable the previously requested pattern, depending on if we are in verbose mode.
		 */
		if (IS_ENABLED(CONFIG_LED_VERBOSE_INDICATION)) {
			short_led_pattern(LED_SUCCESS);
		} else {
			stop_led_pattern();
		}

		/* Reset the failure counter, since we succeeded. */
		send_failure_count = 0;
	}

	return ret;
}

int send_device_message(struct nrf_cloud_sensor_data *const msg)
{
	/* Enqueue the message, creating a copy to be managed by the queue. */
	int ret = enqueue_device_message(msg, true);

	if (ret) {
		LOG_ERR("Cannot add message to queue");
	}

	return ret;
}

void message_queue_thread_fn(void)
{
	/* Continually attempt to consume device messages */
	while (true) {
		(void) consume_device_message();
	}
}
