#include <pthread.h>
#include <signal.h>

#include "omgps.h"
#include "dbus_intf.h"
#include "gps.h"
#include "ubx.h"
#include "util.h"
#include "uart.h"
#include "customized.h"
#include "track.h"

static pthread_t polling_thread_tid = 0;
static gboolean running = TRUE;

static ctrl_cmd_func_t ctrl_cmd = NULL;
static void *ctrl_cmd_args = NULL;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

static gboolean pollsv = FALSE;
static gboolean ask_suspend = FALSE;

typedef void (*gps_poll_func_t)(void);
static gps_poll_func_t gps_poll_func = NULL;
static pthread_context_t *ctx = NULL;

static gps_data_t gdo;

static void poll_by_fso_ogpsd();
static void poll_by_ubx_binary();

static inline void on_new_record()
{
	if (g_gpsdata.latlon_valid && g_gpsdata.hacc <= TRACK_MAX_PACC)
		track_add(/*g_gpsdata.lat, g_gpsdata.lon, g_gpsdata.llh_itow*/);

	switch (g_tab_id) {
	case TAB_ID_MAIN_VIEW:
		if (! g_context.map_view_frozen)
			poll_update_ui();
		break;
	case TAB_ID_NAV_DATA:
		if (pollsv)
			update_nav_tab();
		break;
	default:
		;
	}
}

void init_gpsdata(gps_data_t *gd)
{
	memset(gd, 0, sizeof(gps_data_t));

	gd->latlon_valid = FALSE;
	gd->height_valid = FALSE;
	gd->vel_valid = FALSE;
	gd->svinfo_valid = FALSE;

	gd->llh_itow = 0;
	gd->lat = NAN;
	gd->lon = NAN;
	gd->height = NAN;
	gd->hacc = NAN;
	gd->vacc = NAN;

	gd->speed_2d = NAN;
	gd->heading_2d = NAN;
	gd->vel_down = NAN;

	gd->sv_in_use = 0;
	gd->sv_get_signal = 0;
	gd->sv_channel_count = 0;
}

static inline gboolean check_pollsv() {
	static int pollsv_counter = 0;

	if (pollsv_counter >= 4) {
		pollsv_counter = 0;
		return TRUE;
	} else {
		++pollsv_counter;
		return FALSE;
	}
}

/**
 * busybox does not support system().
 * The behavior of date command is strange on Ubuntu 8.10:
 * $ sudo date -u +'%m%d%H%M%Y.%S' -s `date -u +'%m%d%H%M%Y.%S'`
 * $ date: invalid date "..."
 *
 * `date` commands uses settimeofday and /dev/rtc.
 * Linux recommends adjtimex() and adjtime()...
 * Anyway, change system clock dramatically is a bad idea!
 *
 * return: -1, 0, 0x01 or (0x01 | 0x02)
 */
void sync_gpstime_to_system(struct tm *time)
{
	log_info("Sync GPS time...");

	char *date_cmds[] = {"/bin/date", "/sbin/date", "/usr/bin/date"};
	char *hwclock_cmds[] = {"/sbin/hwclock", "/bin/hwclock", "/usr/bin/hwclock"};

	char buf[16];
	snprintf(buf, sizeof(buf), "%02d%02d%02d%02d%04d.%02d",
		time->tm_mon + 1, time->tm_mday, time->tm_hour, time->tm_min, time->tm_year, time->tm_sec);

	char *date_args[] = {"date", "-u", "-s", buf, NULL};
	if (! exec_linux_cmd(date_cmds, sizeof(date_cmds)/sizeof(char*), date_args))
		return;

	g_context.time_synced = TRUE;

	log_info("Sync GPS time: synchronized to system.");

	char *hwclock_args[] = {"hwclock", "-w", NULL};
	if (exec_linux_cmd(hwclock_cmds, sizeof(hwclock_cmds)/sizeof(char*), hwclock_args))
		log_info("Sync GPS time: synchronized to hardware clock.");

	ubx_msg_type_t types[] = {
		{UBX_CLASS_NAV, UBX_ID_NAV_STATUS},
		{UBX_CLASS_NAV, UBX_ID_NAV_POSLLH},
		{UBX_CLASS_NAV, UBX_ID_NAV_VELNED},
		{UBX_CLASS_NAV, UBX_ID_NAV_SVINFO}
	};
	ubx_set_poll_msg_types(types, sizeof(types)/sizeof(ubx_msg_type_t));
}

/**
 * This is called from GTK UI main thread.
 * NOTE: must be thread safe!
 */
gboolean issue_ctrl_cmd(ctrl_cmd_func_t cmd, void *args)
{
	assert(POLL_ENGINE_TEST(UBX));

	if (ctrl_cmd)
		return FALSE;

	ctrl_cmd_args = args;
	ctrl_cmd = cmd; /* must be set after <ctrl_cmd_args> */

	return TRUE;
}

static inline gboolean exec_ctrl_cmd()
{
	gboolean ok = (*ctrl_cmd) (ctrl_cmd_args);
	uart_flush_output();
	ctrl_cmd = NULL;
	ctrl_cmd_args = NULL;
	return ok;
}

void notify_poll_thread_suspend_resume()
{
	LOCK_MUTEX(&lock);
	if (POLL_STATE_TEST(RUNNING))
		ask_suspend = TRUE;
	pthread_cond_signal(&cond);
	UNLOCK_MUTEX(&lock);
}

static gboolean init_ogpsd_service()
{
	gboolean ok = FALSE;
	LOCK_UI();
	status_label_set_text("Connect FSO GPS dbus service...", FALSE);
	UNLOCK_UI();

	ok = fso_gypsy_init(&gdo, &lock);

	LOCK_UI();
	status_label_set_text(ok? "Connect FSO GPS dbus service: ok" :
		"Connect FSO GPS dbus service: failed", FALSE);
	UNLOCK_UI();
	return ok;
}

static void set_poll_engine(poll_engine_t e)
{
	g_context.poll_engine = e;

	switch(e) {
	case POLL_ENGINE_OGPSD:
		gps_poll_func = poll_by_fso_ogpsd;
		break;
	case POLL_ENGINE_UBX:
		gps_poll_func = poll_by_ubx_binary;
		break;
	default:
		return;
	}

	LOCK_UI();
	ctx_gpsfix_on_poll_engine_changed();
	UNLOCK_UI();
}

/* check: poll_suspending/restore polling thread, take care of OS suspending */
static void suspend_resume(gboolean skip_suspend)
{
	if (skip_suspend)
		goto RUN;

	/* Cleanup GPS */
	if (POLL_ENGINE_TEST(OGPSD))
		fso_gypsy_cleanup();

	/* to avoid long time gap */
	if (g_context.track_enabled) {
		LOCK_UI();
		track_save(TRUE, FALSE);
		UNLOCK_UI();
	}

	if (g_gpsdata.latlon_valid)
		settings_save();

SUSPEND:

	/* suspend: user action or due to failure */

	LOCK_UI();

	POLL_STATE_SET(SUSPENDING);
	g_gpsdata.latlon_valid = FALSE;
	g_gpsdata.vel_valid = FALSE;
	g_gpsdata.svinfo_valid = FALSE;
	ctx_gpsfix_on_poll_state_changed();
	update_tab_on_poll_state_changed();

	UNLOCK_UI();

	LOCK_MUTEX(&lock);
	pthread_cond_wait(&cond, &lock);
	UNLOCK_MUTEX(&lock);

RUN:

	LOCK_UI();
	POLL_STATE_SET(STARTING);
	ctx_gpsfix_on_poll_state_changed();
	UNLOCK_UI();

	ask_suspend = FALSE;

	if (! POLL_ENGINE_TEST(OGPSD) && (fso_gypsy_is_running())) {
		if (! init_ogpsd_service()) {
			/* GPS resource is released by the user we just saw */
			if (! fso_gypsy_is_running())
				goto UBX;
			LOCK_UI();
			status_label_set_text("Can't set data provider as FSO ogpsd", FALSE);
			UNLOCK_UI();
			goto SUSPEND;
		} else {
			set_poll_engine(POLL_ENGINE_OGPSD);
		}
	} else {
UBX:
		if (! gps_device_power_on()) {
			LOCK_UI();
			warn_dialog("Connect to GPS failed");
			UNLOCK_UI();
			goto SUSPEND;
		} else {
			set_poll_engine(POLL_ENGINE_UBX);
		}
	}

	/* Only after device being initialized! */
	POLL_STATE_SET(RUNNING);

	LOCK_UI();
	ctx_gpsfix_on_poll_state_changed();
	update_tab_on_poll_state_changed();
	UNLOCK_UI();

	g_context.cursor_in_view = TRUE;
}

/**
 * hacc and vacc: rough value, see:
 * http://users.erols.com/dlwilson/gpsacc.htm
 * http://en.wikipedia.org/wiki/Dilution_of_precision_(GPS)
 */
static void poll_by_fso_ogpsd()
{
	pollsv = check_pollsv();

	if (! fso_gypsy_poll(pollsv))
		goto END;

	/* lock UI lock */
	LOCK_UI();

	/* d-bus events are asynchronous, need lock */
	LOCK_MUTEX(&lock);

	if (! isnan(gdo.lat) &&
		! isnan(gdo.lon) &&
		! isnan(gdo.hacc)) {

		g_gpsdata.lat = gdo.lat;
		g_gpsdata.lon = gdo.lon;
		g_gpsdata.hacc = gdo.hacc * DOP_TO_HACC;

		g_gpsdata.latlon_valid = (g_gpsdata.nav_status_fixtype >= 0x02 && g_gpsdata.sv_in_use >= 3);
	} else {
		g_gpsdata.latlon_valid = FALSE;
	}

	if (! isnan(gdo.speed_2d) &&
		! isnan(gdo.heading_2d) &&
		! isnan(gdo.vel_down)) {

		g_gpsdata.speed_2d = gdo.speed_2d;
		g_gpsdata.heading_2d = gdo.heading_2d;
		g_gpsdata.vel_down = gdo.vel_down;

		g_gpsdata.vel_valid = TRUE;
	} else {
		g_gpsdata.vel_valid = FALSE;
	}

	if (! isnan(gdo.vacc) &&
		! isnan(gdo.height)) {

		g_gpsdata.height = gdo.height;
		g_gpsdata.vacc = gdo.vacc * DOP_TO_HACC;

		g_gpsdata.height_valid = TRUE;
	} else {
		g_gpsdata.height_valid = FALSE;
	}

	g_gpsdata.svinfo_valid = gdo.svinfo_valid;
	if (g_gpsdata.svinfo_valid) {
		g_gpsdata.sv_in_use = gdo.sv_in_use;
		g_gpsdata.sv_get_signal = gdo.sv_get_signal;
		g_gpsdata.sv_channel_count = gdo.sv_channel_count;
		memcpy(g_gpsdata.sv_channels, gdo.sv_channels, sizeof(gdo.sv_channels));
	}

	g_gpsdata.nav_status_fixtype = gdo.nav_status_fixtype;

	UNLOCK_MUTEX(&lock);

	on_new_record();

	/* release UI lock */
	UNLOCK_UI();

END:

	/* sleep: same regardless asynchronous or not */
	sleep_ms(SEND_RATE);
}

static void switch_to_ogpsd()
{
	/* hack! */
	ubx_cfg_rate((U2)SEND_RATE, FALSE);

	uart_close();

	if (init_ogpsd_service()) {
		g_context.uart_conflict = FALSE;
		set_poll_engine(POLL_ENGINE_OGPSD);

		LOCK_UI();
		menu_tab_on_show();
		status_label_set_text("UART conflict, switched to ogpsd.", FALSE);
		UNLOCK_UI();
	} else {
		LOCK_UI();
		status_label_set_text("UART conflict, can't switch to ogpsd", FALSE);
		UNLOCK_UI();
		ask_suspend = TRUE;
	}
}

static void check_failure()
{
	if (sysfs_get_gps_device_power() != 1) {
		if (! gps_device_power_on()) {
			LOCK_UI();
			warn_dialog("Connect to GPS failed");
			UNLOCK_UI();
			ask_suspend = TRUE;
		}
	} else {
		if (fso_gypsy_is_running()) {
			g_context.uart_conflict = TRUE;
		}
	}
}

static void poll_by_ubx_binary()
{
	gboolean error = FALSE;

	pollsv = check_pollsv();

	if (ubx_poll_group(pollsv)) {
		LOCK_UI();
		if (g_gpsdata.nav_status_fixtype < 0x03)
			g_gpsdata.height_valid = FALSE;
		on_new_record();
		UNLOCK_UI();
	} else {
		error = TRUE;
		goto END;
	}

	/* check and execute control commands */
	if (ctrl_cmd != NULL && ! exec_ctrl_cmd())
		goto END;

END:

	if (error) {
		check_failure();
		if (g_context.uart_conflict)
			switch_to_ogpsd();
	}
}

/**
 * NOTE: test with increase order: (CLASS, ID)
 */
static void * poll_gps_data_routine(void *args)
{
	sigset_t sig_set;
	sigemptyset(&sig_set);
	sigaddset(&sig_set, SIGINT);
	pthread_sigmask(SIG_BLOCK, &sig_set, NULL);

	if (ctx) {
		free(ctx);
		ctx = NULL;
	}

	ctx = register_thread("poll thread", NULL, NULL);

	ubx_msg_type_t types[] = {
		{UBX_CLASS_NAV, UBX_ID_NAV_STATUS},
		{UBX_CLASS_NAV, UBX_ID_NAV_POSLLH},
		{UBX_CLASS_NAV, UBX_ID_NAV_VELNED},
		{UBX_CLASS_NAV, UBX_ID_NAV_TIMEUTC},
		{UBX_CLASS_NAV, UBX_ID_NAV_SVINFO}
	};
	ubx_set_poll_msg_types(types, sizeof(types)/sizeof(ubx_msg_type_t));

	g_context.time_synced = FALSE;
	g_context.uart_conflict = FALSE;
	running = TRUE;

	ctrl_cmd = NULL;
	ctrl_cmd_args = NULL;
	pollsv = FALSE;
	ask_suspend = FALSE;

	gboolean start_now = TRUE;
	if (! g_context.run_gps_on_start) {
		LOCK_UI();
		start_now = confirm_dialog("start GPS now?");
		UNLOCK_UI();
	}

	suspend_resume(start_now);

	while (running) {
		if (ask_suspend) {
			suspend_resume(FALSE);
			if (! running)
				break;
		}

		(*gps_poll_func)();

	} /* while loop*/

	if (ctx)
		free(ctx);

	return NULL;
}

gboolean start_poll_thread()
{
	return (pthread_create(&polling_thread_tid, NULL, poll_gps_data_routine, NULL) == 0);
}

void stop_poll_thread()
{
	running = FALSE;

	if (polling_thread_tid != 0) {
		LOCK_MUTEX(&lock);
		pthread_cond_signal(&cond);
		UNLOCK_MUTEX(&lock);
		sleep_ms(200);

		pthread_kill(polling_thread_tid, SIGUSR1);
		pthread_join(polling_thread_tid, NULL);
		polling_thread_tid = 0;
	}

	if (POLL_STATE_TEST(RUNNING)) {
		if (POLL_ENGINE_TEST(OGPSD)) {
			fso_gypsy_cleanup();
		} else {
			if (g_gpsdata.sv_in_use >= 3) {
				PangoLayout *layout = gtk_widget_create_pango_layout (g_view.da, "Dumping AID data...");
				PangoFontDescription *desc = pango_font_description_from_string (
#if (PLATFORM_FSO)
			"Sans Bold 6"
#else
			"Sans Bold 18"
#endif
				);
				pango_layout_set_font_description (layout, desc);

				int text_width, text_height;
				pango_layout_get_size (layout, &text_width, &text_height);
				text_width /= PANGO_SCALE;

				gdk_draw_layout(g_view.da->window, g_view.da->style->black_gc,
					(g_view.width - text_width) >> 1, g_view.height >> 1, layout);

				pango_font_description_free (desc);
				g_object_unref (layout);
				/* Aha!*/
				gdk_flush();

				agps_dump_aid_data(FALSE);
			}
			uart_cleanup();
		}
		POLL_STATE_SET(SUSPENDING);
	}

	log_info("poll thread was stopped");
}
