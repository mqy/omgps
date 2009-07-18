#include <pthread.h>
#include <signal.h>

#include "omgps.h"
#include "xpm_image.h"
#include "util.h"
#include "track.h"
#include "customized.h"

static char *replay_file_path = NULL;

static GtkWidget *start_button, *suspend_button, *speed_button, *direction_button, *toend_button;
static GtkAdjustment *adjustment;
static GtkWidget *progress_bar, *title_label, *desc_label;

static GdkPixbuf *replay_flag;
#define DEFAULT_SPEED	10
static int speed;

static trackpoint_t *replay_records = NULL;

static U4 replay_start_time, time_span;
static int total_records = 0, last_draw_idx = -1;

static pthread_t replay_thread_tid = 0;
static pthread_mutex_t replay_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t replay_cond = PTHREAD_COND_INITIALIZER;
static gboolean running = FALSE;
static gboolean stop = FALSE;
static gboolean suspend = FALSE;
static gboolean suspending = FALSE;
static gboolean toend = FALSE;

static point_t last_point;
static GdkPixbuf *replay_flag = NULL;

static int replay_flag_w, replay_flag_h;
static point_t replay_flag_pt;
static GdkRectangle flag_last_rect;
static gboolean last_rect_valid = FALSE;

static point_t last_tl_pixel = {0, 0};
static int last_zoom = -1;
static map_repo_t *last_repo = NULL;
static gboolean direction_forward = TRUE;

#define RESET_LAST_POINT() { last_point.x = last_point.y = -100; }
#define WARKUP_REPLAY_THREAD() do {		\
	LOCK_MUTEX(&replay_lock);			\
	pthread_cond_signal(&replay_cond);	\
	UNLOCK_MUTEX(&replay_lock);			\
} while (0)

static void replay_free_data()
{
	if (replay_records) {
		free(replay_records);
		replay_records = NULL;
	}
	replay_file_path = NULL;

	/* keep it to make the row selected */
	//replay_file = NULL;
	replay_thread_tid = 0;
	last_draw_idx = -1;
	RESET_LAST_POINT();
}

static gboolean replay_read_records()
{
	gboolean ret = TRUE;

	FILE *fp = fopen(replay_file_path, "r");
	if (! fp) {
		warn_dialog("Unable to open track file");
		return FALSE;
	}

	/* head */
	U4 end_time, record_count;
	fscanf(fp, TRACK_HEAD_LABEL_1"%u\n", &replay_start_time);
	fscanf(fp, TRACK_HEAD_LABEL_2"%u\n", &end_time);
	fscanf(fp, TRACK_HEAD_LABEL_3"%u\n", &record_count);

	/* auto split at each GPS week start, see track_add() */
	if (record_count > 3600 * 24 * 7) {
		if (! confirm_dialog("Too many records, "
				"risk of out of memory\n\ncontinue?")) {
			ret = FALSE;
			goto END;
		}
	}

	replay_records = (trackpoint_t*)calloc(record_count, sizeof(trackpoint_t));
	if (! replay_records) {
		warn_dialog("Track replay: failed to allocate memory");
		ret = FALSE;
		goto END;
	}

	int i, n;

	for (i=0; i < record_count; i++) {
		n = fscanf(fp, "%lf\t%lf\t%u\n",
			&replay_records[i].wgs84.lat, &replay_records[i].wgs84.lon, &replay_records[i].time_offset);
		if (n == EOF)
			break;
		else if (n != 3) {
			warn_dialog("Read track file failed");
			replay_free_data();
			ret = FALSE;
			goto END;
		}
	}

	if (i == 0) {
		ret = FALSE;
		goto END;
	}

	total_records = i;

	time_span = replay_records[total_records - 1].time_offset;

END:

	fclose(fp);

	return ret;
}

/**
 * calculate tile pixels for all CURRENT map and zoom level.
 */
static void replay_calculate_pixel()
{
	assert(replay_records);

	gboolean same_repo_zoom = (g_view.fglayer.repo == last_repo &&
		g_view.fglayer.repo->zoom == last_zoom);

	int deltax = 0;
	int deltay = 0;

	if (last_repo != NULL) {
		deltax = g_view.fglayer.tl_pixel.x - last_tl_pixel.x;
		deltay = g_view.fglayer.tl_pixel.y - last_tl_pixel.y;
	}
	/* skip if (1) same map, (2) same zoom level, (3) same view */
	if (same_repo_zoom && deltax == 0 && deltay == 0)
		return;

	last_repo = g_view.fglayer.repo;
	last_zoom = last_repo->zoom;

	trackpoint_t *rec;
	int i;

	for (i=0; i<total_records; i++) {
		rec = &(replay_records[i]);
		if (! same_repo_zoom)
			rec->pixel = wgs84_to_tilepixel(rec->wgs84, last_zoom, last_repo);

		rec->inview = POINT_IN_RANGE(rec->pixel, g_view.fglayer.tl_pixel, g_view.fglayer.br_pixel);
	}

	last_tl_pixel = g_view.fglayer.tl_pixel;
}

static void replay_reset_states()
{
	suspend = FALSE;

	gtk_button_set_label(GTK_BUTTON(start_button), "start");
	gtk_button_set_label(GTK_BUTTON(suspend_button), "suspend");

	gtk_widget_set_sensitive(start_button, TRUE);
	gtk_widget_set_sensitive(suspend_button, FALSE);
	gtk_widget_set_sensitive(toend_button, FALSE);
	gtk_widget_set_sensitive(speed_button, TRUE);
	gtk_widget_set_sensitive(direction_button, TRUE);
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0);
	gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), "");

	RESET_LAST_POINT();
}

static void replay_draw_flag(point_t point)
{
	last_point = point;

	replay_flag_pt.x = last_point.x - (replay_flag_w >> 1);
	replay_flag_pt.y = last_point.y - (replay_flag_h >> 1);

	GdkRectangle flag_rect = {replay_flag_pt.x, replay_flag_pt.y, replay_flag_w, replay_flag_h};

	last_rect_valid = gdk_rectangle_intersect(&g_view.fglayer.visible, &flag_rect, &flag_last_rect);

	gdk_draw_pixbuf (g_view.da->window, g_context.track_gc, replay_flag,
		0, 0, flag_rect.x, flag_rect.y, flag_rect.width, flag_rect.height,
		GDK_RGB_DITHER_NONE, -1, -1);
}

/**
 * Draw replay records from start(?) up to some point.
 * Order is important!
 */
static void replay_draw_lines()
{
	int i, start_idx, end_idx, inc;

	if (replay_records == NULL || last_draw_idx == -1)
		return;

	map_draw_back_layers(g_view.pixmap);

	if (direction_forward) {
		start_idx = 0;
		end_idx = last_draw_idx;
		inc = 1;
	} else {
		start_idx = total_records - 1;
		end_idx = last_draw_idx;
		inc = -1;
	}

	trackpoint_t *rec, *last_rec = NULL;

	for (i=start_idx; direction_forward? i<=end_idx : i>=end_idx; i+=inc) {
		rec = &(replay_records[i]);
		point_t p = {(rec->pixel.x - g_view.fglayer.tl_pixel.x),
				(rec->pixel.y - g_view.fglayer.tl_pixel.y)};

		if (last_rec) {
			if (rec->inview || last_rec->inview) {
				gdk_draw_line(g_view.pixmap, g_context.track_gc,
					last_point.x, last_point.y, p.x, p.y);
			}
		}

		last_point = p;
		last_rec = rec;
	}

	gdk_draw_drawable (g_view.da->window, g_context.track_gc, g_view.pixmap,
		g_view.fglayer.visible.x, g_view.fglayer.visible.y,
		g_view.fglayer.visible.x, g_view.fglayer.visible.y,
		g_view.fglayer.visible.width, g_view.fglayer.visible.height);
}

static void draw_current_position()
{
	if (! g_gpsdata.latlon_valid)
		return;

	coord_t wgs84 = {g_gpsdata.lat, g_gpsdata.lon};

	point_t pos_pixel = wgs84_to_tilepixel(wgs84, last_zoom, last_repo);
	int x = pos_pixel.x - g_view.fglayer.tl_pixel.x;
	int y = pos_pixel.y - g_view.fglayer.tl_pixel.y;

	#define XPM_SIZE		32
	#define XPM_SIZE_HALF	16

	gdk_draw_pixbuf (g_view.da->window, g_context.track_gc, g_xpm_images[XPM_ID_POSITION_VALID].pixbuf,
		0, 0, x - XPM_SIZE_HALF, y - XPM_SIZE_HALF, XPM_SIZE, XPM_SIZE,
		GDK_RGB_DITHER_NONE, -1, -1);
}

/**
 * track replay, also being called by map_invalidate_view()
 */
static void replay_redraw_view()
{
	map_draw_back_layers(g_view.da->window);

	replay_calculate_pixel();

	replay_draw_lines();

	replay_draw_flag(last_point);

	/* draw current position */
	draw_current_position();
}

static void replay_update_ui_helper(gboolean prepare)
{
	static int zoom;
	static coord_t center_wgs84;

	if (prepare) {
		g_context.cursor_in_view = TRUE;
		g_context.map_view_frozen = TRUE;
		zoom = g_view.fglayer.repo->zoom;
		center_wgs84 = g_view.center_wgs84;

		map_set_redraw_func(&replay_redraw_view);
	} else {
		g_context.cursor_in_view = TRUE;
		g_context.map_view_frozen = FALSE;

		map_set_redraw_func(NULL);
		map_zoom_to(zoom, center_wgs84, FALSE);
	}
}

void track_replay_centralize()
{
	if (last_draw_idx >= 0) {
		g_view.fglayer.center_pixel = replay_records[last_draw_idx].pixel;
		g_view.center_wgs84 = tilepixel_to_wgs84(g_view.fglayer.center_pixel,
			g_view.fglayer.repo->zoom, g_view.fglayer.repo);
		/* also trigger update all pixels */
		map_invalidate_view(TRUE);
	}
}

static void replay_update_progress_bar(int time_offset)
{
	float fraction = (float)time_offset / time_span;
	if (fraction > 1.0) {
		log_debug("fraction=%f", fraction);
		fraction = 1.0;
	}
	if (! direction_forward)
		fraction = 1.0 - fraction;
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), fraction);

	char buf[32];
	time_t tm = replay_start_time + time_offset;
	struct tm *t = localtime(&tm);
	int len = sprintf(buf, "%2d%%   ", (int)(fraction * 100));
	if (len > 0) {
		strftime(&buf[len], sizeof(buf) - len, "[%b %d, %H:%M:%S]",  t);
		gtk_label_set_text(GTK_LABEL(desc_label), buf);
	}
}

static void replay_update_ui (int from, int to)
{
	trackpoint_t *record = NULL;
	point_t p;
	int i;

	/* restore area covered by flag */
	if (last_rect_valid) {
		gdk_draw_drawable (g_view.da->window, g_context.track_gc, g_view.pixmap,
			flag_last_rect.x, flag_last_rect.y,
			flag_last_rect.x, flag_last_rect.y, flag_last_rect.width, flag_last_rect.height);
	}

	int inc = (direction_forward)? 1 : -1;

	for (i=from; direction_forward? i<=to : i>=to; i+=inc) {
		record = &replay_records[i];
		p.x = record->pixel.x - g_view.fglayer.tl_pixel.x;
		p.y = record->pixel.y - g_view.fglayer.tl_pixel.y;

		if (last_point.x >= 0) {
			gdk_draw_line(g_view.da->window, g_context.track_gc,
				last_point.x, last_point.y, p.x, p.y);
			gdk_draw_line(g_view.pixmap, g_context.track_gc,
				last_point.x, last_point.y, p.x, p.y);
		}
		last_point = p;
		last_draw_idx = i;
	}

	if (record->inview) {
		replay_draw_flag(last_point);
	} else {
		if (g_context.cursor_in_view) {
			RESET_LAST_POINT();
			track_replay_centralize();
		}
	}

	replay_update_progress_bar(record->time_offset);
}

/* make sure the UI lock is released when the thread is killed with SIGUSR1 */
static void replay_thread_cleanup_func(struct __pthread_context_t *ctx)
{
	UNLOCK_UI();
}

static void *replay_routine()
{
	if (total_records == 0)
		return NULL;

	sigset_t sig_set;
	sigemptyset(&sig_set);
	sigaddset(&sig_set, SIGINT);
	pthread_sigmask(SIG_BLOCK, &sig_set, NULL);

	pthread_context_t *ctx = register_thread("track replay thread",
		NULL, replay_thread_cleanup_func);

	running = TRUE;

	while(running) {

		stop = FALSE;
		suspend = FALSE;
		suspending = FALSE;
		last_draw_idx = -1;

		/* settings */
		LOCK_UI();

		replay_update_ui_helper(TRUE);

		map_draw_back_layers(g_view.da->window);

		RESET_LAST_POINT();

		/* the record index in the containing block */
		int last_time_offset = 0;
		int span, orient, start_idx, end_idx, inc;

		if (direction_forward) {
			start_idx = 0;
			end_idx = total_records - 1;
			inc = 1;
			orient = GTK_PROGRESS_LEFT_TO_RIGHT;
		} else {
			start_idx = total_records - 1;
			end_idx = 0;
			inc = -1;
			orient = GTK_PROGRESS_RIGHT_TO_LEFT;
		}

		gtk_progress_bar_set_orientation(GTK_PROGRESS_BAR(progress_bar), orient);
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0);

		gtk_button_set_label(GTK_BUTTON(start_button), "stop");
		gtk_widget_set_sensitive(start_button, TRUE);
		gtk_widget_set_sensitive(suspend_button, TRUE);
		gtk_widget_set_sensitive(toend_button, TRUE);
		gtk_widget_set_sensitive(direction_button, FALSE);

		speed = gtk_adjustment_get_value(adjustment);

		/* calculate pixels in tile map */
		replay_calculate_pixel();

		UNLOCK_UI();

		int i, from = start_idx, to;

		/* FIXME: better to use sigjmp here */
		for (i=start_idx; direction_forward? i<=end_idx : i>=end_idx; i+=inc) {

			if (! running) {
				goto EXIT;
			} else if (stop) {
				stop = FALSE;
				goto STOP;
			} else if (toend) {
				toend = FALSE;
				i = end_idx;
			} else if (suspend) {
				suspend = FALSE;
				suspending = TRUE;
				wait_ms(0, &replay_cond, &replay_lock, TRUE);
				suspending = FALSE;
				LOCK_UI();
				gtk_button_set_label(GTK_BUTTON(suspend_button), "suspend");
				UNLOCK_UI();
			}

			span = abs(replay_records[i].time_offset - last_time_offset) / speed;
			if (span < 1 && i != end_idx)
				continue;
			if (span > 1)
				span = 1; // don't wait too long

			to = i;
			LOCK_UI();
			replay_update_ui(from, to);
			UNLOCK_UI();
			from = to + inc;

			sleep(1);

			last_time_offset = replay_records[i].time_offset;
		}

		last_draw_idx = end_idx;

		/* draw current position */
		draw_current_position();

STOP:

		LOCK_UI();
		replay_reset_states();
		UNLOCK_UI();

		/* wait for restart */
		wait_ms(0, &replay_cond, &replay_lock, TRUE);
	}

EXIT:

	free(ctx);

	return NULL;
}

static void cancel_replay_thread()
{
	if (replay_thread_tid > 0) {
		running = FALSE;

		WARKUP_REPLAY_THREAD();

		sleep_ms(200);
		pthread_kill(replay_thread_tid, SIGUSR1);
		pthread_join(replay_thread_tid, NULL);
		replay_thread_tid = 0;
	}
}

static void start_button_clicked(GtkWidget *widget, gpointer data)
{
	if (running) {
		stop = TRUE;
		last_rect_valid = FALSE;
		WARKUP_REPLAY_THREAD();
	} else {
		if (replay_thread_tid == 0) {
			if (pthread_create(&replay_thread_tid, NULL, replay_routine, NULL) != 0) {
				warn_dialog("can not create replay thread");
				replay_reset_states();
			}
		} else {
			last_rect_valid = FALSE;
			WARKUP_REPLAY_THREAD();
		}
	}
}

static void suspend_button_clicked(GtkWidget *widget, gpointer data)
{
	if (suspending) {
		suspend = FALSE;
	} else {
		suspend = TRUE;
		gtk_button_set_label(GTK_BUTTON(suspend_button), "resume");
	}

	WARKUP_REPLAY_THREAD();
}

static void toend_button_clicked(GtkWidget *widget, gpointer data)
{
	/* FIXME: advance reply go ahead a distance? */
	if (running) {
		WARKUP_REPLAY_THREAD();
		toend = TRUE;
	}
}

static void direction_button_toggled(GtkWidget *widget, gpointer data)
{
	direction_forward = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(direction_button));
}

static void speed_changed(GtkWidget *widget, gpointer data)
{
	speed = gtk_adjustment_get_value(adjustment);
	WARKUP_REPLAY_THREAD();
}

static void close_button_clicked(GtkWidget *widget, gpointer data)
{
	if (replay_thread_tid > 0) {
		cancel_replay_thread();
		replay_update_ui_helper(FALSE);
	}

	replay_free_data();

	drawingarea_reset_default_mouse_handler();
	switch_to_main_view(CTX_ID_GPS_FIX);
}

void track_replay_cleanup()
{
	if (replay_records != NULL) {
		cancel_replay_thread();
		replay_free_data();
	}
}

void ctx_tab_track_replay_on_show()
{
	replay_file_path = get_cur_replay_filepath();
	if (replay_file_path == NULL)
		return;

	speed = DEFAULT_SPEED;

	if (! replay_read_records()) {
		gtk_widget_set_sensitive(start_button, FALSE);
		gtk_widget_set_sensitive(suspend_button, FALSE);
		gtk_widget_set_sensitive(toend_button, FALSE);
	} else {
		replay_reset_states();

		last_tl_pixel.x = last_tl_pixel.y = 0;
		last_zoom = -1;
		last_repo = NULL;

		char buf[128];
		time_t tm = replay_start_time;
		struct tm *t = localtime(&tm);
		int len = strftime(buf, sizeof(buf),
			"<span weight='bold'>Track replay</span>: (%b %d, %H:%M:%S", t);

		tm += time_span;
		t = localtime(&tm);
		strftime(&buf[len], sizeof(buf), " - %b %d, %H:%M:%S)", t);

		gtk_label_set_markup(GTK_LABEL(title_label), buf);
		gtk_label_set_text(GTK_LABEL(desc_label), "");
	}
}

GtkWidget* ctx_tab_track_replay_create()
{
	replay_flag = g_xpm_images[XPM_ID_TRACK_REPLAY_FLAG].pixbuf;
	replay_flag_w = g_xpm_images[XPM_ID_TRACK_REPLAY_FLAG].width;
	replay_flag_h = g_xpm_images[XPM_ID_TRACK_REPLAY_FLAG].height;

	GtkWidget *vbox = gtk_vbox_new(FALSE, 0);

	title_label = gtk_label_new("");

	GtkWidget *sep = gtk_hseparator_new();

	gtk_container_add(GTK_CONTAINER (vbox), title_label);
	gtk_container_add(GTK_CONTAINER (vbox), sep);

	GtkWidget *hbox_0 = gtk_hbox_new(FALSE, 0);
	gtk_container_add(GTK_CONTAINER (vbox), hbox_0);

	/* progress bar, due to it's odd behavior, don't display percent and time on it */
	progress_bar = gtk_progress_bar_new();
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0);
	gtk_box_pack_start(GTK_BOX (hbox_0), progress_bar, TRUE, TRUE, 0);

	/* current percent and time */
	desc_label = gtk_label_new("");

	gtk_box_pack_start(GTK_BOX (hbox_0), desc_label, FALSE, FALSE, 0);

	sep = gtk_hseparator_new();
	gtk_container_add(GTK_CONTAINER (vbox), sep);

	/* configuration */

	GtkWidget *hbox_1 = gtk_hbox_new(FALSE, 3);
	gtk_container_add (GTK_CONTAINER (vbox), hbox_1);

	GtkWidget *speed_label = gtk_label_new("speed:");
	adjustment = (GtkAdjustment*)gtk_adjustment_new(DEFAULT_SPEED, 1, 60, 6, 6, 0);

	speed_button = gtk_hscale_new(adjustment);
	gtk_scale_set_draw_value(GTK_SCALE(speed_button), TRUE);
	gtk_scale_set_value_pos(GTK_SCALE(speed_button), GTK_POS_RIGHT);
	gtk_scale_set_digits(GTK_SCALE(speed_button), 0);
	g_signal_connect (G_OBJECT (adjustment), "value-changed",
		G_CALLBACK (speed_changed), NULL);

	gtk_box_pack_start (GTK_BOX (hbox_1), speed_label, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (hbox_1), speed_button, TRUE, TRUE, 0);

	direction_button = gtk_check_button_new_with_label("forward");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(direction_button), TRUE);
	g_signal_connect (G_OBJECT(direction_button), "toggled",
		G_CALLBACK (direction_button_toggled), NULL);
	gtk_box_pack_start (GTK_BOX (hbox_1), direction_button, FALSE, FALSE, 0);

	/* function buttons */

	GtkWidget *hbox_2 = gtk_hbox_new(TRUE, 5);
	gtk_container_add (GTK_CONTAINER (vbox), hbox_2);

	start_button = gtk_button_new_with_label("start");
	g_signal_connect (G_OBJECT (start_button), "clicked",
			G_CALLBACK (start_button_clicked), NULL);

	suspend_button = gtk_button_new_with_label("suspend");
	g_signal_connect (G_OBJECT (suspend_button), "clicked",
		G_CALLBACK (suspend_button_clicked), NULL);

	toend_button = gtk_button_new_with_label(">>|");
	g_signal_connect (G_OBJECT (toend_button), "clicked",
		G_CALLBACK (toend_button_clicked), NULL);

	GtkWidget *close_button = gtk_button_new_with_label("close");
	g_signal_connect (G_OBJECT (close_button), "clicked",
			G_CALLBACK (close_button_clicked), NULL);

	gtk_container_add (GTK_CONTAINER (hbox_2), start_button);
	gtk_container_add (GTK_CONTAINER (hbox_2), suspend_button);
	gtk_container_add (GTK_CONTAINER (hbox_2), toend_button);
	gtk_container_add (GTK_CONTAINER (hbox_2), close_button);

	return vbox;
}
