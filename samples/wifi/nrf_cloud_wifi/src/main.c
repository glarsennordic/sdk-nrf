/*
 * Copyright (c) 2021 Nordic Semiconductor ASA.
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */


#include <zephyr/kernel.h>
#include <zephyr/random/rand32.h>

#include <stdio.h>
#include <stdint.h>
#include <date_time.h>
#include <cJSON.h>
#include <math.h>
#include <nrfx_clock.h>

#include <sys/printk.h>

#include <net/nrf_cloud.h>
#include <net/wifi_mgmt.h>
#include <net/net_if.h>
#include <net/net_core.h>
#include <net/net_context.h>
#include <net/net_mgmt.h>

#include <mbedtls/ssl_ciphersuites.h>
#include <zephyr/net/tls_credentials.h>

/* Regrettably, we must include src/utils/common because it defines u8, which the wpa_supplicant
 * includes rely upon
 *
 * Similarly, src/utils/common relies on <stdlib.h>, so we must include that as well
 */

#include <stdlib.h>
#include <src/utils/common.h>
#include <wpa_supplicant/config.h>
#include <wpa_supplicant/wpa_supplicant_i.h>


#include <logging/log.h>

LOG_MODULE_REGISTER(nrf_cloud_wifi_sample, CONFIG_NRF_CLOUD_WIFI_SAMPLE_LOG_LEVEL);

#define SEC_TAG CONFIG_NRF_CLOUD_SEC_TAG
#define TEMP_ID "TEMP"
#define HUMID_ID "HUMID"

/* WiFi supplicant access struct. Initialized by the WiFi driver itself.*/
extern struct wpa_supplicant *wpa_s_0;

static K_SEM_DEFINE(wifi_connected, 0, 1);
static K_SEM_DEFINE(cloud_connected, 0, 1);
static K_SEM_DEFINE(cloud_ready, 0, 1);



/**
 * define static arrays containing our credentials; in the future, these
 * should be stored in secure storage
 */
//TODO: What does this gate do?
#if defined(CONFIG_MBEDTLS_X509_CRT_PARSE_C)

static const unsigned char ca_certificate[] = {
	#include "ca_cert.h"
};

static const unsigned char client_certificate[] = {
	#include "client_cert.h"
};

static const unsigned char private_key[] = {
	#include "private_key.h"
};

#endif


static void wifi_event_handler(struct net_mgmt_event_callback *cb,
			       uint32_t mgmt_event, struct net_if *iface)
{
	switch (mgmt_event) {
	case NET_EVENT_WIFI_SCAN_RESULT:
		LOG_INF("Scan Result");
		break;
	case NET_EVENT_WIFI_SCAN_DONE:
		LOG_INF("Scan Done");
		break;
	case NET_EVENT_WIFI_CONNECT_RESULT:
		LOG_INF("Connect Result");
		break;
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		LOG_INF("Disconnect Result");
		break;
	case NET_EVENT_WIFI_IFACE_STATUS:
		LOG_INF("IFACE STATUS");
		const struct wifi_iface_status *iface_stat = (const struct wifi_iface_status *)cb->info;
		int state = iface_stat->state;
		LOG_INF("iface_stat->state: %s",
			state == WIFI_STATE_DISCONNECTED 	? "DISCONNECTED":
			state == WIFI_STATE_INTERFACE_DISABLED 	? "INTERFACE_DISABLED":
			state == WIFI_STATE_INACTIVE	 	? "INACTIVE":
			state == WIFI_STATE_SCANNING	 	? "SCANNING":
			state == WIFI_STATE_AUTHENTICATING	? "AUTHENTICATING":
			state == WIFI_STATE_ASSOCIATING	 	? "ASSOCIATING":
			state == WIFI_STATE_ASSOCIATED	 	? "ASSOCIATED":
			state == WIFI_STATE_4WAY_HANDSHAKE	? "4WAY_HANDSHAKE":
			state == WIFI_STATE_GROUP_HANDSHAKE	? "GROUP_HANDSHAKE":
			state == WIFI_STATE_COMPLETED	 	? "COMPLETED":
								  "INVALID");
		break;
	case NET_EVENT_IF_UP:
		LOG_INF("NET_EVENT_IF_UP");
		break;
	default:
		LOG_INF("Unknown Event %d", mgmt_event);
		break;
	}
	LOG_INF("Event %d", mgmt_event);
}


/**
 * @brief load CA, client cert, and private key to specified security tag
 * in the TLS stack.
 *
 * @param sec_tag the tag to load credentials into.
 * @return 0 on success or negative error code on failure.
 */
static int tls_load_credentials(int sec_tag)
{
	int ret;

	/* Load CA certificate */
	ret = tls_credential_add(sec_tag, TLS_CREDENTIAL_CA_CERTIFICATE,
				ca_certificate, sizeof(ca_certificate));
	if (ret != 0) {
		LOG_ERR("Failed to register CA certificate: %d", ret);
		goto exit;
	}

	/* Load server/client certificate */
	ret = tls_credential_add(sec_tag,
				TLS_CREDENTIAL_SERVER_CERTIFICATE,
				client_certificate, sizeof(client_certificate));
	if (ret < 0) {
		LOG_ERR("Failed to register public cert: %d", ret);
		goto exit;
	}

	/* Load private key */
	ret = tls_credential_add(sec_tag, TLS_CREDENTIAL_PRIVATE_KEY,
				private_key, sizeof(private_key));
	if (ret < 0) {
		LOG_ERR("Failed to register private key: %d", ret);
		goto exit;
	}

exit:
	return ret;
}

/**
 * @brief Free up resources from specified security tag.
 *
 * @param sec_tag - the tag to free.
 * @return 0 on success.
 */
static int tls_unloadcrdl(int sec_tag)
{
	tls_credential_delete(sec_tag, TLS_CREDENTIAL_CA_CERTIFICATE);
	tls_credential_delete(sec_tag, TLS_CREDENTIAL_SERVER_CERTIFICATE);
	tls_credential_delete(sec_tag, TLS_CREDENTIAL_PRIVATE_KEY);

	return 0;
}



static void cloud_handler(const struct nrf_cloud_evt *evt)
{
	switch (evt->type) {
	case NRF_CLOUD_EVT_TRANSPORT_CONNECTED:
		LOG_DBG("NRF_CLOUD_EVT_TRANSPORT_CONNECTED");
		break;
	case NRF_CLOUD_EVT_TRANSPORT_CONNECTING:
		LOG_DBG("NRF_CLOUD_EVT_TRANSPORT_CONNECTING");
		break;
	case NRF_CLOUD_EVT_USER_ASSOCIATION_REQUEST:
		LOG_DBG("NRF_CLOUD_EVT_USER_ASSOCIATION_REQUEST");
		break;
	case NRF_CLOUD_EVT_USER_ASSOCIATED:
		LOG_DBG("NRF_CLOUD_EVT_USER_ASSOCIATED");
		break;
	case NRF_CLOUD_EVT_READY:
		LOG_DBG("NRF_CLOUD_EVT_READY");
		k_sem_give(&cloud_ready);
		break;
	case NRF_CLOUD_EVT_RX_DATA:
		LOG_DBG("NRF_CLOUD_EVT_RX_DATA");
		LOG_DBG("%d bytes received from cloud", evt->data.len);
		break;
	case NRF_CLOUD_EVT_PINGRESP:
		LOG_DBG("NRF_CLOUD_EVT_PINGRESP");
		break;
	case NRF_CLOUD_EVT_SENSOR_DATA_ACK:
		LOG_DBG("NRF_CLOUD_EVT_SENSOR_DATA_ACK");
		break;
	case NRF_CLOUD_EVT_TRANSPORT_DISCONNECTED:
		LOG_DBG("NRF_CLOUD_EVT_TRANSPORT_DISCONNECTED");
		break;
	case NRF_CLOUD_EVT_FOTA_START:
		LOG_DBG("NRF_CLOUD_EVT_FOTA_START");
		break;
	case NRF_CLOUD_EVT_FOTA_DONE:
		LOG_DBG("NRF_CLOUD_EVT_FOTA_DONE");
		break;
	case NRF_CLOUD_EVT_FOTA_ERROR:
		LOG_DBG("NRF_CLOUD_EVT_FOTA_ERROR");
		break;
	case NRF_CLOUD_EVT_ERROR:
		LOG_DBG("NRF_CLOUD_EVT_ERROR: %d", evt->status);
		break;
	default:
		LOG_DBG("Unhandled cloud event type: %d", evt->type);
		break;
	}
}

/**
 * @brief Update device shadow to indicate the services this device supports.
 *
 * @return 0 on success or negative error code if failed.
 */
static int send_service_info(void)
{
	struct nrf_cloud_svc_info_fota fota_info = {
		.application = false,
		.bootloader = false,
		.modem = false
	};
	struct nrf_cloud_svc_info_ui ui_info = {
		.gps = false,
		.humidity = true,
		.rsrp = false,
		.temperature = true,
		.button = false
	};
	struct nrf_cloud_svc_info service_info = {
		.fota = &fota_info,
		.ui = &ui_info
	};
	struct nrf_cloud_device_status device_status = {
		.modem = NULL,
		.svc = &service_info

	};

	return nrf_cloud_shadow_device_status_update(&device_status);
}

/* accumulate random values in the range of +/- scale */
void simulate_sensor_data(float *sensor_value, float scale)
{
	*sensor_value += (((double) sys_rand32_get()) / UINT32_MAX - 0.5) * scale;
}

//TODO: redo this with libc in mind
void render_sensor_data(char *output, size_t len, const char *id, float sensor_value)
{
	/* printf formatter for JSON message to send containing fake temperature data */
	static const char data_fmt[] = "{\"appId\":\"%s\", \"messageType\":\"DATA\","
				       " \"data\":\"%d.%d\"}";

	snprintf(output, len, data_fmt, id, (int)sensor_value,
		 ((int)(sensor_value * 10)) % 10);
}


//TODO: Set up KConfig
#define TEMPORARY_SSID "NordicPDX"
#define TEMPORARY_PASSWORD "BillionBluetooth"
#define TEMPORARY_USE_PASSWORD true
#define TEMPORARY_MAX_SSID_LEN 32

static struct net_mgmt_event_callback wifi_mgmt_cb;

int connect_to_wifi(void)
{
	struct net_if *iface;
	int err = 0;

	LOG_INF("Attempting to connect to SSID %s with PSK of length %d",
		TEMPORARY_SSID, strlen(TEMPORARY_PASSWORD));

	iface = net_if_get_default();
	if (!iface) {
		LOG_ERR("Cannot connect to WiFi, no interface available");
		return -ENODEV;
	}

	/* Sleep for 1 second to make sure the wpa_supplicant thread has had a chance to start */
	k_sleep(K_SECONDS(1));

	/* Set up a callback */
	net_mgmt_init_event_callback(&wifi_mgmt_cb,
				     wifi_event_handler,
				     NET_EVENT_WIFI_SCAN_RESULT |
				     NET_EVENT_WIFI_SCAN_DONE |
				     NET_EVENT_WIFI_CONNECT_RESULT |
				     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_mgmt_cb);

	/* Set up connection request */
	static struct wifi_connect_req_params connect_params = {
		.timeout = SYS_FOREVER_MS,
		.ssid = TEMPORARY_SSID,
		.ssid_length = sizeof(TEMPORARY_SSID) - 1,
		.channel = WIFI_CHANNEL_ANY,
		.psk = TEMPORARY_PASSWORD,
		.psk_length = sizeof(TEMPORARY_PASSWORD) - 1,
		.security = WIFI_SECURITY_TYPE_PSK,
		.mfp = WIFI_MFP_OPTIONAL
	};

	/* Execute connection request */
	err = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &connect_params, sizeof(connect_params));

	if (err) {
		LOG_ERR("WiFi connection request failed, error %d", err);
		return -ENOEXEC;
	}

	LOG_INF("Connection requested");

	const struct wifi_status *status;
	err = net_mgmt_event_wait(NET_EVENT_WIFI_CONNECT_RESULT, NULL, &iface,
				  (const void **)&status, NULL, K_SECONDS(30));
	if (err == -ETIMEDOUT) {
		LOG_ERR("WiFi connection attempt timed out.");
		return -ETIMEDOUT;
	} else if (err) {
		LOG_ERR("Failed to wait for WiFi connection attempt, error %d.", err);
		return -ENOEXEC;
	} else if (status->status) {
		LOG_ERR("WiFi connection attempt failed, error status %d.", status->status);
		return -ENOEXEC;
	}


	/* Sleep for 1 second for reasons I don't understand,
	 * but it prevents hostname lookup from sometimes failing somehow
	 */
	k_sleep(K_SECONDS(30));

	LOG_INF("WiFi connected");

	return 0;
}

void main(void)
{
	int err;

	//TODO: QFWT: Why?
#ifdef CLOCK_FEATURE_HFCLK_DIVIDE_PRESENT
	/* For now hardcode to 128MHz */
	nrfx_clock_divider_set(NRF_CLOCK_DOMAIN_HFCLK, NRF_CLOCK_HFCLK_DIV_1);
#endif

	LOG_INF("nRF Cloud WiFi demo started");

	if (connect_to_wifi()) {
		LOG_INF("Could not start WiFi connection, please check your configuration.");
		return;
	}

	LOG_INF("Loading credentials");
	err = tls_load_credentials(SEC_TAG);
	if (err) {
		LOG_ERR("Unable to load credentials: %d", err);
	}

	//LOG_INF("JKLOL");

	LOG_INF("Initializing nRF Cloud");
	struct nrf_cloud_init_param init_param = {
		.event_handler = cloud_handler,
		.client_id = NULL
	};

	err = nrf_cloud_init(&init_param);
	if (err) {
		LOG_ERR("Error initializing nRF Cloud: %d", err);
		return;
	}

	LOG_INF("Connecting to nRF Cloud...");
	err = nrf_cloud_connect(NULL);
	if (err) {
		LOG_ERR("Error connecting to nRF Cloud: %d", err);
		return;
	}

	LOG_INF("Waiting for Cloud connection to be ready.");
	k_sem_take(&cloud_ready, K_FOREVER);

	LOG_INF("Cloud ready.");

	char tenant_id[NRF_CLOUD_TENANT_ID_MAX_LEN];

	err = nrf_cloud_tenant_id_get(tenant_id, sizeof(tenant_id));
	if (err) {
		LOG_ERR("Error getting tenant id: %d", err);
	} else {
		LOG_INF("Tenant id: %s", tenant_id);
	}

	err = send_service_info();
	if (err) {
		LOG_ERR("Error sending service info: %d", err);
	} else {
		LOG_INF("Service info sent.");
	}

	char data[100];
	struct nrf_cloud_tx_data msg = {
		.topic_type = NRF_CLOUD_TOPIC_MESSAGE,
		.qos = MQTT_QOS_1_AT_LEAST_ONCE,
		.data.ptr = data
	};
	float temp = 10.0;
	float humid = 25.0;

	do
	{
		simulate_sensor_data(&temp, 0.5f);
		render_sensor_data(data, sizeof(data), TEMP_ID, temp);
		msg.data.len = strlen(data);

		LOG_INF("Sending %s to nRF Cloud...", data);

		err = nrf_cloud_send(&msg);
		if (!err) {
			simulate_sensor_data(&humid, 0.1f);
			render_sensor_data(data, sizeof(data), HUMID_ID, humid);
			msg.data.len = strlen(data);

			LOG_INF("Sending %s to nRF Cloud...", data);

			err = nrf_cloud_send(&msg);
		}
		if (err) {
			LOG_ERR("Error sending message to cloud: %d", err);
			k_sleep(K_SECONDS(1));

			k_sem_reset(&cloud_ready);
			LOG_INF("Reconnecting to nRF Cloud...");
			err = nrf_cloud_connect(NULL);
			if (err) {
				LOG_ERR("Connection failed: %d", err);
				break;
			}
			k_sem_take(&cloud_ready, K_FOREVER);
			LOG_INF("Connected.");
		} else {
			LOG_INF("message sent!");
		}

		k_sleep(K_SECONDS(5));
	} while (1);

	err = nrf_cloud_disconnect();
	if (err) {
		LOG_ERR("Error disconnecting from nRF Cloud: %d", err);
	} else {
		LOG_INF("Disconnected.");
	}
}
