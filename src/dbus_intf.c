#include <glib.h>
#include <dbus/dbus-glib.h>

#include "dbus_intf.h"
#include "util.h"
#include "omgps.h"

/**
 * NOTE: the policy is: when this program detect conflict (gypsy is running), it
 * switches engine to gypsy. Else built-in UBX binary is used.
 */
static DBusGConnection *connection = NULL;

static pthread_mutex_t *lock = NULL;
static gps_data_t *gpsdata = NULL;

static DBusGProxy *device_proxy = NULL;
static DBusGProxy *position_proxy = NULL;
static DBusGProxy *accuracy_proxy = NULL;
static DBusGProxy *satellite_proxy = NULL;
static DBusGProxy *course_proxy = NULL;

static gboolean registered = FALSE;
static gboolean connecting = FALSE;
static gboolean stopping = FALSE;

#define GYPSY 						"org.freedesktop.Gypsy"
#define GYPSY_PATH					"/org/freedesktop/Gypsy"

#define POSITION_CHANGED			"PositionChanged"
#define ACCURACY_CHANGED			"AccuracyChanged"
#define COURSE_CHANGED				"CourseChanged"
#define SATELLITES_CHANGED			"SatellitesChanged"
#define CONNECTION_STATUS_CHANGED	"ConnectionStatusChanged"
#define FIX_STATUS_CHANGED			"FixStatusChanged"

#define GET_POSITION				"GetPosition"
#define GET_ACCURACCY				"GetAccuracy"
#define GET_COURSE					"GetCourse"
#define GET_SATELLITES				"GetSatellites"
#define GET_CONNECTION_STATUS		"GetConnectionStatus"
#define GET_FIX_STATUS				"GetFixStatus"

#define KNOTS_TO_MPS 				0.51444444

static DBusGProxy *power_supply_proxy = NULL;
static DBusGProxy *usage_proxy = NULL;
static gboolean initialized = FALSE;
static gboolean power_low = FALSE;

static int init_gps_proxy_signal();
static void reset_resources();
static gboolean dbus_get_connection();

static void marshal_VOID__INT_INT_DOUBLE_DOUBLE_DOUBLE(GClosure *closure, GValue *return_value G_GNUC_UNUSED,
		guint n_param_values, const GValue *param_values, gpointer invocation_hint G_GNUC_UNUSED,
		gpointer marshal_data)
{
	typedef void (*GMarshalFunc_VOID__INT_INT_DOUBLE_DOUBLE_DOUBLE)(gpointer data1, gint arg_1,
			gint arg_2, gdouble arg_3, gdouble arg_4, gdouble arg_5, gpointer data2);

	GMarshalFunc_VOID__INT_INT_DOUBLE_DOUBLE_DOUBLE callback;
	GCClosure *cc = (GCClosure*) closure;
	gpointer data1, data2;

	g_return_if_fail(n_param_values == 6);

	if (G_CCLOSURE_SWAP_DATA(closure)) {
		data1 = closure->data;
		data2 = g_value_peek_pointer(param_values + 0);
	} else {
		data1 = g_value_peek_pointer(param_values + 0);
		data2 = closure->data;
	}

	callback = (GMarshalFunc_VOID__INT_INT_DOUBLE_DOUBLE_DOUBLE) (marshal_data
			? marshal_data
			: cc->callback);

	callback(data1, g_value_get_int(param_values + 1), g_value_get_int(param_values + 2),
			g_value_get_double(param_values + 3), g_value_get_double(param_values + 4),
			g_value_get_double(param_values + 5), data2);
}

static void marshal_VOID__INT_DOUBLE_DOUBLE_DOUBLE(GClosure *closure, GValue *return_value G_GNUC_UNUSED,
		guint n_param_values, const GValue *param_values, gpointer invocation_hint G_GNUC_UNUSED,
		gpointer marshal_data)
{
	typedef void (*GMarshalFunc_VOID__INT_DOUBLE_DOUBLE_DOUBLE)(gpointer data1, gint arg_1,
			gdouble arg_2, gdouble arg_3, gdouble arg_4, gpointer data2);

	GMarshalFunc_VOID__INT_DOUBLE_DOUBLE_DOUBLE callback;
	GCClosure *cc = (GCClosure*) closure;
	gpointer data1, data2;

	g_return_if_fail(n_param_values == 5);

	if (G_CCLOSURE_SWAP_DATA(closure)) {
		data1 = closure->data;
		data2 = g_value_peek_pointer(param_values + 0);
	} else {
		data1 = g_value_peek_pointer(param_values + 0);
		data2 = closure->data;
	}

	callback = (GMarshalFunc_VOID__INT_DOUBLE_DOUBLE_DOUBLE) (marshal_data
			? marshal_data
			: cc->callback);

	callback(data1, g_value_get_int(param_values + 1), g_value_get_double(param_values + 2),
			g_value_get_double(param_values + 3), g_value_get_double(param_values + 4), data2);
}

static void position_changed(DBusGProxy *proxy, int fields, int timestamp, double lat, double lon,
		double alt, gpointer user_data)
{
	LOCK_MUTEX(lock);

	gpsdata->llh_itow = (U4) timestamp;
	if (fields & 1)
		gpsdata->lat = lat;
	if (fields & 2)
		gpsdata->lon = lon;
	if (fields & 4)
		gpsdata->height = alt;

	UNLOCK_MUTEX(lock);
}

static void accuracy_changed(DBusGProxy *proxy, int fields, double pdop, double hdop, double vdop,
		gpointer user_data)
{
	LOCK_MUTEX(lock);

	if (fields & 2)
		gpsdata->hacc = hdop;
	if (fields & 4)
		gpsdata->vacc = vdop;

	UNLOCK_MUTEX(lock);
}

static void course_changed(DBusGProxy *proxy, int fields, int timestamp, double speed,
		double direction, double climb, gpointer user_data)
{
	LOCK_MUTEX(lock);

	//gpsdata->vel_itow = (U4) timestamp;
	if (fields & 1)
		gpsdata->speed_2d = speed * KNOTS_TO_MPS;
	if (fields & 2)
		gpsdata->heading_2d = direction;
	if (fields & 4) {
		gpsdata->vel_down = climb * KNOTS_TO_MPS;
	}

	UNLOCK_MUTEX(lock);
}

static void parse_sv(GPtrArray *satellites)
{
	/* The statistics is not very accurate */
	gpsdata->sv_in_use = 0;
	gpsdata->sv_get_signal = 0;

	if (!satellites) {
		gpsdata->svinfo_valid = FALSE;
		return;
	}

	gpsdata->svinfo_valid = TRUE;

	int i, j;
	GValueArray *val;
	svinfo_channel_t *sv;

	j = 0;
	for (i = 0; i < satellites->len; i++) {
		val = satellites->pdata[i];

		sv = &gpsdata->sv_channels[j];
		sv->sv_id = g_value_get_uint(g_value_array_get_nth(val, 0));

		if (g_value_get_boolean(g_value_array_get_nth(val, 1))) {
			++gpsdata->sv_in_use;
			sv->flags = 0x01;
		}

		sv->elevation = (int)g_value_get_uint(g_value_array_get_nth(val, 2));
		sv->azimuth = (int)g_value_get_uint(g_value_array_get_nth(val, 3));
		sv->cno = g_value_get_uint(g_value_array_get_nth(val, 4));
		if (sv->cno > 0)
			++gpsdata->sv_get_signal;

		if (++j == SV_MAX_CHANNELS)
			break;
	}
	gpsdata->sv_channel_count = j;
}

static void satellites_changed(DBusGProxy *proxy, GPtrArray *satellites, gpointer user_data)
{
	LOCK_MUTEX(lock);
	parse_sv(satellites);
	UNLOCK_MUTEX(lock);
}

static void get_position_notify(DBusGProxy *proxy, DBusGProxyCall *call, gpointer data)
{
	GError *error = NULL;
	int fields = 0, timestamp = 0;
	double latitude = 0.0, longitude = 0.0, altitude = 0.0;

	gboolean ok = dbus_g_proxy_end_call(proxy, call, &error, G_TYPE_INT, &fields, G_TYPE_INT,
			&timestamp, G_TYPE_DOUBLE, &latitude, G_TYPE_DOUBLE, &longitude, G_TYPE_DOUBLE,
			&altitude, G_TYPE_INVALID);

	if (!ok) {
		log_warn("GetPosition: %s", error->message);
		g_error_free(error);
	} else {
		position_changed(NULL, fields, timestamp, latitude, longitude, altitude, NULL);
	}
}

static void get_accuracy_notify(DBusGProxy *proxy, DBusGProxyCall *call, gpointer data)
{
	GError *error = NULL;
	int fields = 0;
	double pdop = 0.0, hdop = 0.0, vdop = 0.0;

	gboolean ok = dbus_g_proxy_end_call(proxy, call, &error, G_TYPE_INT, &fields, G_TYPE_DOUBLE,
			&pdop, G_TYPE_DOUBLE, &hdop, G_TYPE_DOUBLE, &vdop, G_TYPE_INVALID);

	if (!ok) {
		log_warn("GetAccuracy: %s", error->message);
		g_error_free(error);
	} else {
		accuracy_changed(NULL, fields, pdop, hdop, vdop, NULL);
	}
}

static void get_course_notify(DBusGProxy *proxy, DBusGProxyCall *call, gpointer data)
{
	GError *error = NULL;
	int fields = 0, timestamp = 0;
	double speed = 0.0, direction = 0.0, climb = 0.0;

	gboolean ok = dbus_g_proxy_end_call(proxy, call, &error, G_TYPE_INT, &fields, G_TYPE_INT,
			&timestamp, G_TYPE_DOUBLE, &speed, G_TYPE_DOUBLE, &direction, G_TYPE_DOUBLE, &climb,
			G_TYPE_INVALID);

	if (!ok) {
		log_warn("GetCourse: %s", error->message);
		g_error_free(error);
	} else {
		course_changed(NULL, fields, timestamp, speed, direction, climb, NULL);
	}
}

static void get_satellites_notify(DBusGProxy *proxy, DBusGProxyCall *call, gpointer data)
{
	GError *error = NULL;
	GPtrArray *satellites = NULL;

	gboolean ok = dbus_g_proxy_end_call(proxy, call, &error, dbus_g_type_get_collection(
			"GPtrArray", dbus_g_type_get_struct("GValueArray", G_TYPE_UINT, G_TYPE_BOOLEAN,
			G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_INVALID)), &satellites,
			G_TYPE_INVALID);

	if (!ok) {
		log_warn("GetSatellites: %s", error->message);
		g_error_free(error);
	} else {
		satellites_changed(NULL, satellites, NULL);
	}
}

static void reset_resources()
{
	if (device_proxy) {
		g_object_unref(G_OBJECT(device_proxy));
		device_proxy = NULL;
	}

	if (position_proxy) {
		g_object_unref(G_OBJECT(position_proxy));
		position_proxy = NULL;
	}

	if (accuracy_proxy) {
		g_object_unref(G_OBJECT(accuracy_proxy));
		accuracy_proxy = NULL;
	}

	if (course_proxy) {
		g_object_unref(G_OBJECT(course_proxy));
		course_proxy = NULL;
	}

	if (satellite_proxy) {
		g_object_unref(G_OBJECT(satellite_proxy));
		satellite_proxy = NULL;
	}

	registered = FALSE;
}

static void re_initialize_gypsy()
{
	init_gpsdata(gpsdata);
	reset_resources();
	connecting = (fso_gypsy_init(gpsdata, lock));
}

static void connection_status_changed(DBusGProxy *proxy, gboolean connected, gpointer user_data)
{
	LOCK_MUTEX(lock);

	if (!connected) {
		log_info("connection status changed to FALSE, reset");
		re_initialize_gypsy();
	} else {
		log_info("connection status changed to: TRUE");
	}
	connecting = connected;

	UNLOCK_MUTEX(lock);
}

static void fix_status_changed(DBusGProxy *proxy, int fixstatus, gpointer user_data)
{
	LOCK_MUTEX(lock);
	/* 1: no fix, 2: 2D, 3: 3D */
	if (fixstatus == 1) {
		/* reset */
		init_gpsdata(gpsdata);
		gpsdata->nav_status_fixtype = 0;
	} else {
		gpsdata->nav_status_fixtype = fixstatus;
	}

	UNLOCK_MUTEX(lock);
}

static void get_connection_status_notify(DBusGProxy *proxy, DBusGProxyCall *call, gpointer data)
{
	GError *error = NULL;
	gboolean connected = FALSE;

	gboolean ok = dbus_g_proxy_end_call(proxy, call, &error, G_TYPE_BOOLEAN, &connected,
			G_TYPE_INVALID);

	if (!ok) {
		log_warn("GetConnectionStatus: %s", error->message);
		g_error_free(error);
	} else {
		connection_status_changed(NULL, connected, NULL);
	}
}

static void get_fix_status_notify(DBusGProxy *proxy, DBusGProxyCall *call, gpointer data)
{
	GError *error = NULL;
	int fixstatus = 0;

	gboolean ok =
			dbus_g_proxy_end_call(proxy, call, &error, G_TYPE_INT, &fixstatus, G_TYPE_INVALID);

	if (!ok) {
		log_warn("GetFixStatus: %s", error->message);
		g_error_free(error);
	} else {
		fix_status_changed(NULL, fixstatus, NULL);
	}
}

static void register_signals()
{
	if (!registered) {
		dbus_g_object_register_marshaller(marshal_VOID__INT_DOUBLE_DOUBLE_DOUBLE, G_TYPE_NONE,
				G_TYPE_INT, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_INVALID);
		dbus_g_object_register_marshaller(marshal_VOID__INT_INT_DOUBLE_DOUBLE_DOUBLE, G_TYPE_NONE,
				G_TYPE_INT, G_TYPE_INT, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_INVALID);

		dbus_g_proxy_add_signal(position_proxy, POSITION_CHANGED, G_TYPE_INT, G_TYPE_INT,
				G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_INVALID);
		dbus_g_proxy_add_signal(accuracy_proxy, ACCURACY_CHANGED, G_TYPE_INT, G_TYPE_DOUBLE,
				G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_INVALID);
		dbus_g_proxy_add_signal(course_proxy, COURSE_CHANGED, G_TYPE_INT, G_TYPE_INT,
				G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_INVALID);
		dbus_g_proxy_add_signal(satellite_proxy, SATELLITES_CHANGED, dbus_g_type_get_collection(
				"GPtrArray", dbus_g_type_get_struct("GValueArray", G_TYPE_UINT, G_TYPE_BOOLEAN,
						G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_INVALID)), G_TYPE_INVALID);
		registered = TRUE;
	}

	dbus_g_proxy_connect_signal(position_proxy, POSITION_CHANGED, G_CALLBACK(position_changed),
			NULL, NULL);
	dbus_g_proxy_connect_signal(accuracy_proxy, ACCURACY_CHANGED, G_CALLBACK(accuracy_changed),
			NULL, NULL);
	dbus_g_proxy_connect_signal(course_proxy, COURSE_CHANGED, G_CALLBACK(course_changed), NULL,
			NULL);
	dbus_g_proxy_connect_signal(satellite_proxy, SATELLITES_CHANGED,
			G_CALLBACK(satellites_changed), NULL, NULL);
}

static void unregister_signals()
{
	dbus_g_proxy_disconnect_signal(position_proxy, POSITION_CHANGED, G_CALLBACK(position_changed),
			NULL);
	dbus_g_proxy_disconnect_signal(accuracy_proxy, ACCURACY_CHANGED, G_CALLBACK(accuracy_changed),
			NULL);
	dbus_g_proxy_disconnect_signal(course_proxy, COURSE_CHANGED, G_CALLBACK(course_changed), NULL);
	dbus_g_proxy_disconnect_signal(satellite_proxy, SATELLITES_CHANGED,
			G_CALLBACK(satellites_changed), NULL);
}

static int init_gps_proxy_signal()
{
	dbus_g_proxy_add_signal(device_proxy, CONNECTION_STATUS_CHANGED, G_TYPE_BOOLEAN, G_TYPE_INVALID);
	dbus_g_proxy_connect_signal(device_proxy, CONNECTION_STATUS_CHANGED,
			G_CALLBACK(connection_status_changed), NULL, NULL);

	dbus_g_proxy_add_signal(device_proxy, FIX_STATUS_CHANGED, G_TYPE_INT, G_TYPE_INVALID);
	dbus_g_proxy_connect_signal(device_proxy, FIX_STATUS_CHANGED, G_CALLBACK(fix_status_changed),
			NULL, NULL);

	position_proxy = dbus_g_proxy_new_for_name(connection, GYPSY, GYPSY_PATH, GYPSY".Position");
	accuracy_proxy = dbus_g_proxy_new_for_name(connection, GYPSY, GYPSY_PATH, GYPSY".Accuracy");
	course_proxy = dbus_g_proxy_new_for_name(connection, GYPSY, GYPSY_PATH, GYPSY".Course");
	satellite_proxy = dbus_g_proxy_new_for_name(connection, GYPSY, GYPSY_PATH, GYPSY".Satellite");

	register_signals();
	dbus_g_proxy_begin_call(device_proxy, GET_CONNECTION_STATUS, get_connection_status_notify,
			NULL, NULL, G_TYPE_INVALID);
	dbus_g_proxy_begin_call(device_proxy, GET_FIX_STATUS, get_fix_status_notify, NULL, NULL,
			G_TYPE_INVALID);
	dbus_g_proxy_begin_call(position_proxy, GET_POSITION, get_position_notify, NULL, NULL,
			G_TYPE_INVALID);
	dbus_g_proxy_begin_call(accuracy_proxy, GET_ACCURACCY, get_accuracy_notify, NULL, NULL,
			G_TYPE_INVALID);
	dbus_g_proxy_begin_call(course_proxy, GET_COURSE, get_course_notify, NULL, NULL,
			G_TYPE_INVALID);
	dbus_g_proxy_begin_call(satellite_proxy, GET_SATELLITES, get_satellites_notify, NULL, NULL,
			G_TYPE_INVALID);

	return TRUE;
}

/**
 * it seems if resource "GPS" is not enabled, dbus returns error, right?
 */
int fso_gypsy_get_users()
{
	GError *error = NULL;
	int i = 0;
	char **name_list;
	char **name_list_ptr;

	if (! dbus_get_connection())
		return 0;

	DBusGProxy *proxy = dbus_g_proxy_new_for_name(connection, "org.freesmartphone.ousaged",
			"/org/freesmartphone/Usage", "org.freesmartphone.Usage");

	gboolean ok = dbus_g_proxy_call_with_timeout(proxy, "GetResourceUsers", PROXY_CALL_TIMEOUT,
			&error, G_TYPE_STRING, "GPS", G_TYPE_INVALID, G_TYPE_STRV, &name_list, G_TYPE_INVALID);

	g_object_unref(proxy);

	if (!ok) {
		g_printerr("Error: %s\n", error->message);
		g_error_free(error);
	} else {
		for (name_list_ptr = name_list; *name_list_ptr; name_list_ptr++)
			++i;
		g_strfreev(name_list);
	}

	return i;
}

/**
 * it seems if resource "GPS" is not enabled, dbus returns error, right?
 */
static gboolean fso_gypsy_is_connecting()
{
	GError *error = NULL;
	gboolean connected = FALSE;

	if (! dbus_get_connection())
		return FALSE;

	if (!device_proxy)
		device_proxy = dbus_g_proxy_new_for_name(connection, GYPSY, GYPSY_PATH, GYPSY".Device");

	gboolean ok = dbus_g_proxy_call_with_timeout(device_proxy, GET_CONNECTION_STATUS,
			PROXY_CALL_TIMEOUT, &error, G_TYPE_INVALID, G_TYPE_BOOLEAN, &connected, G_TYPE_INVALID);

	if (!ok) {
		//log_warn("dbus error: domain=%d, code=%d, msg=%s", error->domain, error->code, error->message);
		g_error_free(error);
		return FALSE;
	}

	return connected;
}

gboolean fso_gypsy_is_running()
{
	return fso_gypsy_is_connecting() && (fso_gypsy_get_users() > 0);
}

gboolean fso_gypsy_init(gps_data_t *_gpsdata, pthread_mutex_t *_lock)
{
	stopping = FALSE;
	gpsdata = _gpsdata;
	lock = _lock;

	init_gpsdata(gpsdata);

	log_info("Gypsy: initializing...");

	if (! dbus_get_connection())
		return FALSE;

	/* already connected */
	if (! fso_gypsy_is_running())
		return FALSE;

	if (! device_proxy)
		device_proxy = dbus_g_proxy_new_for_name(connection, GYPSY, GYPSY_PATH, GYPSY".Device");

	init_gps_proxy_signal();

	log_info("Gypsy: initialized");

	/* make sure framework don't close device when other users (say fso-gpsd) release GPS */
	dbus_request_resource("GPS");

	return TRUE;
}

void fso_gypsy_cleanup()
{
	/* If the users of "GPS" reaches to 0, framework will close device */
	dbus_release_resource("GPS");

	/* In case: (1) gypsy is stopped (2) fso frameworkd is stopped */
	if (! connecting)
		return;

	stopping = TRUE;

	dbus_g_proxy_disconnect_signal(device_proxy, CONNECTION_STATUS_CHANGED,
			G_CALLBACK(connection_status_changed), NULL);
	dbus_g_proxy_disconnect_signal(device_proxy, FIX_STATUS_CHANGED,
			G_CALLBACK(fix_status_changed), NULL);

	unregister_signals();

	reset_resources();
}

/**
 * NOTE: take care of stopping/restarting fso frameworkd.
 * Real world tests shows:
 * (1) sometimes restarting fso frameworkd freezes GPS.
 * (2) most of the time no effects -- as if it is not restarted at all.
 */
gboolean fso_gypsy_poll(gboolean pollsv)
{
	if (! connecting)
		return FALSE;

	return TRUE;
}

/**
 * It seems that dbus connection is auto re-connected when fso frameoworkd is restarted.
 */
static gboolean dbus_get_connection()
{
	GError *error = NULL;
	if (connection)
		return TRUE;

	connection = dbus_g_bus_get(DBUS_BUS_SYSTEM, &error);

	if (connection == NULL) {
		log_warn("Connect to dbus system failed.");
		if (error)
			g_error_free(error);
	}

	return (connection != NULL);
}

/**
 * for reporting "low power" event by sound
 */
static void power_status_changed(DBusGProxy *proxy, const gchar *status, gpointer user_data)
{
	if (! status)
		return;

	if (strcmp(status, "empty") == 0 || strcmp(status, "critical") == 0) {
		power_low = TRUE;
	} else {
		power_low = FALSE;
	}
}

gboolean dbus_power_is_low()
{
	return power_low;
}

/**
 * The name of the action. Expected values are:
 * - "suspend": The system is suspending.
 * - "resume": The system has resumed.
 * - "reboot": The system is rebooting.
 * - "shutdown": The system is shutting down.
 */
static void system_action(DBusGProxy *proxy, const gchar *action, gpointer user_data)
{
	if (! action)
		return;

	if (! POLL_ENGINE_TEST(UBX))
		return;

	if ((strcmp(action, "SUSPEND") == 0) || (strcmp(action, "suspend") == 0)) {
		if (POLL_STATE_TEST(RUNNING)) {
			notify_poll_thread_suspend_resume();
			do {
				sleep(1);
			} while (! POLL_STATE_TEST(SUSPENDING));
		}
	} else if ((strcmp(action, "RESUME") == 0) || (strcmp(action, "resume") == 0)) {
		notify_poll_thread_suspend_resume();
	}
}

static void dbus_connect_common_signals()
{
	/* power status */
	power_supply_proxy = dbus_g_proxy_new_for_name(connection, ODEVICED,
		DEVICE_PATH"/PowerSupply/battery", DEVICE".PowerSupply");
	dbus_g_proxy_add_signal(power_supply_proxy, "PowerStatus", G_TYPE_STRING, G_TYPE_INVALID);
	dbus_g_proxy_connect_signal(power_supply_proxy, "PowerStatus",
		G_CALLBACK(power_status_changed), NULL, NULL);

	if (! usage_proxy)
		usage_proxy = dbus_g_proxy_new_for_name(connection, OUSAGED, USAGE_PATH, USAGE);
	dbus_g_proxy_add_signal(usage_proxy, "SystemAction", G_TYPE_STRING, G_TYPE_INVALID);
	dbus_g_proxy_connect_signal(usage_proxy, "SystemAction", G_CALLBACK(system_action), NULL, NULL);
}

static void dbus_disconnect_common_signals()
{
	if (power_supply_proxy) {
		dbus_g_proxy_disconnect_signal(power_supply_proxy, "PowerStatus",
			G_CALLBACK(power_status_changed), NULL);
		g_object_unref(G_OBJECT(power_supply_proxy));
		power_supply_proxy = NULL;
	}

	if (usage_proxy) {
		dbus_g_proxy_disconnect_signal(usage_proxy, "SystemAction",	G_CALLBACK(system_action), NULL);
		g_object_unref(G_OBJECT(usage_proxy));
		usage_proxy = NULL;
	}
}

static gboolean dbus_toggle_resource(const char *name, gboolean enable)
{
	if (! dbus_get_connection())
		return FALSE;

	char *method = enable ? "RequestResource" : "ReleaseResource";

	GError *error = NULL;

	if (! usage_proxy)
		usage_proxy = dbus_g_proxy_new_for_name(connection, OUSAGED, USAGE_PATH, USAGE);

	gboolean ok = dbus_g_proxy_call_with_timeout(usage_proxy, method, PROXY_CALL_TIMEOUT, &error,
			G_TYPE_STRING, name, G_TYPE_INVALID, G_TYPE_INVALID);

	if (! ok) {
		log_warn("Request/Release resource: %s", error->message);
		if (error)
			g_error_free(error);
		return FALSE;
	}
	return TRUE;
}

/**
 * name: CPU, GPS, Display, etc.
 */
gboolean dbus_request_resource(const char *name)
{
	return dbus_toggle_resource(name, TRUE);
}

gboolean dbus_release_resource(const char *name)
{
	return dbus_toggle_resource(name, FALSE);
}

/**
 * Being called by init() or user actions. Thread safe.
 */
gboolean dbus_init()
{
	if (initialized)
		return TRUE;

	int i;
	for (i=0; i<10; i++) {
		if (dbus_get_connection())
			break;
		sleep(1);
	}
	if (! connection)
		return FALSE;

	dbus_connect_common_signals();

	initialized = TRUE;

	return TRUE;
}

void dbus_cleanup()
{
	if (connection) {
		dbus_disconnect_common_signals();
		//dbus_g_connection_unref(connection);
	}
}
