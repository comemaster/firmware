#include <zephyr.h>
#include <stdio.h>
#include <string.h>
#include <logging/log_ctrl.h>
#include <power/reboot.h>
#include <device.h>
#include <drivers/sensor.h>
#include <drivers/gps.h>
#include <net/cloud.h>
#include <modem/lte_lc.h>
#include <stdlib.h>
#include <modem/modem_info.h>
#include <net/socket.h>
#include <dfu/mcuboot.h>
#include <date_time.h>
#include <dk_buttons_and_leds.h>
#include <math.h>

/* Application spesific module*/
#include "gps_controller.h"
#include "ext_sensors.h"
#include "watchdog.h"
#include "cloud_codec.h"
#include "ui.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(cat_tracker, CONFIG_CAT_TRACKER_LOG_LEVEL);

#define AWS_CLOUD_CLIENT_ID_LEN 15
#define AWS "$aws/things/"
#define AWS_LEN (sizeof(AWS) - 1)
#define CFG_TOPIC AWS "%s/shadow/get/accepted/desired/cfg"
#define CFG_TOPIC_LEN (AWS_LEN + AWS_CLOUD_CLIENT_ID_LEN + 32)
#define BATCH_TOPIC "%s/batch"
#define BATCH_TOPIC_LEN (AWS_CLOUD_CLIENT_ID_LEN + 6)
#define MESSAGES_TOPIC "%s/messages"
#define MESSAGES_TOPIC_LEN (AWS_CLOUD_CLIENT_ID_LEN + 9)

enum app_endpoint_type { CLOUD_EP_TOPIC_MESSAGES = CLOUD_EP_PRIV_START };

static struct cloud_data_gps gps_buf[CONFIG_GPS_BUFFER_MAX];
static struct cloud_data_sensors sensors_buf[CONFIG_SENSOR_BUFFER_MAX];
static struct cloud_data_modem modem_buf[CONFIG_MODEM_BUFFER_MAX];
static struct cloud_data_ui ui_buf[CONFIG_UI_BUFFER_MAX];
static struct cloud_data_accelerometer accel_buf[CONFIG_ACCEL_BUFFER_MAX];
static struct cloud_data_battery bat_buf[CONFIG_BAT_BUFFER_MAX];

static struct cloud_data_cfg cfg = { .gpst = 60,
				     .act = true,
				     .actw = 60,
				     .pasw = 60,
				     .movt = 3600,
				     .acct = 100 };

/** Head of circular buffers. */
static int head_gps_buf;
static int head_sensor_buf;
static int head_modem_buf;
static int head_ui_buf;
static int head_accel_buf;
static int head_bat_buf;

static struct cloud_endpoint sub_ep_topics_sub[1];
static struct cloud_endpoint pub_ep_topics_sub[2];

static char client_id_buf[AWS_CLOUD_CLIENT_ID_LEN + 1];
static char batch_topic[BATCH_TOPIC_LEN + 1];
static char cfg_topic[CFG_TOPIC_LEN + 1];
static char messages_topic[MESSAGES_TOPIC_LEN + 1];

static struct modem_param_info modem_param;
static struct cloud_backend *cloud_backend;

static bool gps_fix;

static bool cloud_connected;
static bool initial_cloud_connection;

static struct k_delayed_work device_config_get_work;
static struct k_delayed_work device_config_send_work;
static struct k_delayed_work data_send_work;
static struct k_delayed_work buffered_data_send_work;
static struct k_delayed_work ui_send_work;
static struct k_delayed_work leds_set_work;
static struct k_delayed_work mov_timeout_work;
static struct k_delayed_work sample_data_work;

K_SEM_DEFINE(accel_trig_sem, 0, 1);
K_SEM_DEFINE(gps_timeout_sem, 0, 1);
K_SEM_DEFINE(cloud_conn_sem, 0, 1);

void error_handler(int err_code)
{
	LOG_ERR("err_handler, error code: %d", err_code);
	ui_led_set_pattern(UI_LED_ERROR_SYSTEM_FAULT);

#if !defined(CONFIG_DEBUG) && defined(CONFIG_REBOOT)
	LOG_PANIC();
	sys_reboot(0);
#else
	while (true) {
		k_cpu_idle();
	}
#endif
}

void bsd_recoverable_error_handler(uint32_t err)
{
	error_handler((int)err);
}

void k_sys_fatal_error_handler(unsigned int reason, const z_arch_esf_t *esf)
{
	ARG_UNUSED(esf);

	LOG_PANIC();
	LOG_ERR("k_sys_fatal_error_handler, error: %d", reason);
	error_handler(reason);
	CODE_UNREACHABLE;
}

static int device_mode_check(void)
{
	if (!cfg.act) {
		return cfg.pasw;
	}

	return cfg.actw;
}

static void time_set(struct gps_pvt *gps_data)
{
	struct tm gps_time;

	/* Change datetime.year and datetime.month to accomodate the
	 * correct input format. */
	gps_time.tm_year = gps_data->datetime.year - 1900;
	gps_time.tm_mon = gps_data->datetime.month - 1;
	gps_time.tm_mday = gps_data->datetime.day;
	gps_time.tm_hour = gps_data->datetime.hour;
	gps_time.tm_min = gps_data->datetime.minute;
	gps_time.tm_sec = gps_data->datetime.seconds;

	date_time_set(&gps_time);
}

static void leds_set(void)
{
	if (!gps_control_is_active()) {
		if (!cfg.act) {
			ui_led_set_pattern(UI_LED_PASSIVE_MODE);
		} else {
			ui_led_set_pattern(UI_LED_ACTIVE_MODE);
		}
	} else {
		ui_led_set_pattern(UI_LED_GPS_SEARCHING);
	}
}

static void battery_buffer_populate(void)
{
	/* Go to start of buffer if end is reached. */
	head_bat_buf += 1;
	if (head_bat_buf == CONFIG_BAT_BUFFER_MAX) {
		head_bat_buf = 0;
	}

	bat_buf[head_bat_buf].bat = modem_param.device.battery.value;
	bat_buf[head_bat_buf].bat_ts = k_uptime_get();
	bat_buf[head_bat_buf].queued = true;

	LOG_INF("Entry: %d of %d in battery buffer filled", head_bat_buf,
		CONFIG_BAT_BUFFER_MAX - 1);
}

static void gps_buffer_populate(struct gps_pvt *gps_data)
{
	/* Go to start of buffer if end is reached. */
	head_gps_buf += 1;
	if (head_gps_buf == CONFIG_GPS_BUFFER_MAX) {
		head_gps_buf = 0;
	}

	gps_buf[head_gps_buf].longi = gps_data->longitude;
	gps_buf[head_gps_buf].lat = gps_data->latitude;
	gps_buf[head_gps_buf].alt = gps_data->altitude;
	gps_buf[head_gps_buf].acc = gps_data->accuracy;
	gps_buf[head_gps_buf].spd = gps_data->speed;
	gps_buf[head_gps_buf].hdg = gps_data->heading;
	gps_buf[head_gps_buf].gps_ts = k_uptime_get();
	gps_buf[head_gps_buf].queued = true;

	LOG_INF("Entry: %d of %d in GPS buffer filled", head_gps_buf,
		CONFIG_GPS_BUFFER_MAX - 1);
}

void acc_array_swap(struct cloud_data_accelerometer *xp,
		    struct cloud_data_accelerometer *yp)
{
    struct cloud_data_accelerometer temp = *xp;
    *xp = *yp;
    *yp = temp;
}

static void accelerometer_buffer_populate(
		const struct ext_sensor_evt *const acc_data)
{
	static int buf_entry_try_again_timeout;
	int j, k, n;
	int i = 0;
	double temp = 0;
	double temp_ = 0;
	s64_t newest_time = 0;

	/** Only populate accelerometer buffer if a configurable amount of time
	 *  has passed since the last accelerometer buffer entry was filled.
	 *
	 *  If the circular buffer is filled always keep the highest
         *  values in the circular buffer.
	 */
	if (k_uptime_get() - buf_entry_try_again_timeout >
		K_SECONDS(CONFIG_TIME_BETWEEN_ACCELEROMETER_BUFFER_STORE_SEC)) {

		/** Populate the next available unqueued entry. */
		for (k = 0; k < ARRAY_SIZE(accel_buf); k++) {
			if (!accel_buf[k].queued) {
				head_accel_buf = k;
				goto populate_buffer;
			}
		}

		/** Sort list after highest values using bubble sort.
		 */
		for (j = 0; j < ARRAY_SIZE(accel_buf)-i-1; j++) {
			for (n = 0; n < 3; n++) {
				if (temp < abs(accel_buf[j].values[n])) {
					temp = abs(accel_buf[j].values[n]);
				}

				if (temp_ < abs(accel_buf[j + 1].values[n])) {
					temp_ = abs(accel_buf[j + 1].values[n]);
				}
			}

			if (temp > temp_) {
				acc_array_swap(&accel_buf[j], &accel_buf[j+1]);
			}
		}


		temp = 0;
		temp_ = 0;

		/** Find highest value in new accelerometer entry. */
		for (n = 0; n < 3; n++) {
			if (temp < abs(acc_data->value_array[n])) {
				temp = abs(acc_data->value_array[n]);
			}

		}

		/** Repalce old accelerometer entry with the new entry if the
		 *  highest value in new value is greater than the old.
		 */
		for (int k = 0; k < ARRAY_SIZE(accel_buf); k++) {
			for (n = 0; n < 3; n++) {
				if (temp_ < abs(accel_buf[k].values[n])) {
					temp_ = abs(accel_buf[k].values[n]);
				}

				if (temp > temp_) {
					head_accel_buf = k;
				}
			}
		}

populate_buffer:

		accel_buf[head_accel_buf].values[0] = acc_data->value_array[0];
		accel_buf[head_accel_buf].values[1] = acc_data->value_array[1];
		accel_buf[head_accel_buf].values[2] = acc_data->value_array[2];
		accel_buf[head_accel_buf].ts = k_uptime_get();
		accel_buf[head_accel_buf].queued = true;

		LOG_INF("Entry: %d of %d in accelerometer buffer filled",
			head_accel_buf, CONFIG_ACCEL_BUFFER_MAX - 1);

		buf_entry_try_again_timeout = k_uptime_get();

		/** Always point head of buffer to the newest sampled value. */
		for(i = 0; i < ARRAY_SIZE(accel_buf); i++) {
			if (newest_time < accel_buf[i].ts &&
			    accel_buf[i].queued) {
				newest_time = accel_buf[i].ts;
				head_accel_buf = i;
			}
		}
	}
}

static int modem_buffer_populate(void)
{
	int err;

	/* Request data from modem. */
	err = modem_info_params_get(&modem_param);
	if (err) {
		LOG_ERR("modem_info_params_get, error: %d", err);
		return err;
	}

	/* Go to start of buffer if end is reached. */
	head_modem_buf += 1;
	if (head_modem_buf == CONFIG_MODEM_BUFFER_MAX) {
		head_modem_buf = 0;
	}

	modem_buf[head_modem_buf].ip =
		modem_param.network.ip_address.value_string;
	modem_buf[head_modem_buf].cell =
		modem_param.network.cellid_dec;
	modem_buf[head_modem_buf].mccmnc =
		modem_param.network.current_operator.value_string;
	modem_buf[head_modem_buf].area =
		modem_param.network.area_code.value;
	modem_buf[head_modem_buf].appv =
		CONFIG_CAT_TRACKER_APP_VERSION;
	modem_buf[head_modem_buf].brdv =
		modem_param.device.board;
	modem_buf[head_modem_buf].fw =
		modem_param.device.modem_fw.value_string;
	modem_buf[head_modem_buf].iccid =
		modem_param.sim.iccid.value_string;
	modem_buf[head_modem_buf].nw_lte_m =
		modem_param.network.lte_mode.value;
	modem_buf[head_modem_buf].nw_nb_iot =
		modem_param.network.nbiot_mode.value;
	modem_buf[head_modem_buf].nw_gps =
		modem_param.network.gps_mode.value;
	modem_buf[head_modem_buf].bnd =
		modem_param.network.current_band.value;
	modem_buf[head_modem_buf].mod_ts = k_uptime_get();
	modem_buf[head_modem_buf].mod_ts_static = k_uptime_get();
	modem_buf[head_modem_buf].queued = true;

	LOG_INF("Entry: %d of %d in modem buffer filled", head_modem_buf,
		CONFIG_MODEM_BUFFER_MAX - 1);

	return 0;
}

static int sensors_buffer_populate(void)
{
	int err;

	/* Go to start of buffer if end is reached. */
	head_sensor_buf += 1;
	if (head_sensor_buf == CONFIG_SENSOR_BUFFER_MAX) {
		head_sensor_buf = 0;
	}

	/* Request data from external sensors. */
	err = ext_sensors_temperature_get(&sensors_buf[head_sensor_buf].temp);
	if (err) {
		LOG_ERR("temperature_get, error: %d", err);
		return err;
	}

	err = ext_sensors_humidity_get(&sensors_buf[head_sensor_buf].hum);
	if (err) {
		LOG_ERR("temperature_get, error: %d", err);
		return err;
	}

	sensors_buf[head_sensor_buf].env_ts = k_uptime_get();
	sensors_buf[head_sensor_buf].queued = true;

	LOG_INF("Entry: %d of %d in sensor buffer filled", head_sensor_buf,
		CONFIG_SENSOR_BUFFER_MAX - 1);

	return 0;
}

static void ui_buffer_populate(int btn_number)
{
	/* Go to start of buffer if end is reached. */
	head_ui_buf += 1;
	if (head_ui_buf == CONFIG_UI_BUFFER_MAX) {
		head_ui_buf = 0;
	}

	ui_buf[head_ui_buf].btn = 1;
	ui_buf[head_ui_buf].btn_ts = k_uptime_get();
	ui_buf[head_ui_buf].queued = true;

	LOG_INF("Entry: %d of %d in UI buffer filled", head_ui_buf,
		CONFIG_UI_BUFFER_MAX - 1);
}

static void lte_evt_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if ((evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_HOME) &&
		    (evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			k_sem_take(&cloud_conn_sem, K_NO_WAIT);
			break;
		}

		LOG_INF("Network registration status: %s",
			evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ?
				"Connected to home network" :
				"Connected to roaming network");

		k_sem_give(&cloud_conn_sem);
		break;
	case LTE_LC_EVT_PSM_UPDATE:
		LOG_DBG("PSM parameter update: TAU: %d, Active time: %d",
			evt->psm_cfg.tau, evt->psm_cfg.active_time);
		break;
	case LTE_LC_EVT_EDRX_UPDATE: {
		char log_buf[60];
		ssize_t len;

		len = snprintf(log_buf, sizeof(log_buf),
			       "eDRX parameter update: eDRX: %f, PTW: %f",
			       evt->edrx_cfg.edrx, evt->edrx_cfg.ptw);
		if (len > 0) {
			LOG_DBG("%s", log_strdup(log_buf));
		}
		break;
	}
	case LTE_LC_EVT_RRC_UPDATE:
		LOG_DBG("RRC mode: %s",
			evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ?
				"Connected" :
				"Idle");
		break;
	case LTE_LC_EVT_CELL_UPDATE:
		LOG_DBG("LTE cell changed: Cell ID: %d, Tracking area: %d",
			evt->cell.id, evt->cell.tac);
		break;
	default:
		break;
	}
}

#if defined(CONFIG_EXTERNAL_SENSORS)
static void ext_sensors_evt_handler(const struct ext_sensor_evt *const evt)
{
	switch (evt->type) {
	case EXT_SENSOR_EVT_ACCELEROMETER_TRIGGER:
		if (!cfg.act) {
			accelerometer_buffer_populate(evt);
			k_sem_give(&accel_trig_sem);
		}
		break;
	default:
		break;
	}
}
#endif

static int modem_configure(void)
{
	int err;

	LOG_INF("Configuring the modem...");

	err = lte_lc_init_and_connect_async(lte_evt_handler);
	if (err) {
		LOG_ERR("lte_lc_init_connect_async, error: %d", err);
	}

	return err;
}

static void device_config_get(void)
{
	int err;

	struct cloud_msg msg = { .qos = CLOUD_QOS_AT_MOST_ONCE,
				 .endpoint.type = CLOUD_EP_TOPIC_STATE,
				 .buf = "",
				 .len = 0 };

	err = cloud_send(cloud_backend, &msg);
	if (err) {
		LOG_ERR("Cloud send failed, err: %d", err);
	}
}

static void ui_send(void)
{
	int err;

	ui_led_set_pattern(UI_CLOUD_PUBLISHING);

	struct cloud_msg msg = { .qos = CLOUD_QOS_AT_MOST_ONCE,
				 .endpoint = pub_ep_topics_sub[1] };

	err = cloud_codec_encode_data(&msg,
				      &gps_buf[head_gps_buf],
				      &sensors_buf[head_sensor_buf],
				      &modem_buf[head_modem_buf],
				      &ui_buf[head_ui_buf],
				      &accel_buf[head_accel_buf],
				      &bat_buf[head_bat_buf],
				      CLOUD_DATA_ENCODE_UI);
	if (err) {
		LOG_ERR("cloud_encode_button_message_data, error: %d", err);
		return;
	}

	err = cloud_send(cloud_backend, &msg);
	cloud_codec_release_data(&msg);
	if (err) {
		LOG_ERR("Cloud send failed, err: %d", err);
	}
}

static void device_config_send(void)
{
	int err;

	struct cloud_msg msg = { .qos = CLOUD_QOS_AT_MOST_ONCE,
				 .endpoint.type = CLOUD_EP_TOPIC_MSG };

	err = cloud_codec_encode_cfg_data(&msg, &cfg);
	if (err == -EAGAIN) {
		LOG_INF("No change in device configuration");
		return;
	} else if (err) {
		LOG_ERR("Device configuration not encoded, error: %d", err);
		return;
	}

	err = cloud_send(cloud_backend, &msg);
	cloud_codec_release_data(&msg);
	if (err) {
		LOG_ERR("Cloud send failed, err: %d", err);
	}
}

static void data_send(void)
{
	int err;
	enum cloud_data_encode_schema pub_schema;

	/** Data encoded depending on mode, obtained gps fix and
	 * accelerometer trigger.
	 */

	if (!initial_cloud_connection) {
		pub_schema = CLOUD_DATA_ENCODE_MSTAT_MDYN_SENS_BAT;
	} else {
		if (cfg.act && !gps_fix) {
			pub_schema = CLOUD_DATA_ENCODE_MDYN_SENS_BAT;
		}

		if (cfg.act && gps_fix) {
			pub_schema = CLOUD_DATA_ENCODE_MDYN_SENS_BAT_GPS;
		}

		if (!cfg.act && !gps_fix) {
			pub_schema = CLOUD_DATA_ENCODE_MDYN_SENS_BAT_ACCEL;
		}

		if (!cfg.act && gps_fix) {
			pub_schema = CLOUD_DATA_ENCODE_MDYN_SENS_BAT_GPS_ACCEL;
		}
	}

	struct cloud_msg msg = { .qos = CLOUD_QOS_AT_MOST_ONCE,
				 .endpoint.type = CLOUD_EP_TOPIC_MSG };

	err = cloud_codec_encode_data(&msg,
				      &gps_buf[head_gps_buf],
				      &sensors_buf[head_sensor_buf],
				      &modem_buf[head_modem_buf],
				      &ui_buf[head_ui_buf],
				      &accel_buf[head_accel_buf],
				      &bat_buf[head_bat_buf],
				      pub_schema);
	if (err) {
		LOG_ERR("Error enconding message %d", err);
		return;
	}

	err = cloud_send(cloud_backend, &msg);
	cloud_codec_release_data(&msg);
	if (err) {
		LOG_ERR("Cloud send failed, err: %d", err);
		return;
	}

	gps_fix = false;
	initial_cloud_connection = true;
}

static void buffered_data_send(void)
{
	int err;
	bool queued_entries = false;

	struct cloud_msg msg = {
		.qos = CLOUD_QOS_AT_MOST_ONCE,
		.endpoint = pub_ep_topics_sub[0],
	};

check_gps_buffer:

	/* Check if it exists queued entries in the gps buffer. */
	for (int i = 0; i < CONFIG_GPS_BUFFER_MAX; i++) {
		if (gps_buf[i].queued) {
			queued_entries = true;
			break;
		} else {
			queued_entries = false;
		}
	}

	if (queued_entries) {
		/* Encode and send queued entries in batches. */
		err = cloud_codec_encode_gps_buffer(&msg, gps_buf);
		if (err) {
			LOG_ERR("Error encoding GPS buffer: %d", err);
			return;
		}

		err = cloud_send(cloud_backend, &msg);
		cloud_codec_release_data(&msg);
		if (err) {
			LOG_ERR("Cloud send failed, err: %d", err);
			return;
		}

		goto check_gps_buffer;
	}

check_sensors_buffer:

	/* Check if it exists queued entries in the gps buffer. */
	for (int i = 0; i < CONFIG_SENSOR_BUFFER_MAX; i++) {
		if (sensors_buf[i].queued) {
			queued_entries = true;
			break;
		} else {
			queued_entries = false;
		}
	}

	if (queued_entries) {
		/* Encode and send queued entries in batches. */
		err = cloud_codec_encode_sensor_buffer(&msg, sensors_buf);
		if (err) {
			LOG_ERR("Error encoding sensors buffer: %d", err);
			return;
		}

		err = cloud_send(cloud_backend, &msg);
		cloud_codec_release_data(&msg);
		if (err) {
			LOG_ERR("Cloud send failed, err: %d", err);
			return;
		}

		goto check_sensors_buffer;
	}

check_modem_buffer:

	/* Check if it exists queued entries in the gps buffer. */
	for (int i = 0; i < CONFIG_MODEM_BUFFER_MAX; i++) {
		if (modem_buf[i].queued) {
			queued_entries = true;
			break;
		} else {
			queued_entries = false;
		}
	}

	if (queued_entries) {
		/* Encode and send queued entries in batches. */
		err = cloud_codec_encode_modem_buffer(&msg, modem_buf);
		if (err) {
			LOG_ERR("Error encoding modem buffer: %d", err);
			return;
		}

		err = cloud_send(cloud_backend, &msg);
		cloud_codec_release_data(&msg);
		if (err) {
			LOG_ERR("Cloud send failed, err: %d", err);
			return;
		}

		goto check_modem_buffer;
	}

check_ui_buffer:

	/* Check if it exists queued entries in the gps buffer. */
	for (int i = 0; i < CONFIG_UI_BUFFER_MAX; i++) {
		if (ui_buf[i].queued) {
			queued_entries = true;
			break;
		} else {
			queued_entries = false;
		}
	}

	if (queued_entries) {
		/* Encode and send queued entries in batches. */
		err = cloud_codec_encode_ui_buffer(&msg, ui_buf);
		if (err) {
			LOG_ERR("Error encoding modem buffer: %d", err);
			return;
		}

		err = cloud_send(cloud_backend, &msg);
		cloud_codec_release_data(&msg);
		if (err) {
			LOG_ERR("Cloud send failed, err: %d", err);
			return;
		}

		goto check_ui_buffer;
	}

check_accel_buffer:

	/* Check if it exists queued entries in the gps buffer. */
	for (int i = 0; i < CONFIG_ACCEL_BUFFER_MAX; i++) {
		if (accel_buf[i].queued) {
			queued_entries = true;
			break;
		} else {
			queued_entries = false;
		}
	}

	/** Only publish buffered accelerometer data if in
	 * passive device mode.
	 */
	if (queued_entries && !cfg.act) {
		/* Encode and send queued entries in batches. */
		err = cloud_codec_encode_accel_buffer(&msg, accel_buf);
		if (err) {
			LOG_ERR("Error encoding accelerometer buffer: %d", err);
			return;
		}

		err = cloud_send(cloud_backend, &msg);
		cloud_codec_release_data(&msg);
		if (err) {
			LOG_ERR("Cloud send failed, err: %d", err);
			return;
		}

		goto check_accel_buffer;
	}

check_battery_buffer:

	/* Check if it exists queued entries in the gps buffer. */
	for (int i = 0; i < CONFIG_BAT_BUFFER_MAX; i++) {
		if (bat_buf[i].queued) {
			queued_entries = true;
			break;
		} else {
			queued_entries = false;
		}
	}

	if (queued_entries) {
		/* Encode and send queued entries in batches. */
		err = cloud_codec_encode_bat_buffer(&msg, bat_buf);
		if (err) {
			LOG_ERR("Error encoding accelerometer buffer: %d", err);
			return;
		}

		err = cloud_send(cloud_backend, &msg);
		cloud_codec_release_data(&msg);
		if (err) {
			LOG_ERR("Cloud send failed, err: %d", err);
			return;
		}

		goto check_battery_buffer;
	}
}

static void config_get(void)
{
	ui_led_set_pattern(UI_CLOUD_PUBLISHING);

	/** Sample data from modem and environmental sensor before
	 *  cloud publication.
	 */
	k_delayed_work_submit(&sample_data_work, K_NO_WAIT);

	if (cloud_connected) {
		k_delayed_work_submit(&device_config_get_work, K_NO_WAIT);
		k_delayed_work_submit(&device_config_send_work, K_NO_WAIT);
		k_delayed_work_submit(&data_send_work, K_NO_WAIT);
		k_delayed_work_submit(&buffered_data_send_work, K_NO_WAIT);
	}
}

static void data_publish(void)
{
	ui_led_set_pattern(UI_CLOUD_PUBLISHING);

	/** Sample data from modem and environmental sensor before
	 *  cloud publication.
	 */
	k_delayed_work_submit(&sample_data_work, K_NO_WAIT);

	if (cloud_connected) {
		k_delayed_work_submit(&data_send_work, K_NO_WAIT);
		k_delayed_work_submit(&buffered_data_send_work, K_NO_WAIT);
	}
}

static void sample_data_work_fn(struct k_work *work)
{
	int err;

	err = modem_buffer_populate();
	if (err) {
		LOG_ERR("modem_buffer_populate, error: %d", err);
		return;
	}

	battery_buffer_populate();

#if defined(CONFIG_EXTERNAL_SENSORS)
	err = sensors_buffer_populate();
	if (err) {
		LOG_ERR("sensors_buffer_populate, error: %d", err);
		return;
	}
#endif
}

static void leds_set_work_fn(struct k_work *work)
{
	leds_set();
}

static void device_config_get_work_fn(struct k_work *work)
{
	device_config_get();
}

static void device_config_send_work_fn(struct k_work *work)
{
	device_config_send();
}

static void data_send_work_fn(struct k_work *work)
{
	data_send();
}

static void buffered_data_send_work_fn(struct k_work *work)
{
	buffered_data_send();
}

static void ui_send_work_fn(struct k_work *work)
{
	ui_send();
}

static void mov_timeout_work_fn(struct k_work *work)
{
	if (!cfg.act) {
		LOG_INF("Movement timeout triggered");
		k_sem_give(&accel_trig_sem);
	}

	k_delayed_work_submit(&mov_timeout_work,
			      K_SECONDS(cfg.movt));
}

static void work_init(void)
{
	k_delayed_work_init(&device_config_get_work,
			    device_config_get_work_fn);
	k_delayed_work_init(&data_send_work,
			    data_send_work_fn);
	k_delayed_work_init(&device_config_send_work,
			    device_config_send_work_fn);
	k_delayed_work_init(&buffered_data_send_work,
			    buffered_data_send_work_fn);
	k_delayed_work_init(&leds_set_work,
			    leds_set_work_fn);
	k_delayed_work_init(&mov_timeout_work,
			    mov_timeout_work_fn);
	k_delayed_work_init(&ui_send_work,
			    ui_send_work_fn);
	k_delayed_work_init(&sample_data_work,
			    sample_data_work_fn);
}

static void gps_trigger_handler(struct device *dev, struct gps_event *evt)
{
	switch (evt->type) {
	case GPS_EVT_SEARCH_STARTED:
		LOG_INF("GPS_EVT_SEARCH_STARTED");
		break;
	case GPS_EVT_SEARCH_STOPPED:
		LOG_INF("GPS_EVT_SEARCH_STOPPED");
		break;
	case GPS_EVT_SEARCH_TIMEOUT:
		LOG_INF("GPS_EVT_SEARCH_TIMEOUT");
		gps_control_set_active(false);
		k_sem_give(&gps_timeout_sem);
		break;
	case GPS_EVT_PVT:
		/* Don't spam logs */
		break;
	case GPS_EVT_PVT_FIX:
		LOG_INF("GPS_EVT_PVT_FIX");
		gps_control_set_active(false);
		time_set(&evt->pvt);
		gps_buffer_populate(&evt->pvt);
		gps_fix = true;
		k_sem_give(&gps_timeout_sem);
		break;
	case GPS_EVT_NMEA:
		/* Don't spam logs */
		break;
	case GPS_EVT_NMEA_FIX:
		LOG_INF("Position fix with NMEA data");
		break;
	case GPS_EVT_OPERATION_BLOCKED:
		LOG_INF("GPS_EVT_OPERATION_BLOCKED");
		break;
	case GPS_EVT_OPERATION_UNBLOCKED:
		LOG_INF("GPS_EVT_OPERATION_UNBLOCKED");
		ui_led_set_pattern(UI_LED_GPS_SEARCHING);
		break;
	case GPS_EVT_AGPS_DATA_NEEDED:
		LOG_INF("GPS_EVT_AGPS_DATA_NEEDED");
		break;
	case GPS_EVT_ERROR:
		LOG_INF("GPS_EVT_ERROR\n");
		break;
	default:
		break;
	}
}

void cloud_event_handler(const struct cloud_backend *const backend,
			 const struct cloud_event *const evt, void *user_data)
{
	ARG_UNUSED(user_data);

	int err;

	switch (evt->type) {
	case CLOUD_EVT_CONNECTED:
		LOG_INF("CLOUD_EVT_CONNECTED");
		cloud_connected = true;
		config_get();
		boot_write_img_confirmed();
		break;
	case CLOUD_EVT_READY:
		LOG_INF("CLOUD_EVT_READY");
		err = lte_lc_psm_req(true);
		if (err) {
			LOG_ERR("PSM request failed, error: %d", err);
		} else {
			LOG_INF("PSM enabled");
		}
		break;
	case CLOUD_EVT_DISCONNECTED:
		LOG_INF("CLOUD_EVT_DISCONNECTED");
		cloud_connected = false;
		break;
	case CLOUD_EVT_ERROR:
		LOG_ERR("CLOUD_EVT_ERROR");
		break;
	case CLOUD_EVT_FOTA_START:
		LOG_INF("CLOUD_EVT_FOTA_START");
		break;
	case CLOUD_EVT_FOTA_ERASE_PENDING:
		LOG_INF("CLOUD_EVT_FOTA_ERASE_PENDING");
		break;
	case CLOUD_EVT_FOTA_ERASE_DONE:
		LOG_INF("CLOUD_EVT_FOTA_ERASE_DONE");
		break;
	case CLOUD_EVT_FOTA_DONE:
		LOG_INF("CLOUD_EVT_FOTA_DONE");
		cloud_disconnect(cloud_backend);
		sys_reboot(0);
		break;
	case CLOUD_EVT_DATA_SENT:
		LOG_INF("CLOUD_EVT_DATA_SENT");
		break;
	case CLOUD_EVT_DATA_RECEIVED:
		LOG_INF("CLOUD_EVT_DATA_RECEIVED");
		err = cloud_codec_decode_response(evt->data.msg.buf, &cfg);
		if (err) {
			LOG_ERR("Could not decode response %d", err);
		}
		ext_sensors_accelerometer_threshold_set(cfg.acct);
		k_delayed_work_submit(&device_config_send_work, K_NO_WAIT);
		break;
	case CLOUD_EVT_PAIR_REQUEST:
		LOG_INF("CLOUD_EVT_PAIR_REQUEST");
		break;
	case CLOUD_EVT_PAIR_DONE:
		LOG_INF("CLOUD_EVT_PAIR_DONE");
		break;
	default:
		LOG_ERR("Unknown cloud event type: %d", evt->type);
		break;
	}
}

void cloud_poll(void)
{
	int err;
	int cloud_connect_retries = 0;
	int retry_backoff_s = 0;

	k_sem_take(&cloud_conn_sem, K_FOREVER);

connect:

	if (cloud_connect_retries >= CONFIG_CLOUD_RECONNECT_RETRIES) {
		LOG_ERR("Too many cloud connect retires, reboot");
		error_handler(-EIO);
	}

	/* Exponential backoff in case of disconnect from
	 * cloud.
	 */

	retry_backoff_s = 10 + pow(cloud_connect_retries, 4);
	cloud_connect_retries++;

	LOG_INF("Trying to connect to cloud in %d seconds", retry_backoff_s);

	/** Sleep in order to make sure time has been pushed to the modem. */
	k_sleep(K_SECONDS(5));

	date_time_update();

	k_sleep(K_SECONDS(retry_backoff_s));

	err = cloud_connect(cloud_backend);
	if (err) {
		LOG_ERR("cloud_connect failed: %d", err);
		goto connect;
	}

	cloud_connect_retries++;

	struct pollfd fds[] = { { .fd = cloud_backend->config->socket,
				  .events = POLLIN } };

	while (true) {
		err = poll(fds, ARRAY_SIZE(fds),
			   cloud_keepalive_time_left(cloud_backend));

		if (err < 0) {
			LOG_ERR("poll, error: %d", err);
			error_handler(err);
			continue;
		}

		if (err == 0) {
			cloud_ping(cloud_backend);
			LOG_INF("Cloud ping!");
			continue;
		}

		if ((fds[0].revents & POLLIN) == POLLIN) {
			cloud_input(cloud_backend);
		}

		if ((fds[0].revents & POLLNVAL) == POLLNVAL) {
			LOG_ERR("Socket error: POLLNVAL");
			LOG_ERR("The cloud socket was unexpectedly closed.");
			error_handler(-EIO);
			return;
		}

		if ((fds[0].revents & POLLHUP) == POLLHUP) {
			LOG_ERR("Socket error: POLLHUP");
			LOG_ERR("Connection was closed by the cloud.");
			LOG_ERR("TRYING TO RECONNECT...");
			break;
		}

		if ((fds[0].revents & POLLERR) == POLLERR) {
			LOG_ERR("Socket error: POLLERR");
			LOG_ERR("Cloud connection was unexpectedly closed.");
			error_handler(-EIO);
			return;
		}
	}

	cloud_disconnect(cloud_backend);
	goto connect;
}

K_THREAD_DEFINE(cloud_poll_thread, CONFIG_CLOUD_POLL_STACKSIZE, cloud_poll,
		NULL, NULL, NULL, CONFIG_CLOUD_POLL_PRIORITY, 0, K_NO_WAIT);

static void modem_rsrp_handler(char rsrp_value)
{
	/* RSRP raw values that represent actual signal strength are
	 * 0 through 97 (per "nRF91 AT Commands" v1.1).
	 */

	if (rsrp_value > 97) {
		return;
	}

	modem_buf[head_modem_buf].rsrp = rsrp_value;

	LOG_INF("Incoming RSRP status message, RSRP value is %d",
		modem_buf[head_modem_buf].rsrp);
}

static int modem_data_init(void)
{
	int err;

	err = modem_info_init();
	if (err) {
		LOG_INF("modem_info_init, error: %d", err);
		return err;
	}

	err = modem_info_params_init(&modem_param);
	if (err) {
		LOG_INF("modem_info_params_init, error: %d", err);
		return err;
	}

	err = modem_info_rsrp_register(modem_rsrp_handler);
	if (err) {
		LOG_INF("modem_info_rsrp_register, error: %d", err);
		return err;
	}

	return 0;
}

static void button_handler(u32_t button_states, u32_t has_changed)
{
	static int try_again_timeout;

	/* Publication of data due to button presses limited
	 * to 1 push every 2 seconds to avoid spamming the cloud socket.
	 */
	if ((has_changed & button_states & DK_BTN1_MSK) &&
	    k_uptime_get() - try_again_timeout > K_SECONDS(2)) {
		LOG_INF("Cloud publication by button 1 triggered, ");
		LOG_INF("2 seconds to next allowed cloud publication ");
		LOG_INF("triggered by button 1");

		ui_buffer_populate(1);

		if (cloud_connected) {
			k_delayed_work_submit(&ui_send_work, K_NO_WAIT);
			k_delayed_work_submit(&leds_set_work,
					      K_SECONDS(3));
		}

		try_again_timeout = k_uptime_get();
	}

#if defined(CONFIG_BOARD_NRF9160_PCA10090NS)
	/* Fake motion. The nRF9160 DK does not have an accelerometer by
	 * default. Reset accelerometer data.
	 */
	if (has_changed & button_states & DK_BTN2_MSK) {
		k_sem_give(&accel_trig_sem);
	}
#endif
}

static int populate_app_endpoint_topics()
{
	int err;

	err = snprintf(batch_topic, sizeof(batch_topic), BATCH_TOPIC,
		       client_id_buf);
	if (err != BATCH_TOPIC_LEN) {
		return -ENOMEM;
	}

	pub_ep_topics_sub[0].str = batch_topic;
	pub_ep_topics_sub[0].len = BATCH_TOPIC_LEN;
	pub_ep_topics_sub[0].type = CLOUD_EP_TOPIC_BATCH;

	err = snprintf(messages_topic, sizeof(messages_topic), MESSAGES_TOPIC,
		       client_id_buf);
	if (err != MESSAGES_TOPIC_LEN) {
		return -ENOMEM;
	}

	pub_ep_topics_sub[1].str = messages_topic;
	pub_ep_topics_sub[1].len = MESSAGES_TOPIC_LEN;
	pub_ep_topics_sub[1].type = CLOUD_EP_TOPIC_MESSAGES;

	err = snprintf(cfg_topic, sizeof(cfg_topic), CFG_TOPIC, client_id_buf);
	if (err != CFG_TOPIC_LEN) {
		return -ENOMEM;
	}

	sub_ep_topics_sub[0].str = cfg_topic;
	sub_ep_topics_sub[0].len = CFG_TOPIC_LEN;
	sub_ep_topics_sub[0].type = CLOUD_EP_TOPIC_CONFIG;

	err = cloud_ep_subscriptions_add(cloud_backend, sub_ep_topics_sub,
					 ARRAY_SIZE(sub_ep_topics_sub));
	if (err) {
		LOG_INF("cloud_ep_subscriptions_add, error: %d", err);
		error_handler(err);
	}

	return 0;
}

static int cloud_setup(void)
{
	int err;

	cloud_backend = cloud_get_binding(CONFIG_CLOUD_BACKEND);
	__ASSERT(cloud_backend != NULL, "%s cloud backend not found",
		 CONFIG_CLOUD_BACKEND);

	err = modem_info_string_get(MODEM_INFO_IMEI, client_id_buf,
				    sizeof(client_id_buf));
	if (err != AWS_CLOUD_CLIENT_ID_LEN) {
		LOG_ERR("modem_info_string_get, error: %d", err);
		return err;
	}

	LOG_INF("Device IMEI: %s", log_strdup(client_id_buf));

	/* Fetch IMEI from modem data and set IMEI as cloud connection ID **/
	cloud_backend->config->id = client_id_buf;
	cloud_backend->config->id_len = sizeof(client_id_buf);

	err = cloud_init(cloud_backend, cloud_event_handler);
	if (err) {
		LOG_ERR("cloud_init, error: %d", err);
		return err;
	}

	/* Populate cloud spesific endpoint topics */
	err = populate_app_endpoint_topics();
	if (err) {
		LOG_ERR("populate_app_endpoint_topics, error: %d", err);
		return err;
	}

	return err;
}

void main(void)
{
	int err;

	LOG_INF("The cat tracker has started");
	LOG_INF("Version: %s", log_strdup(CONFIG_CAT_TRACKER_APP_VERSION));

#if defined(CONFIG_WATCHDOG)
	err = watchdog_init_and_start();
	if (err) {
		LOG_INF("watchdog_init_and_start, error: %d", err);
		error_handler(err);
	}
#endif

	work_init();

#if defined(CONFIG_EXTERNAL_SENSORS)
	err = ext_sensors_init(ext_sensors_evt_handler);
	if (err) {
		LOG_INF("ext_sensors_init, error: %d", err);
		error_handler(err);
	}
#endif
	err = dk_buttons_init(button_handler);
	if (err) {
		LOG_INF("dk_buttons_init, error: %d", err);
		error_handler(err);
	}

	err = modem_data_init();
	if (err) {
		LOG_INF("modem_data_init, error: %d", err);
		error_handler(err);
	}

	err = cloud_setup();
	if (err) {
		LOG_INF("cloud_setup, error %d", err);
		error_handler(err);
	}

	err = ui_init();
	if (err) {
		LOG_INF("ui_init, error: %d", err);
		error_handler(err);
	}

	err = gps_control_init(gps_trigger_handler);
	if (err) {
		LOG_INF("gps_control_init, error %d", err);
		error_handler(err);
	}

	err = modem_configure();
	if (err) {
		LOG_INF("modem_configure, error: %d", err);
		error_handler(err);
	}

	/* Start movement timer which triggers every movement timeout.
	 * Makes sure the device publishes every once and a while even
	 * though the device is in passive mode and movement is not detected.
	 */
	k_delayed_work_submit(&mov_timeout_work,
			      K_SECONDS(cfg.movt));

	while (true) {
		/*Check current device mode*/
		if (!cfg.act) {
			LOG_INF("Device in PASSIVE mode");
			k_delayed_work_submit(&leds_set_work,
					      K_NO_WAIT);
			if (!k_sem_take(&accel_trig_sem, K_FOREVER)) {
				LOG_INF("The cat is moving!");
			}
		} else {
			LOG_INF("Device in ACTIVE mode");
		}

		/** Start GPS search, disable GPS if gpst is set to 0. */
		if (cfg.gpst > 0) {
			gps_control_start(K_NO_WAIT, cfg.gpst);

			/*Wait for GPS search timeout*/
			k_sem_take(&gps_timeout_sem, K_FOREVER);
		}

		/*Send update to cloud. */
		data_publish();

		/* Set device mode led behaviour */
		k_delayed_work_submit(&leds_set_work, K_SECONDS(15));

		/*Sleep*/
		LOG_INF("Going to sleep for: %d seconds", device_mode_check());
		k_sleep(K_SECONDS(device_mode_check()));
	}
}
