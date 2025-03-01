/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/shell/shell.h>
#include <getopt.h>
#include <net/nrf_cloud.h>
#if defined(CONFIG_NRF_CLOUD_REST)
#include <net/nrf_cloud_rest.h>
#endif
#include <modem/location.h>
#include <dk_buttons_and_leds.h>
#if defined(CONFIG_MOSH_CLOUD_LWM2M)
#include <net/lwm2m_client_utils_location.h>
#include "cloud_lwm2m.h"
#endif

#include "mosh_print.h"
#include "location_cmd_utils.h"

#include "mosh_defines.h"

#if defined(CONFIG_MOSH_CLOUD_LWM2M)
/* Location Assistance resource IDs */
#define GROUND_FIX_SEND_LOCATION_BACK	0
#define GROUND_FIX_RESULT_CODE		1
#define GROUND_FIX_LATITUDE		2
#define GROUND_FIX_LONGITUDE		3
#define GROUND_FIX_ACCURACY		4
#endif

enum location_shell_command {
	LOCATION_CMD_NONE = 0,
	LOCATION_CMD_GET,
	LOCATION_CMD_CANCEL
};
extern struct k_work_q mosh_common_work_q;

/* Work for sending acquired location to nRF Cloud */
struct gnss_location_work_data {
	struct k_work work;

	enum nrf_cloud_gnss_type format;

	/* Data from location event */
	struct location_event_data loc_evt_data;
};
static struct gnss_location_work_data gnss_location_work_data;


#if defined(CONFIG_DK_LIBRARY)
static struct k_work_delayable location_evt_led_work;
#endif

/******************************************************************************/
static const char location_usage_str[] =
	"Usage: location <subcommand> [options]\n"
	"\n"
	"Subcommands:\n"
	"  get:      Requests the current position or starts periodic position updates.\n"
	"  cancel:   Cancel/stop on going request. No options\n";

static const char location_get_usage_str[] =
	"Usage: location get [--mode <mode>] [--method <method>]\n"
	"[--timeout <msecs>] [--interval <secs>]\n"
	"[--gnss_accuracy <acc>] [--gnss_num_fixes <number of fixes>]\n"
	"[--gnss_timeout <timeout in msecs>] [--gnss_visibility]\n"
	"[--gnss_priority] [--gnss_cloud_nmea] [--gnss_cloud_pvt]\n"
	"[--cellular_timeout <timeout in msecs>] [--cellular_service <service_string>]\n"
	"[--wifi_timeout <timeout in msecs>] [--wifi_service <service_string>]\n"
	"Options:\n"
	"  -m, --method,       Location method: 'gnss', 'cellular' or 'wifi'. Multiple\n"
	"                      '--method' parameters may be given to indicate list of\n"
	"                      methods in priority order.\n"
	"  --mode,             Location request mode: 'fallback' (default) or 'all'.\n"
	"  --interval,         Position update interval in seconds\n"
	"                      (default: 0 = single position)\n"
	"  -t, --timeout,      Timeout for the entire location request in milliseconds.\n"
	"                      Zero means timeout is disabled.\n"
	"  --gnss_accuracy,    Used GNSS accuracy: 'low', 'normal' or 'high'\n"
	"  --gnss_num_fixes,   Number of consecutive fix attempts (if gnss_accuracy\n"
	"                      set to 'high', default: 3)\n"
	"  --gnss_timeout,     GNSS timeout in milliseconds. Zero means timeout is disabled.\n"
	"  --gnss_visibility,  Enables GNSS obstructed visibility detection\n"
	"  --gnss_priority,    Enables GNSS priority mode\n"
	"  --gnss_cloud_nmea,  Send acquired GNSS location to nRF Cloud formatted as NMEA\n"
	"  --gnss_cloud_pvt,   Send acquired GNSS location to nRF Cloud formatted as PVT\n"
	"  --cellular_timeout, Cellular timeout in milliseconds. Zero means timeout is disabled.\n"
	"  --cellular_service, Used cellular positioning service:\n"
	"                      'any' (default), 'nrf' or 'here'\n"
	"  --wifi_timeout,     Wi-Fi timeout in milliseconds. Zero means timeout is disabled.\n"
	"  --wifi_service,     Used Wi-Fi positioning service:\n"
	"                      'any' (default), 'nrf' or 'here'\n";

/******************************************************************************/

/* Following are not having short options */
enum {
	LOCATION_SHELL_OPT_INTERVAL         = 1001,
	LOCATION_SHELL_OPT_MODE,
	LOCATION_SHELL_OPT_GNSS_ACCURACY,
	LOCATION_SHELL_OPT_GNSS_TIMEOUT,
	LOCATION_SHELL_OPT_GNSS_NUM_FIXES,
	LOCATION_SHELL_OPT_GNSS_VISIBILITY,
	LOCATION_SHELL_OPT_GNSS_PRIORITY_MODE,
	LOCATION_SHELL_OPT_GNSS_LOC_CLOUD_NMEA,
	LOCATION_SHELL_OPT_GNSS_LOC_CLOUD_PVT,
	LOCATION_SHELL_OPT_CELLULAR_TIMEOUT,
	LOCATION_SHELL_OPT_CELLULAR_SERVICE,
	LOCATION_SHELL_OPT_WIFI_TIMEOUT,
	LOCATION_SHELL_OPT_WIFI_SERVICE,
};

/* Specifying the expected options */
static struct option long_options[] = {
	{ "method", required_argument, 0, 'm' },
	{ "mode", required_argument, 0, LOCATION_SHELL_OPT_MODE },
	{ "interval", required_argument, 0, LOCATION_SHELL_OPT_INTERVAL },
	{ "timeout", required_argument, 0, 't' },
	{ "gnss_accuracy", required_argument, 0, LOCATION_SHELL_OPT_GNSS_ACCURACY },
	{ "gnss_timeout", required_argument, 0, LOCATION_SHELL_OPT_GNSS_TIMEOUT },
	{ "gnss_num_fixes", required_argument, 0, LOCATION_SHELL_OPT_GNSS_NUM_FIXES },
	{ "gnss_visibility", no_argument, 0, LOCATION_SHELL_OPT_GNSS_VISIBILITY },
	{ "gnss_priority", no_argument, 0, LOCATION_SHELL_OPT_GNSS_PRIORITY_MODE },
	{ "gnss_cloud_nmea", no_argument, 0, LOCATION_SHELL_OPT_GNSS_LOC_CLOUD_NMEA },
	{ "gnss_cloud_pvt", no_argument, 0, LOCATION_SHELL_OPT_GNSS_LOC_CLOUD_PVT },
	{ "cellular_timeout", required_argument, 0, LOCATION_SHELL_OPT_CELLULAR_TIMEOUT },
	{ "cellular_service", required_argument, 0, LOCATION_SHELL_OPT_CELLULAR_SERVICE },
	{ "wifi_timeout", required_argument, 0, LOCATION_SHELL_OPT_WIFI_TIMEOUT },
	{ "wifi_service", required_argument, 0, LOCATION_SHELL_OPT_WIFI_SERVICE },
	{ 0, 0, 0, 0 }
};

static bool gnss_location_to_cloud;
static enum nrf_cloud_gnss_type gnss_location_to_cloud_format;

/******************************************************************************/

static void location_shell_print_usage(const struct shell *shell,
				       enum location_shell_command command)
{
	switch (command) {
	case LOCATION_CMD_GET:
		mosh_print_no_format(location_get_usage_str);
		break;

	default:
		mosh_print_no_format(location_usage_str);
		break;
	}
}

#define MOSH_LOC_SERVICE_NONE 0xFF

static enum location_service location_shell_string_to_service(const char *service_str)
{
	enum location_service service = MOSH_LOC_SERVICE_NONE;

	if (strcmp(service_str, "any") == 0) {
		service = LOCATION_SERVICE_ANY;
	} else if (strcmp(service_str, "nrf") == 0) {
		service = LOCATION_SERVICE_NRF_CLOUD;
	} else if (strcmp(service_str, "here") == 0) {
		service = LOCATION_SERVICE_HERE;
	}

	return service;
}

/******************************************************************************/

static void location_to_cloud_worker(struct k_work *work_item)
{
	struct gnss_location_work_data *data =
		CONTAINER_OF(work_item, struct gnss_location_work_data, work);
	int ret;
	char *body = NULL;

	/* If the position update was acquired by using GNSS, send it to nRF Cloud. */
	if (data->loc_evt_data.method == LOCATION_METHOD_GNSS) {
		/* Encode json payload in requested format, either NMEA or in PVT*/
		ret = location_cmd_utils_gnss_loc_to_cloud_payload_json_encode(
			data->format, &data->loc_evt_data.location, &body);
		if (ret) {
			mosh_error("Failed to generate nrf cloud NMEA or PVT, err: %d", ret);
			goto clean_up;
		}

		/* Send the encoded message to the nRF Cloud using either MQTT or REST transport */
#if defined(CONFIG_NRF_CLOUD_MQTT)
		struct nrf_cloud_tx_data mqtt_msg = {
			.data.ptr = body,
			.data.len = strlen(body),
			.qos = MQTT_QOS_1_AT_LEAST_ONCE,
			.topic_type = NRF_CLOUD_TOPIC_MESSAGE,
		};
		mosh_print("Sending acquired GNSS location to nRF Cloud via MQTT, body: %s", body);

		ret = nrf_cloud_send(&mqtt_msg);
		if (ret) {
			mosh_error("MQTT: location sending failed");
		}
#elif defined(CONFIG_NRF_CLOUD_REST)
#define REST_RX_BUF_SZ 300 /* No payload in a response, "just" headers */
		static char rx_buf[REST_RX_BUF_SZ];
		static char device_id[NRF_CLOUD_CLIENT_ID_MAX_LEN + 1];
		static struct nrf_cloud_rest_context rest_ctx = { .connect_socket = -1,
								  .keep_alive = false,
								  .rx_buf = rx_buf,
								  .rx_buf_len = sizeof(rx_buf),
								  .fragment_size = 0 };

		mosh_print("Sending acquired GNSS location to nRF Cloud via REST, body: %s", body);

		ret = nrf_cloud_client_id_get(device_id, sizeof(device_id));
		if (ret) {
			mosh_error("Failed to get device ID, error: %d", ret);
			goto clean_up;
		}

		ret = nrf_cloud_rest_send_device_message(&rest_ctx, device_id, body, false, NULL);
		if (ret) {
			mosh_error("REST: location sending failed: %d", ret);
		}
#endif
	}

clean_up:
	if (body) {
		cJSON_free(body);
	}
}

#if defined(CONFIG_DK_LIBRARY)
static void location_evt_led_worker(struct k_work *work_item)
{
	dk_set_led_off(LOCATION_STATUS_LED);
}
#endif

/******************************************************************************/

#if defined(CONFIG_LOCATION_METHOD_GNSS_AGPS_EXTERNAL)
static void location_ctrl_agps_ext_handle(const struct nrf_modem_gnss_agps_data_frame *agps_req)
{
#if defined(CONFIG_MOSH_CLOUD_LWM2M)
	int err;

	if (!cloud_lwm2m_is_connected()) {
		mosh_error("LwM2M not connected, can't request A-GPS data");
		return;
	}

	location_assistance_agps_set_mask(agps_req);

	while ((err = location_assistance_agps_request_send(cloud_lwm2m_client_ctx_get(), true)) ==
	       -EAGAIN) {
		/* LwM2M client utils library is currently handling a P-GPS data request, need to
		 * wait until it has been completed.
		 */
		k_sleep(K_SECONDS(1));
	}
	if (err) {
		mosh_error("Failed to request A-GPS data, err: %d", err);
	}
#endif /* CONFIG_MOSH_CLOUD_LWM2M */
}
#endif /* CONFIG_LOCATION_METHOD_GNSS_AGPS_EXTERNAL */

#if defined(CONFIG_LOCATION_METHOD_GNSS_PGPS_EXTERNAL)
static void location_ctrl_pgps_ext_handle(const struct gps_pgps_request *pgps_req)
{
#if defined(CONFIG_MOSH_CLOUD_LWM2M)
	int err = -1;

	if (!cloud_lwm2m_is_connected()) {
		mosh_error("LwM2M not connected, can't request P-GPS data");
		return;
	}

	err = location_assist_pgps_set_prediction_count(pgps_req->prediction_count);
	err |= location_assist_pgps_set_prediction_interval(pgps_req->prediction_period_min);
	location_assist_pgps_set_start_gps_day(pgps_req->gps_day);
	err |= location_assist_pgps_set_start_time(pgps_req->gps_time_of_day);
	if (err) {
		mosh_error("Failed to set P-GPS request parameters");
		return;
	}

	while ((err = location_assistance_pgps_request_send(cloud_lwm2m_client_ctx_get(), true)) ==
	       -EAGAIN) {
		/* LwM2M client utils library is currently handling an A-GPS data request, need to
		 * wait until it has been completed.
		 */
		k_sleep(K_SECONDS(1));
	}
	if (err) {
		mosh_error("Failed to request P-GPS data, err: %d", err);
	}
#endif /* CONFIG_MOSH_CLOUD_LWM2M */
}
#endif /* CONFIG_LOCATION_METHOD_GNSS_PGPS_EXTERNAL */

#if defined(CONFIG_LOCATION_METHOD_CELLULAR_EXTERNAL) && defined(CONFIG_MOSH_CLOUD_LWM2M)
static int location_ctrl_lwm2m_cell_result_cb(uint16_t obj_inst_id,
					      uint16_t res_id, uint16_t res_inst_id,
					      uint8_t *data, uint16_t data_len,
					      bool last_block, size_t total_size)
{
	int err;
	int32_t result;
	struct location_data location = {0};
	double temp_accuracy;

	if (data_len != sizeof(int32_t)) {
		mosh_error("Unexpected data length in callback");
		err = -1;
		goto exit;
	}

	result = (int32_t)*data;
	switch (result) {
	case LOCATION_ASSIST_RESULT_CODE_OK:
		break;

	case LOCATION_ASSIST_RESULT_CODE_PERMANENT_ERR:
	case LOCATION_ASSIST_RESULT_CODE_TEMP_ERR:
	default:
		mosh_error("LwM2M server failed to get location");
		err = -1;
		goto exit;
	}

	err = lwm2m_engine_get_float(LWM2M_PATH(GROUND_FIX_OBJECT_ID, 0, GROUND_FIX_LATITUDE),
				     &location.latitude);
	if (err) {
		mosh_error("Getting latitude from LwM2M engine failed, err: %d", err);
		goto exit;
	}

	err = lwm2m_engine_get_float(LWM2M_PATH(GROUND_FIX_OBJECT_ID, 0, GROUND_FIX_LONGITUDE),
				     &location.longitude);
	if (err) {
		mosh_error("Getting longitude from LwM2M engine failed, err: %d", err);
		goto exit;
	}

	err = lwm2m_engine_get_float(LWM2M_PATH(GROUND_FIX_OBJECT_ID, 0, GROUND_FIX_ACCURACY),
				     &temp_accuracy);
	if (err) {
		mosh_error("Getting accuracy from LwM2M engine failed, err: %d", err);
		goto exit;
	}
	location.accuracy = temp_accuracy;

exit:
	if (err) {
		location_cellular_ext_result_set(LOCATION_EXT_RESULT_ERROR, NULL);
	} else {
		location_cellular_ext_result_set(LOCATION_EXT_RESULT_SUCCESS, &location);
	}

	return err;
}
#endif /* CONFIG_LOCATION_METHOD_CELLULAR_EXTERNAL && CONFIG_MOSH_CLOUD_LWM2M */

#if defined(CONFIG_LOCATION_METHOD_CELLULAR_EXTERNAL)
static void location_ctrl_cellular_ext_handle(const struct lte_lc_cells_info *cell_info)
{
#if defined(CONFIG_MOSH_CLOUD_LWM2M)
	int err = -1;

	if (!cloud_lwm2m_is_connected()) {
		mosh_error("LwM2M not connected, can't send cellular location request");
		goto exit;
	}

	err = lwm2m_engine_register_post_write_callback(
		LWM2M_PATH(GROUND_FIX_OBJECT_ID, 0, GROUND_FIX_RESULT_CODE),
		location_ctrl_lwm2m_cell_result_cb);
	if (err) {
		mosh_error("Failed to register post write callback, err: %d", err);
	}

	ground_fix_set_report_back(true);

	err = lwm2m_update_signal_meas_objects(cell_info);
	if (err) {
		mosh_error("Failed to update LwM2M signal meas object, err: %d", err);
		goto exit;
	}

	err = location_assistance_ground_fix_request_send(cloud_lwm2m_client_ctx_get(), true);
	if (err) {
		mosh_error("Failed to send cellular location request, err: %d", err);
		goto exit;
	}

exit:
	if (err) {
		location_cellular_ext_result_set(LOCATION_EXT_RESULT_ERROR, NULL);
	}
#else /* !CONFIG_MOSH_CLOUD_LWM2M */
	location_cellular_ext_result_set(LOCATION_EXT_RESULT_ERROR, NULL);
#endif
}
#endif /* CONFIG_LOCATION_METHOD_CELLULAR_EXTERNAL */

void location_ctrl_event_handler(const struct location_event_data *event_data)
{
	switch (event_data->id) {
	case LOCATION_EVT_LOCATION:
		mosh_print("Location:");
		mosh_print(
			"  used method: %s (%d)",
			location_method_str(event_data->method),
			event_data->method);
		mosh_print("  latitude: %.06f", event_data->location.latitude);
		mosh_print("  longitude: %.06f", event_data->location.longitude);
		mosh_print("  accuracy: %.01f m", event_data->location.accuracy);
		if (event_data->location.datetime.valid) {
			mosh_print(
				"  date: %04d-%02d-%02d",
				event_data->location.datetime.year,
				event_data->location.datetime.month,
				event_data->location.datetime.day);
			mosh_print(
				"  time: %02d:%02d:%02d.%03d UTC",
				event_data->location.datetime.hour,
				event_data->location.datetime.minute,
				event_data->location.datetime.second,
				event_data->location.datetime.ms);
		}
		mosh_print(
			"  Google maps URL: https://maps.google.com/?q=%f,%f",
			event_data->location.latitude, event_data->location.longitude);

		if (gnss_location_to_cloud) {
			k_work_init(&gnss_location_work_data.work, location_to_cloud_worker);

			gnss_location_work_data.loc_evt_data = *event_data;
			gnss_location_work_data.format = gnss_location_to_cloud_format;
			k_work_submit_to_queue(&mosh_common_work_q, &gnss_location_work_data.work);
		}

#if defined(CONFIG_DK_LIBRARY)
		dk_set_led_on(LOCATION_STATUS_LED);
		k_work_reschedule_for_queue(&mosh_common_work_q, &location_evt_led_work,
			K_SECONDS(5));
#endif
		break;

	case LOCATION_EVT_TIMEOUT:
		mosh_error("Location request timed out");
		break;

	case LOCATION_EVT_ERROR:
		mosh_error("Location request failed");
		break;
	case LOCATION_EVT_GNSS_ASSISTANCE_REQUEST:
#if defined(CONFIG_LOCATION_METHOD_GNSS_AGPS_EXTERNAL)
		mosh_print(
			"A-GPS request from Location library "
			"(ephe: 0x%08x alm: 0x%08x flags: 0x%02x)",
			event_data->agps_request.sv_mask_ephe,
			event_data->agps_request.sv_mask_alm,
			event_data->agps_request.data_flags);
		location_ctrl_agps_ext_handle(&event_data->agps_request);
#endif
		break;
	case LOCATION_EVT_GNSS_PREDICTION_REQUEST:
#if defined(CONFIG_LOCATION_METHOD_GNSS_PGPS_EXTERNAL)
		mosh_print(
			"P-GPS request from Location library "
			"(prediction count: %d validity time: %d gps day: %d time of day: %d)",
			event_data->pgps_request.prediction_count,
			event_data->pgps_request.prediction_period_min,
			event_data->pgps_request.gps_day,
			event_data->pgps_request.gps_time_of_day);
		location_ctrl_pgps_ext_handle(&event_data->pgps_request);
#endif
		break;
	case LOCATION_EVT_CELLULAR_EXT_REQUEST:
#if defined(CONFIG_LOCATION_METHOD_CELLULAR_EXTERNAL)
		mosh_print("Cellular location request from Location library "
			   "(neighbor cells: %d GCI cells: %d)",
			   event_data->cellular_request.ncells_count,
			   event_data->cellular_request.gci_cells_count);
		location_ctrl_cellular_ext_handle(&event_data->cellular_request);
#endif
		break;
	case LOCATION_EVT_RESULT_UNKNOWN:
		mosh_print("Location request completed, but the result is not known");
		break;
	default:
		mosh_warn("Unknown event from location library, id %d", event_data->id);
		break;
	}
}

void location_ctrl_init(void)
{
	int ret;

#if defined(CONFIG_DK_LIBRARY)
	k_work_init_delayable(&location_evt_led_work, location_evt_led_worker);
#endif

	ret = location_init(location_ctrl_event_handler);
	if (ret) {
		mosh_error("Initializing the Location library failed, err: %d\n", ret);
	}
}

/******************************************************************************/

int location_shell(const struct shell *shell, size_t argc, char **argv)
{
	enum location_shell_command command = LOCATION_CMD_NONE;

	int interval = 0;
	bool interval_set = false;

	int timeout = 0;
	bool timeout_set = false;

	int gnss_timeout = 0;
	bool gnss_timeout_set = false;

	enum location_accuracy gnss_accuracy = 0;
	bool gnss_accuracy_set = false;

	int gnss_num_fixes = 0;
	bool gnss_num_fixes_set = false;

	bool gnss_visibility = false;

	bool gnss_priority_mode = false;

	int cellular_timeout = 0;
	bool cellular_timeout_set = false;
	enum location_service cellular_service = LOCATION_SERVICE_ANY;

	int wifi_timeout = 0;
	bool wifi_timeout_set = false;
	enum location_service wifi_service = LOCATION_SERVICE_ANY;

	enum location_method method_list[CONFIG_LOCATION_METHODS_LIST_SIZE] = { 0 };
	int method_count = 0;

	enum location_req_mode req_mode = LOCATION_REQ_MODE_FALLBACK;

	int opt;
	int ret = 0;
	int long_index = 0;

	gnss_location_to_cloud_format = NRF_CLOUD_GNSS_TYPE_PVT;

	if (argc < 2) {
		goto show_usage;
	}

	/* command = argv[0] = "location" */
	/* sub-command = argv[1] */
	if (strcmp(argv[1], "get") == 0) {
		command = LOCATION_CMD_GET;
	} else if (strcmp(argv[1], "cancel") == 0) {
		command = LOCATION_CMD_CANCEL;
	} else {
		mosh_error("Unsupported command=%s\n", argv[1]);
		ret = -EINVAL;
		goto show_usage;
	}

	/* Reset getopt due to possible previous failures */
	optreset = 1;

	/* We start from subcmd arguments */
	optind = 2;

	gnss_location_to_cloud = false;

	while ((opt = getopt_long(argc, argv, "m:t:", long_options, &long_index)) != -1) {
		switch (opt) {
		case LOCATION_SHELL_OPT_GNSS_TIMEOUT:
			gnss_timeout = atoi(optarg);
			gnss_timeout_set = true;
			if (gnss_timeout == 0) {
				gnss_timeout = SYS_FOREVER_MS;
			}
			break;

		case LOCATION_SHELL_OPT_GNSS_NUM_FIXES:
			gnss_num_fixes = atoi(optarg);
			gnss_num_fixes_set = true;
			break;
		case LOCATION_SHELL_OPT_GNSS_LOC_CLOUD_NMEA:
			gnss_location_to_cloud_format = NRF_CLOUD_GNSS_TYPE_NMEA;
		/* flow-through */
		case LOCATION_SHELL_OPT_GNSS_LOC_CLOUD_PVT:
			gnss_location_to_cloud = true;
			break;
		case LOCATION_SHELL_OPT_CELLULAR_TIMEOUT:
			cellular_timeout = atoi(optarg);
			cellular_timeout_set = true;
			if (cellular_timeout == 0) {
				cellular_timeout = SYS_FOREVER_MS;
			}
			break;

		case LOCATION_SHELL_OPT_CELLULAR_SERVICE:
			cellular_service = location_shell_string_to_service(optarg);
			if (cellular_service == MOSH_LOC_SERVICE_NONE) {
				mosh_error("Unknown cellular positioning service. See usage:");
				goto show_usage;
			}
			break;

		case LOCATION_SHELL_OPT_WIFI_TIMEOUT:
			wifi_timeout = atoi(optarg);
			wifi_timeout_set = true;
			if (wifi_timeout == 0) {
				wifi_timeout = SYS_FOREVER_MS;
			}
			break;

		case LOCATION_SHELL_OPT_WIFI_SERVICE:
			wifi_service = location_shell_string_to_service(optarg);
			if (wifi_service == MOSH_LOC_SERVICE_NONE) {
				mosh_error("Unknown Wi-Fi positioning service. See usage:");
				goto show_usage;
			}
			break;

		case LOCATION_SHELL_OPT_INTERVAL:
			interval = atoi(optarg);
			interval_set = true;
			break;
		case 't':
			timeout = atoi(optarg);
			timeout_set = true;
			if (timeout == 0) {
				timeout = SYS_FOREVER_MS;
			}
			break;

		case LOCATION_SHELL_OPT_GNSS_ACCURACY:
			if (strcmp(optarg, "low") == 0) {
				gnss_accuracy = LOCATION_ACCURACY_LOW;
			} else if (strcmp(optarg, "normal") == 0) {
				gnss_accuracy = LOCATION_ACCURACY_NORMAL;
			} else if (strcmp(optarg, "high") == 0) {
				gnss_accuracy = LOCATION_ACCURACY_HIGH;
			} else {
				mosh_error("Unknown GNSS accuracy. See usage:");
				goto show_usage;
			}
			gnss_accuracy_set = true;
			break;

		case LOCATION_SHELL_OPT_GNSS_VISIBILITY:
			gnss_visibility = true;
			break;

		case LOCATION_SHELL_OPT_GNSS_PRIORITY_MODE:
			gnss_priority_mode = true;
			break;

		case LOCATION_SHELL_OPT_MODE:
			if (strcmp(optarg, "fallback") == 0) {
				req_mode = LOCATION_REQ_MODE_FALLBACK;
			} else if (strcmp(optarg, "all") == 0) {
				req_mode = LOCATION_REQ_MODE_ALL;
			} else {
				mosh_error(
					"Unknown location request mode (%s) was given. See usage:",
						optarg);
				goto show_usage;
			}
			break;

		case 'm':
			if (method_count >= CONFIG_LOCATION_METHODS_LIST_SIZE) {
				mosh_error(
					"Maximum number of location methods (%d) exceeded. "
					"Location method (%s) still given.",
					CONFIG_LOCATION_METHODS_LIST_SIZE, optarg);
				return -EINVAL;
			}

			if (strcmp(optarg, "cellular") == 0) {
				method_list[method_count] = LOCATION_METHOD_CELLULAR;
			} else if (strcmp(optarg, "gnss") == 0) {
				method_list[method_count] = LOCATION_METHOD_GNSS;
			} else if (strcmp(optarg, "wifi") == 0) {
				method_list[method_count] = LOCATION_METHOD_WIFI;
			} else {
				mosh_error("Unknown method (%s) given. See usage:", optarg);
				goto show_usage;
			}
			method_count++;
			break;

		case '?':
			goto show_usage;

		default:
			mosh_error("Unknown option. See usage:");
			goto show_usage;
		}
	}

	/* Handle location subcommands */
	switch (command) {
	case LOCATION_CMD_CANCEL:
		gnss_location_to_cloud = false;
		k_work_cancel(&gnss_location_work_data.work);
		ret = location_request_cancel();
		if (ret) {
			mosh_error("Canceling location request failed, err: %d", ret);
			return -1;
		}
		mosh_print("Location request cancelled");
		break;

	case LOCATION_CMD_GET: {
		struct location_config config = { 0 };
		struct location_config *real_config = &config;

		if (method_count == 0 && !interval_set && !timeout_set) {
			/* No methods or top level config given. Use default config. */
			real_config = NULL;
		}

		location_config_defaults_set(&config, method_count, method_list);

		for (uint8_t i = 0; i < method_count; i++) {
			if (config.methods[i].method == LOCATION_METHOD_GNSS) {
				if (gnss_timeout_set) {
					config.methods[i].gnss.timeout = gnss_timeout;
				}
				if (gnss_accuracy_set) {
					config.methods[i].gnss.accuracy = gnss_accuracy;
				}
				if (gnss_num_fixes_set) {
					config.methods[i].gnss.num_consecutive_fixes =
						gnss_num_fixes;
				}
				config.methods[i].gnss.visibility_detection = gnss_visibility;
				config.methods[i].gnss.priority_mode = gnss_priority_mode;
			} else if (config.methods[i].method == LOCATION_METHOD_CELLULAR) {
				config.methods[i].cellular.service = cellular_service;
				if (cellular_timeout_set) {
					config.methods[i].cellular.timeout = cellular_timeout;
				}
			} else if (config.methods[i].method == LOCATION_METHOD_WIFI) {
				config.methods[i].wifi.service = wifi_service;
				if (wifi_timeout_set) {
					config.methods[i].wifi.timeout = wifi_timeout;
				}
			}
		}

		if (interval_set) {
			config.interval = interval;
		}
		if (timeout_set) {
			config.timeout = timeout;
		}
		config.mode = req_mode;

		ret = location_request(real_config);
		if (ret) {
			mosh_error("Requesting location failed, err: %d", ret);
			return -1;
		}
		mosh_print("Started to get current location...");
		break;
	}
	default:
		mosh_error("Unknown command. See usage:");
		goto show_usage;
	}
	return ret;
show_usage:
	/* Reset getopt for another users */
	optreset = 1;

	location_shell_print_usage(shell, command);
	return ret;
}
