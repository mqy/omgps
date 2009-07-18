#include "omgps.h"
#include "xpm_image.h"
#include "sound.h"
#include "track.h"

#define NAV_H_NUM		7
#define NAV_SPD_NUM		7
#define NAV_SV_NUM		5

/* normal image size */
#define FONT_WIDTH		18
#define FONT_HEIGHT		36

#define HEADING_WIDTH	40
#define HEADING_HEIGHT	40

#define BOTTOM			38

static GtkWidget *nav_da, *heading_da;
static GtkWidget *poll_engine_image, *track_image;

#define INVALID_HASH	0xFFFFFFFF

#define OUTER_WIDTH		4
#define INTER_WIDTH		2
/*(360 * 64)*/
#define POS_ARC			23040
/* position image: xpm image size */
#define XPM_SIZE		32
#define XPM_SIZE_HALF	16

static int x = 0, y = 0, r = 0;
static GdkRectangle last_rect;
static gboolean last_rect_valid = FALSE;

static int last_heading = -1;
static int lastx = 0, lasty = 0;
static char speed_unit_sign;

static point_t cursor_range_tl, cursor_range_br;

typedef struct __nav_da_data_t
{
	char text[8];
	int text_count;
	int offset;
	int width;
	U4 hash;
	U4 last_hash;
} nav_da_data_t;

static nav_da_data_t nav_da_data[3];

#define RESET_HEADING() \
	lastx = lasty = 0;	\
	last_heading = -1;	\

/**
 * Heading == -1 means just clear previous
 */
static void draw_speed_heading(int heading)
{
	#define DEG_TO_RAD 		(M_PI / 180)
	#define HEADING_W_HALF	(HEADING_WIDTH >> 1)
	#define HEADING_H_HALF	(HEADING_HEIGHT >> 1)
	#define HEADING_R 		(MIN(HEADING_W_HALF, HEADING_H_HALF) - 6)

	if (heading == last_heading)
		return;

	if (lastx > 0) {
		/* clear previous */
		gdk_draw_line(heading_da->window, g_context.heading_gc,
			HEADING_W_HALF, HEADING_H_HALF, lastx, lasty);
		RESET_HEADING();
	}

	/* degree 0 points to true north. headings respect to north, clockwise */
	if (heading >= 0) {
		float rad = heading * DEG_TO_RAD;
		lastx = (int)(HEADING_W_HALF + HEADING_R * sin(rad));
		lasty = (int)(HEADING_H_HALF - HEADING_R * cos(rad));
		gdk_draw_line(heading_da->window, g_context.heading_gc,
			HEADING_W_HALF, HEADING_H_HALF, lastx, lasty);

		last_heading = heading;
	}
}

static gboolean heading_da_expose_event (GtkWidget *widget, GdkEventExpose *evt, gpointer data)
{
	if (! GTK_WIDGET_DRAWABLE(heading_da))
		return TRUE;

	gdk_draw_pixbuf (heading_da->window, g_context.drawingarea_bggc,
		g_xpm_images[XPM_ID_POSITION_HEADING].pixbuf,
		0, 0, 0, 0, HEADING_WIDTH, HEADING_HEIGHT,
		GDK_RGB_DITHER_NONE, -1, -1);

	RESET_HEADING();

	return TRUE;
}

static inline int get_hacc_r()
{
	int r = MIN(g_view.width, g_view.height) >> 1;
	int hacc_r;

	if (isnan(g_gpsdata.hacc)) {
		hacc_r = 0;
	} else {
		int zoom = g_view.fglayer.repo->zoom;
		hacc_r = g_gpsdata.hacc / g_pixel_meters[zoom];
		if (hacc_r > r)
			hacc_r = r;
	}
	return hacc_r;
}

/**
 * call this on: zoom, move
 */
static gboolean map_draw_position()
{
	GdkDrawable *canvas = g_view.da->window;

	gboolean valid = (g_gpsdata.latlon_valid);

	x = g_view.pos_offset.x;
	y = g_view.pos_offset.y;
	r = get_hacc_r();

	/* Backup.
	 * Real range, including cross sign and circle.
	 * Take care about extra space due to round end */

	int rr = MAX(XPM_SIZE_HALF, r) + OUTER_WIDTH;
	GdkRectangle pos_rect = {x-rr, y-rr, rr << 1 , rr << 1};
	last_rect_valid = gdk_rectangle_intersect(&g_view.fglayer.visible, &pos_rect, &last_rect);

	/* Draw cross and circle */

	if (r >= 10) {
		int d = r << 1;
		gdk_gc_set_line_attributes(g_context.pos_hacc_circle_gc, OUTER_WIDTH,
			(valid? GDK_LINE_SOLID : GDK_LINE_ON_OFF_DASH), GDK_CAP_ROUND, GDK_JOIN_ROUND);
		gdk_gc_set_rgb_fg_color (g_context.pos_hacc_circle_gc, &g_base_colors[ID_COLOR_White]);
		gdk_draw_arc(canvas, g_context.pos_hacc_circle_gc, FALSE, x - r, y - r, d, d, 0, POS_ARC);

		gdk_gc_set_line_attributes(g_context.pos_hacc_circle_gc, INTER_WIDTH,
			(valid? GDK_LINE_SOLID : GDK_LINE_ON_OFF_DASH), GDK_CAP_ROUND, GDK_JOIN_ROUND);
		gdk_gc_set_rgb_fg_color (g_context.pos_hacc_circle_gc, &g_base_colors[ID_COLOR_Olive]);
		gdk_draw_arc(canvas, g_context.pos_hacc_circle_gc, FALSE, x - r, y - r, d, d, 0, POS_ARC);
		r = rr;
	} else {
		r = 0;
	}

	int id = valid? XPM_ID_POSITION_VALID : XPM_ID_POSITION_INVALID;
	gdk_draw_pixbuf (g_view.da->window, g_context.track_gc, g_xpm_images[id].pixbuf,
		0, 0, x - XPM_SIZE_HALF, y - XPM_SIZE_HALF, XPM_SIZE, XPM_SIZE,
		GDK_RGB_DITHER_NONE, -1, -1);

	return TRUE;
}

static void increment_draw()
{
	if (last_rect_valid) {
		gdk_draw_drawable (g_view.da->window, g_context.track_gc, g_view.pixmap,
			last_rect.x, last_rect.y, last_rect.x, last_rect.y,
			last_rect.width, last_rect.height);
	}

	GdkRectangle rect;
	track_draw(g_view.pixmap, FALSE, &rect);
	gdk_draw_drawable(g_view.da->window, g_context.track_gc, g_view.pixmap,
		rect.x, rect.y, rect.x, rect.y,	rect.width, rect.height);

	map_draw_position();
}

/**
 * Update current position's screen offset
 * return: TRUE: need redraw
 */
static gboolean update_position_offset()
{
	/* pixels */
	#define SENSITIVE_R 5
	static int lastx = -SENSITIVE_R;
	static int lasty = -SENSITIVE_R;
	static int last_pacc_r = 0;

	/* delta relative to view center */
	point_t pos_pixel = wgs84_to_tilepixel(g_view.pos_wgs84, g_view.fglayer.repo->zoom, g_view.fglayer.repo);
	g_view.pos_offset.x = pos_pixel.x - g_view.fglayer.tl_pixel.x;
	g_view.pos_offset.y = pos_pixel.y - g_view.fglayer.tl_pixel.y;

	int diff_x = abs(g_view.pos_offset.x - lastx);
	int diff_y = abs(g_view.pos_offset.y - lasty);

	if (sqrt((diff_x * diff_x) + (diff_y * diff_y)) >= SENSITIVE_R) {
		lastx = g_view.pos_offset.x;
		lasty = g_view.pos_offset.y;
		return TRUE;
	}

	int pacc_r = get_hacc_r();

	if (abs(pacc_r - last_pacc_r) >= SENSITIVE_R) {
		last_pacc_r = pacc_r;
		return TRUE;
	}

	return FALSE;
}

/**
 * The background map.
 * refresh: moved, zoomed, auto-center mode
 */
void map_redraw_view_gps_running()
{
	map_draw_back_layers(g_view.pixmap);

	track_draw(g_view.pixmap, TRUE, NULL);

	update_position_offset();

	gdk_draw_drawable (g_view.da->window, g_context.track_gc, g_view.pixmap,
		g_view.fglayer.visible.x, g_view.fglayer.visible.y,
		g_view.fglayer.visible.x, g_view.fglayer.visible.y,
		g_view.fglayer.visible.width, g_view.fglayer.visible.height);

	map_draw_position();
}

static void draw_labels(nav_da_data_t *da_data)
{
	if (! GTK_WIDGET_DRAWABLE(nav_da))
		return;

	int i, w, h, offset = da_data->offset;
	XPM_ID_T id;
	char c;

	GdkGC *gc = nav_da->style->bg_gc[GTK_WIDGET_STATE(nav_da)];
	gdk_draw_rectangle (nav_da->window, gc, TRUE, offset, (BOTTOM - FONT_HEIGHT), da_data->width, FONT_HEIGHT);

	da_data->last_hash = da_data->hash;
	if (da_data->hash == INVALID_HASH)
		return;

	for (i=0; i< da_data->text_count; i++) {
		c = da_data->text[i];

		switch(c) {
		case '0':
			id = XPM_ID_LETTER_0; break;
		case '1':
			id = XPM_ID_LETTER_1; break;
		case '2':
			id = XPM_ID_LETTER_2; break;
		case '3':
			id = XPM_ID_LETTER_3; break;
		case '4':
			id = XPM_ID_LETTER_4; break;
		case '5':
			id = XPM_ID_LETTER_5; break;
		case '6':
			id = XPM_ID_LETTER_6; break;
		case '7':
			id = XPM_ID_LETTER_7; break;
		case '8':
			id = XPM_ID_LETTER_8; break;
		case '9':
			id = XPM_ID_LETTER_9; break;
		case '.':
			id = XPM_ID_LETTER_dot; break;
		case '/':
			id = XPM_ID_LETTER_slash; break;
		case '-':
			id = XPM_ID_LETTER_minus; break;
		case 'M': /* m */
			id = XPM_ID_UNIT_m;	break;
		case 'K': /* km/h */
			id = XPM_ID_UNIT_kmph; break;
		case 'S': /* m/s */
			id = XPM_ID_UNIT_mps; break;
		case 'L': /* mph */
			id = XPM_ID_UNIT_mph; break;
		default:
			id = XPM_ID_NONE;
			break;
		}

		if (id == XPM_ID_NONE) {
			offset += FONT_WIDTH;
			continue;
		}

		w = g_xpm_images[id].width;
		h = g_xpm_images[id].height;
		gdk_draw_pixbuf (nav_da->window, gc, g_xpm_images[id].pixbuf,
			0, 0, offset, BOTTOM - h, w, h, GDK_RGB_DITHER_NORMAL, -1, -1);

		offset += w;
	}
}

static gboolean nav_da_expose_event (GtkWidget *widget, GdkEventExpose *evt, gpointer data)
{
	int i;
	for (i=0; i<3; i++) {
		if (POLL_STATE_TEST(STARTING)) {
			nav_da_data[i].hash = nav_da_data[i].last_hash = INVALID_HASH;
		} else {
			nav_da_data[i].hash = 0;
		}
		draw_labels(&nav_da_data[i]);
	}
	return FALSE;
}

/**
 * Called by polling thread on each poll.
 */
void poll_update_ui()
{
	static gboolean last_valid = FALSE;
	gboolean offset_sensitive = FALSE;
	int len;
	float speed_2d = 0;
	nav_da_data_t *da_data;

	if (g_gpsdata.latlon_valid) {
		g_view.pos_wgs84.lat = g_gpsdata.lat;
		g_view.pos_wgs84.lon = g_gpsdata.lon;
		offset_sensitive = update_position_offset();
	}

	/* height */

	da_data = &nav_da_data[0];

	if (g_gpsdata.height_valid) {
		da_data->hash = (U4)g_gpsdata.height;
		sprintf(da_data->text, "%6dM", (int)g_gpsdata.height);
	} else {
		da_data->hash = INVALID_HASH;
	}

	if (da_data->hash != da_data->last_hash)
		draw_labels(da_data);

	/* speed_2d */

	da_data = &nav_da_data[1];

	if (g_gpsdata.vel_valid) {
		speed_2d = g_gpsdata.speed_2d;
		if (g_context.speed_unit == SPEED_UNIT_KMPH) {
			speed_2d *= MPS_TO_KMPH;
		} else if (g_context.speed_unit == SPEED_UNIT_MPH) {
			speed_2d *= MPS_TO_MPH;
		}
		da_data->hash = (U4)((int)(speed_2d * 10)) << 8 | (U1)g_context.speed_unit;
		sprintf(da_data->text, "%6.1f%c", speed_2d, speed_unit_sign);
	} else {
		da_data->hash = INVALID_HASH;
	}

	if (da_data->hash != da_data->last_hash) {
		draw_labels(da_data);
		draw_speed_heading((da_data->hash > 0 && speed_2d > 0.1)? (int)g_gpsdata.heading_2d : -1);
	}

	/* sv */

	da_data = &nav_da_data[2];

	if (g_gpsdata.svinfo_valid) {
		da_data->hash = (g_gpsdata.sv_in_use << 8) | g_gpsdata.sv_get_signal;
		len = g_gpsdata.sv_in_use < 10? 2 : 3;
		len += g_gpsdata.sv_get_signal < 10? 1 : 2;
		sprintf(da_data->text,
			"%d/%d", g_gpsdata.sv_in_use, g_gpsdata.sv_get_signal);
	} else {
		da_data->hash = INVALID_HASH;
	}

	if (da_data->hash != da_data->last_hash) {
		draw_labels(da_data);
	}

	/* draw */

	gboolean out_of_range =
		g_view.pos_offset.x < cursor_range_tl.x ||
		g_view.pos_offset.y < cursor_range_tl.y ||
		g_view.pos_offset.x > cursor_range_br.x ||
		g_view.pos_offset.y > cursor_range_br.y;

	if (g_context.cursor_in_view && out_of_range) {
		map_centralize();
	} else if (offset_sensitive) {
		increment_draw();
	} else if (last_valid != g_gpsdata.latlon_valid) {
		update_position_offset();
		increment_draw();
	}

	last_valid = g_gpsdata.latlon_valid;
}

void ctx_tab_gps_fix_on_show()
{
	last_rect_valid = FALSE;

	status_label_set_text("", FALSE);
	if (POLL_STATE_TEST(RUNNING)) {
		map_set_redraw_func(&map_redraw_view_gps_running);
	} else {
		map_set_redraw_func(NULL);
	}
}

/* To avoid unnecessay calculations, we cache the cursor range on each FG layer update */
void poll_ui_on_view_range_changed()
{
	cursor_range_tl.x = g_view.fglayer.visible.width >> 2;
	cursor_range_tl.y = g_view.fglayer.visible.height >> 2;
	cursor_range_br.x = g_view.fglayer.visible.x + g_view.fglayer.visible.width - cursor_range_tl.x;
	cursor_range_br.y = g_view.fglayer.visible.x + g_view.fglayer.visible.width - cursor_range_tl.y;
}

void poll_ui_on_speed_unit_changed()
{
	if (g_context.speed_unit == SPEED_UNIT_KMPH)
		speed_unit_sign = 'K';
	else if (g_context.speed_unit == SPEED_UNIT_MPH)
		speed_unit_sign = 'L';
	else
		speed_unit_sign = 'S';
}

void ctx_gpsfix_on_track_state_changed()
{
	gboolean enabled = g_context.track_enabled;

	gtk_image_set_from_pixbuf(GTK_IMAGE(track_image),
		g_xpm_images[enabled? XPM_ID_TRACK_ON : XPM_ID_TRACK_OFF].pixbuf);
}

void ctx_gpsfix_on_poll_state_changed()
{
	last_rect_valid = FALSE;

	if (POLL_STATE_TEST(STARTING)) {
		int i;
		for (i=0; i<3; i++)
			nav_da_data[i].hash = nav_da_data[i].last_hash = INVALID_HASH;
		switch_to_main_view(CTX_ID_GPS_FIX);
	} else if (POLL_STATE_TEST(RUNNING)) {
		if (ctx_tab_get_current_id() == CTX_ID_NONE)
			switch_to_ctx_tab(CTX_ID_GPS_FIX);
	} else if (POLL_STATE_TEST(SUSPENDING)) {
		if (ctx_tab_get_current_id() == CTX_ID_GPS_FIX)
			switch_to_ctx_tab(CTX_ID_NONE);
	}
}

void ctx_gpsfix_on_poll_engine_changed()
{
	XPM_ID_T id;

	switch(g_context.poll_engine) {
	case POLL_ENGINE_OGPSD:
		id = XPM_ID_POLLENGINE_FSO;
		break;
	case POLL_ENGINE_UBX:
		id = XPM_ID_POLLENGINE_UBX;
		break;
	default:
		return;
	}
	gtk_image_set_from_pixbuf(GTK_IMAGE(poll_engine_image), g_xpm_images[id].pixbuf);
}

GtkWidget * ctx_tab_gps_fix_create()
{
	poll_ui_on_speed_unit_changed();

	GtkWidget *hbox = gtk_hbox_new(FALSE, 0);

	nav_da_data[0].text_count = NAV_H_NUM;
	nav_da_data[1].text_count = NAV_SPD_NUM;
	nav_da_data[2].text_count = NAV_SV_NUM;

	nav_da_data[0].offset = 0;
	nav_da_data[0].width = NAV_H_NUM * FONT_WIDTH + 15;

	nav_da_data[1].offset = nav_da_data[0].offset + nav_da_data[0].width ;
	nav_da_data[1].width = NAV_SPD_NUM * FONT_WIDTH + 35;

	nav_da_data[2].offset = nav_da_data[1].offset + nav_da_data[1].width;
	nav_da_data[2].width = NAV_SV_NUM * FONT_WIDTH;

	nav_da_data[0].hash = nav_da_data[0].last_hash = INVALID_HASH;
	nav_da_data[1].hash = nav_da_data[0].last_hash = INVALID_HASH;
	nav_da_data[2].hash = nav_da_data[0].last_hash = INVALID_HASH;

	nav_da = gtk_drawing_area_new();
	gtk_widget_set_size_request(nav_da, -1, FONT_HEIGHT);
	gtk_box_pack_start (GTK_BOX (hbox), nav_da, TRUE, TRUE, 0);
	g_signal_connect (nav_da, "expose-event", G_CALLBACK(nav_da_expose_event), NULL);

	/* tips box */
	GtkWidget *hbox_tips = gtk_hbox_new(FALSE, 0);
	gtk_box_pack_end (GTK_BOX (hbox), hbox_tips, FALSE, FALSE, 2);

	/* speed heading */
	heading_da = gtk_drawing_area_new();
	gtk_widget_set_size_request(heading_da, HEADING_WIDTH, HEADING_HEIGHT);
	gtk_box_pack_start(GTK_BOX (hbox_tips), heading_da, FALSE, FALSE, 0);
	g_signal_connect (heading_da, "expose-event", G_CALLBACK(heading_da_expose_event), NULL);

	/* track and poll engine */
	GtkWidget *vbox = gtk_vbox_new(FALSE, 0);
	gtk_box_pack_start(GTK_BOX (hbox_tips), vbox, FALSE, FALSE, 0);

	track_image = gtk_image_new_from_pixbuf(g_xpm_images[XPM_ID_TRACK_OFF].pixbuf);
	gtk_box_pack_start(GTK_BOX (vbox), track_image, FALSE, FALSE, 0);

	poll_engine_image = gtk_image_new();
	gtk_box_pack_end(GTK_BOX (vbox), poll_engine_image, FALSE, FALSE, 0);

	return hbox;
}
