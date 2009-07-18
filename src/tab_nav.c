#include <math.h>
#include "omgps.h"
#include "gps.h"
#include "xpm_image.h"
#include "customized.h"

static GtkWidget *ubx_svinfo_treeview, *ubx_svinfo_treeview_sw;
static GtkListStore *ubx_svinfo_store = NULL;
static char *ubx_svinfo_col_names[] = {"SVID", "USED", "DCD", "EPH", "ORB", "Elev", "Azim", "CNO"};

static GtkWidget *basicinfo_table, *ogpsd_svinfo_treeview, *ogpsd_svinfo_treeview_sw;
static GtkListStore *ogpsd_svinfo_store = NULL;
static char *ogpsd_svinfo_col_names[] = {"SVID", "USED", "Elev", "Azim", "CNO"};

static GtkWidget *speed_unit_radios[3];
static char *speed_unit_labels[3] = {"km/h", "mph", "m/s"};
static speed_unit_t speed_unit_values[3] = {SPEED_UNIT_KMPH, SPEED_UNIT_MPH, SPEED_UNIT_MPS};

static GtkWidget *time_label, *notebook, *skymap_da, *speed_2d_label, *vel_down_label;

#define DEG_TO_RAD 		(M_PI / 180)

#define CIRCLE_ARC		23040

#define EDGE			16

/* bg_image_r: max circle, less than bg_image_d */
static int da_width, da_height, bg_image_d, bg_image_r;

static U4 last_hash[SV_MAX_CHANNELS], hash[SV_MAX_CHANNELS];

typedef enum {
	PAGE_NUM_BASIC,
	PAGE_NUM_SVINFO,
	PAGE_NUM_SKYMAP
} NAV_PAGE_NUM;

static int cur_page_num = PAGE_NUM_BASIC;

/* It seems only first 4 are used actually */
static char *fixtype_text[] = { "No fix", "Dead reckoning", "2D", "3D",
	"GPS + dead reckoning",	"Time only fix"};

typedef enum
{
	FIX_TYPE,
	LAT,
	LON,
	HACC,
	HEIGHT,
	SPEED2D,
	HEADING2D,
	VELDOWN,
	ROW_COUNT
} ROW_IDX;

static char *nav_basic_names[] = {
	 " Fix Type",
	 " Latitude",
	 " Longitude",
	 " HACC",
	 " Altitude",
	 " Ground Speed",
	 " Ground Heading",
	 " Down Velocity",
};

static char *nav_basic_units[] = {
	 "",
	 "°",
	 "°",
	 "m",
	 "m",
	 "",
	 "°",
	 "",
};

static GtkWidget *labels[ROW_COUNT];
static char labels_text[ROW_COUNT][64];

static inline void update_nav_basic();

static void draw_skymap_fg(GdkDrawable *canvas)
{
	#define DEG_TO_RAD 		(M_PI / 180)
	#define IMG_SIZE 		15
	#define IMG_SIZE_HALF 	7

	int i;
	svinfo_channel_t *sv;
	int r, x, y;
	double deg;
	XPM_ID_T xpm_id;
	int center_x = da_width >> 1;
	int center_y = da_height >> 1;

	for (i=0; i<g_gpsdata.sv_channel_count; i++) {
		sv = &g_gpsdata.sv_channels[i];
		if (sv->elevation >= 0 && sv->azimuth >= 0) {
			r = bg_image_r * (1.0 - sv->elevation / 90.0);
			deg = sv->azimuth * DEG_TO_RAD;
			x = center_x + r * sin(deg);
			y = center_y - r * cos(deg);

			if (sv->flags & 0x01)
				xpm_id = XPM_ID_SV_IN_USE;
			else if (sv->cno > 0)
				xpm_id = XPM_ID_SV_SIGNAL;
			else
				xpm_id = XPM_ID_SV_NO_SIGNAL;

			gdk_draw_pixbuf(canvas, g_context.skymap_gc, g_xpm_images[xpm_id].pixbuf,
				0, 0, x - IMG_SIZE_HALF, y - IMG_SIZE_HALF, IMG_SIZE, IMG_SIZE, GDK_RGB_DITHER_NONE, -1, -1);
		}
	}
}

static inline void update_skymap()
{
	int i;
	U1 color;
	svinfo_channel_t *sv;

	memset(hash, 0, sizeof(hash));

	for (i=0; i<g_gpsdata.sv_channel_count; i++) {
		sv = &g_gpsdata.sv_channels[i];
		color = (sv->flags & 0x01)? 0x2 : ((sv->cno > 0)? 0x1 : 0x0);
		hash[i] = color | (sv->elevation << 8) | (sv->azimuth << 24);
	}

	if (memcmp(hash, last_hash, sizeof(hash)) == 0)
		return;

	memcpy(last_hash, hash, sizeof(last_hash));

	int center_x = da_width >> 1;
	int center_y = da_height >> 1;

	int off_x = center_x - bg_image_r;
	int off_y = center_y - bg_image_r;

	if (g_view.sky_pixbuf) {
		gdk_draw_pixbuf (g_view.pixmap, g_context.drawingarea_bggc, g_view.sky_pixbuf,
			0, 0, off_x, off_y, bg_image_d, bg_image_d, GDK_RGB_DITHER_NONE, -1, -1);
		draw_skymap_fg(g_view.pixmap);
		gdk_draw_drawable (skymap_da->window, g_context.drawingarea_bggc, g_view.pixmap,
			off_x, off_y, off_x, off_y, bg_image_d, bg_image_d);
	} else {
		gdk_draw_rectangle (skymap_da->window, skymap_da->style->bg_gc[GTK_STATE_NORMAL], TRUE, 0, 0, da_width, da_height);
		gdk_draw_line(skymap_da->window, g_context.grid_line_gc, off_x, center_y, da_width - off_x, center_y);
		gdk_draw_line(skymap_da->window, g_context.grid_line_gc, center_x, off_y, center_x, da_height - off_y);

		int d;
		int R = bg_image_r - (EDGE >> 1);
		for (i=1; i<=9; i++) {
			d = R * i / 9.0;
			gdk_draw_arc(skymap_da->window, g_context.grid_line_gc, FALSE,
				center_x - d, center_y - d, d << 1, d << 1, 0, CIRCLE_ARC);
		}
		g_view.sky_pixbuf = gdk_pixbuf_get_from_drawable (NULL, g_view.pixmap,
			gdk_rgb_get_colormap(), 0, 0, 0, 0, bg_image_d, bg_image_d);

		draw_skymap_fg(skymap_da->window);
	}
}

/**
 * Update UI waste too much CPU cycles -- up to 50% usage!
 * We compare cached signature with new calculated to avoid unnecessary redraw.
 */
static inline void update_svinfo()
{
	GtkTreeIter iter;
	int i, j, k;
	U1 flags;
	svinfo_channel_t *sv;
	int channel_ids[SV_MAX_CHANNELS];

	k = g_gpsdata.sv_channel_count - 1;
	j = 0;

	for (i=k; i>=0; i--) {
		sv = &g_gpsdata.sv_channels[i];
		if (sv->cno > 0)
			channel_ids[j++] = i;
		else
			channel_ids[k--] = i;
	}

	char elev_buf[5], azim_buf[5];

	if (POLL_ENGINE_TEST(UBX)) {
		gtk_list_store_clear(ubx_svinfo_store);

		for (i=0; i<g_gpsdata.sv_channel_count; i++) {
			sv = &g_gpsdata.sv_channels[channel_ids[i]];

			/* seems elev and azim are valid when orbit data is EPH */
			if (sv->elevation >= 0)
				snprintf(elev_buf, sizeof(elev_buf), "%d", sv->elevation);
			else
				elev_buf[0] = '\0';

			if (sv->azimuth >= 0)
				snprintf(azim_buf, sizeof(azim_buf), "%d", sv->azimuth);
			else
				azim_buf[0] = '\0';

			flags = sv->flags;

			gtk_list_store_append (ubx_svinfo_store, &iter);
			gtk_list_store_set (ubx_svinfo_store, &iter,
				0, (int)sv->sv_id,
				1, (flags & 0x01)? "*" : "",
				2, (flags & 0x02)? "*" : "",
				3, (flags & 0x04 && (flags & 0x08))? "*" : "",
				4, (flags & 0x04)? "*" : "",
				5, elev_buf,
				6, azim_buf,
				7, sv->cno,
				-1);
		}
	} else {
		gtk_list_store_clear(ogpsd_svinfo_store);

		for (i=0; i<g_gpsdata.sv_channel_count; i++) {
			sv = &g_gpsdata.sv_channels[channel_ids[i]];
			gtk_list_store_append (ogpsd_svinfo_store, &iter);
			gtk_list_store_set (ogpsd_svinfo_store, &iter,
				0, (int)sv->sv_id,
				1, (sv->flags == 0x01)? "*" : " ",
				2, sv->elevation,
				3, sv->azimuth,
				4, sv->cno,
				-1);
		}
	}
}

/* Waste CPU! */
static void svinfo_treeview_func_text(GtkTreeViewColumn *tree_column, GtkCellRenderer *cell, GtkTreeModel *model,
	GtkTreeIter *iter, gpointer data)
{
	char *value;
	gtk_tree_model_get(model, iter, 1, &value, -1);
	if (value && strcmp(value, "*") == 0)
		g_object_set(cell, "foreground", "Blue", "foreground-set", TRUE, NULL);
	else
		g_object_set(cell, "foreground-set", FALSE, NULL);
}

static void create_treeview(GtkWidget *treeview, GtkWidget *treeview_sw, char *col_names[],
		int col_names_count)
{
	GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
	gtk_tree_selection_set_mode (sel, GTK_SELECTION_NONE);

	/* fixed with and height improve performance */
	gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(treeview), TRUE);
	gtk_tree_view_set_rules_hint (GTK_TREE_VIEW (treeview), TRUE);
	gtk_tree_view_set_reorderable(GTK_TREE_VIEW (treeview), FALSE);
	gtk_container_add (GTK_CONTAINER (treeview_sw), treeview);

	/* add columns to the tree view */
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *col;
	/* hack! minimal screen width - scroll bar width - separators and borders */
	int width = 430 / col_names_count;

	int i;
	for (i=0; i<col_names_count; i++) {
		renderer = gtk_cell_renderer_text_new();
		col = gtk_tree_view_column_new_with_attributes (col_names[i], renderer, "text", i, NULL);
	  	gtk_tree_view_column_set_sizing (col, GTK_TREE_VIEW_COLUMN_FIXED);
	  	gtk_tree_view_column_set_fixed_width(col, width);
		gtk_tree_view_column_set_clickable(col, FALSE);
		gtk_tree_view_column_set_cell_data_func (col, renderer, svinfo_treeview_func_text, NULL, NULL);
		gtk_tree_view_append_column (GTK_TREE_VIEW(treeview), col);
	}
}

static void on_speed_unit_changed()
{
	char *label;
	if (g_context.speed_unit == SPEED_UNIT_KMPH)
		label = speed_unit_labels[0];
	else if (g_context.speed_unit == SPEED_UNIT_MPH)
		label = speed_unit_labels[1];
	else
		label = speed_unit_labels[2];

	gtk_label_set_text(GTK_LABEL(speed_2d_label), label);
	gtk_label_set_text(GTK_LABEL(vel_down_label), label);

	update_nav_basic();
}

static void speed_unit_changed(GtkWidget *widget, gpointer data)
{
	speed_unit_t unit = (speed_unit_t)data;
	if (unit == g_context.speed_unit)
		return;

	g_context.speed_unit = unit;

	on_speed_unit_changed();
	poll_ui_on_speed_unit_changed();
}

/**
 * GtkTreeview is not used here, because it's poor performance according to frequently updates.
 */
static GtkWidget * create_basic_nav_table()
{
	int i;
	GtkWidget *hbox, *label, *event_box, *vbox_1, *vbox_2, *vbox_3;
	GdkColor *color_1 = &g_base_colors[ID_COLOR_White];
	GdkColor color;
	gdk_color_parse("#F0F0F0", &color);
	GdkColor *color_2 = &color;
	int padding_x = 6, padding_y = 6;

	hbox = gtk_hbox_new(TRUE, 1);
	vbox_1 = gtk_vbox_new(FALSE, 1);
	vbox_2 = gtk_vbox_new(FALSE, 1);
	vbox_3 = gtk_vbox_new(FALSE, 1);

	gtk_container_add(GTK_CONTAINER(hbox), vbox_1);
	gtk_container_add(GTK_CONTAINER(hbox), vbox_2);
	gtk_container_add(GTK_CONTAINER(hbox), vbox_3);

	for (i = 0; i<ROW_COUNT; i++) {
		/* first column */
		label = gtk_label_new(nav_basic_names[i]);
		gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_FILL);
		gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
		gtk_misc_set_padding(GTK_MISC(label), padding_x, padding_y);

		event_box = gtk_event_box_new();
		gtk_widget_modify_bg(event_box, GTK_STATE_NORMAL, (i % 2 == 0)? color_1 : color_2);

		gtk_container_add(GTK_CONTAINER(event_box), label);
		gtk_container_add(GTK_CONTAINER(vbox_1), event_box);

		/* second column */

		labels[i] = gtk_label_new("");
		gtk_label_set_use_markup(GTK_LABEL(labels[i]), FALSE);
		gtk_misc_set_alignment(GTK_MISC(labels[i]), 0.0, 0.5);
		gtk_misc_set_padding(GTK_MISC(labels[i]), padding_x, padding_y);

		event_box = gtk_event_box_new();
		gtk_widget_modify_bg(event_box, GTK_STATE_NORMAL, (i % 2 == 0)? color_1 : color_2);

		gtk_container_add(GTK_CONTAINER(event_box), labels[i]);
		gtk_container_add(GTK_CONTAINER(vbox_2), event_box);

		/* third column */

		label = gtk_label_new(nav_basic_units[i]);
		gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
		gtk_misc_set_padding(GTK_MISC(label), padding_x, padding_y);

		if (i == SPEED2D)
			speed_2d_label = label;
		else if (i == VELDOWN)
			vel_down_label = label;

		event_box = gtk_event_box_new();
		gtk_widget_modify_bg(event_box, GTK_STATE_NORMAL, (i % 2 == 0)? color_1 : color_2);

		gtk_container_add(GTK_CONTAINER(event_box), label);
		gtk_container_add(GTK_CONTAINER(vbox_3), event_box);
	}

	on_speed_unit_changed();

	GtkWidget *vbox = gtk_vbox_new(FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

	/* speed unit */

	GtkWidget *speed_unit_hbox = gtk_hbox_new(FALSE, 10);
	GtkWidget *speed_unit_head = gtk_label_new("Speed unit: ");
	gtk_box_pack_start(GTK_BOX(speed_unit_hbox), speed_unit_head, FALSE, FALSE, 5);

	for (i=0; i<3; i++) {
		speed_unit_radios[i] = gtk_radio_button_new_with_label_from_widget(
			i==0? NULL : GTK_RADIO_BUTTON(speed_unit_radios[i-1]), speed_unit_labels[i]);
		gtk_box_pack_start(GTK_BOX(speed_unit_hbox), speed_unit_radios[i], FALSE, FALSE, 10);
		g_signal_connect(G_OBJECT(speed_unit_radios[i]), "toggled",
			G_CALLBACK(speed_unit_changed), (gpointer) speed_unit_values[i]);
	}

	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(speed_unit_radios[0]),
		g_context.speed_unit == SPEED_UNIT_KMPH);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(speed_unit_radios[1]),
		g_context.speed_unit == SPEED_UNIT_MPH);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(speed_unit_radios[2]),
		g_context.speed_unit == SPEED_UNIT_MPS);

	gtk_box_pack_start (GTK_BOX (vbox), speed_unit_hbox, FALSE, FALSE, 5);

	GtkWidget *padding = gtk_label_new("\n\n\n\n");
	gtk_box_pack_end(GTK_BOX(vbox), padding, TRUE, TRUE, 0);

	return vbox;
}

static gboolean skymap_da_expose_event (GtkWidget *widget, GdkEventExpose *evt, gpointer data)
{
	if (POLL_STATE_TEST(RUNNING)) {
		memset(last_hash, 0, sizeof(last_hash));
		update_skymap();
	}

	return FALSE;
}

static gboolean skymap_da_configure_event (GtkWidget *widget, GdkEventConfigure *evt, gpointer data)
{
	da_width = evt->width;
	da_height = evt->height;

	#ifndef ICONDIR
	#define ICONDIR "/usr/share/pixmaps/omgps/"
	#endif

	if (g_view.sky_pixbuf) {
		g_object_unref(g_view.sky_pixbuf);
		g_view.sky_pixbuf = NULL;
	}

	GError *error = NULL;
	char *file = ICONDIR"/sky.png";

	/* slightly enlarge the bg image, because fg SV's can be drawn out of biggest circle. */

	if (g_view.sky_pixbuf)
		g_object_unref(g_view.sky_pixbuf);

	int d;
	g_view.sky_pixbuf = gdk_pixbuf_new_from_file_at_scale(file, da_width - EDGE, da_height - EDGE, TRUE, &error);

	bg_image_d = MIN(da_width, da_height);
	bg_image_r = bg_image_d >> 1;

	if (g_view.sky_pixbuf) {
		d = gdk_pixbuf_get_width(g_view.sky_pixbuf);

		int off_x = (da_width - d) >> 1;
		int off_y = (da_height - d) >> 1;
		gdk_draw_rectangle (g_view.pixmap, skymap_da->style->bg_gc[GTK_STATE_NORMAL], TRUE, 0, 0, da_width, da_height);
		gdk_draw_pixbuf (g_view.pixmap, g_context.drawingarea_bggc, g_view.sky_pixbuf,
			0, 0, off_x, off_y, d, d, GDK_RGB_DITHER_NONE, -1, -1);
		g_object_unref(g_view.sky_pixbuf);
		off_x = (da_width - bg_image_d) >> 1;
		off_y = (da_height - bg_image_d) >> 1;
		g_view.sky_pixbuf = gdk_pixbuf_get_from_drawable (NULL, g_view.pixmap,
			gdk_rgb_get_colormap(), off_x, off_y, 0, 0, bg_image_d, bg_image_d);
	}

	return FALSE;
}

static inline void update_nav_basic()
{
	int i;
	float speed_2d, vel_down;

	for (i = 0; i<ROW_COUNT; i++)
		labels_text[i][0] = '\0';

	sprintf(labels_text[FIX_TYPE], "%s", fixtype_text[g_gpsdata.nav_status_fixtype]);

	if (g_gpsdata.latlon_valid) {
		sprintf(labels_text[LAT], "%f (%c)", fabs(g_gpsdata.lat), (g_gpsdata.lat > 0? 'N' : 'S'));
		sprintf(labels_text[LON], "%f (%c)", fabs(g_gpsdata.lon), (g_gpsdata.lon > 0? 'E' : 'W'));
		sprintf(labels_text[HACC], "%.2f", g_gpsdata.hacc);
	}

	if (g_gpsdata.height_valid) {
		sprintf(labels_text[HEIGHT], "%.2f ± %.2f", g_gpsdata.height, g_gpsdata.vacc);
	}

	if (g_gpsdata.vel_valid) {
		speed_2d = g_gpsdata.speed_2d;
		vel_down = g_gpsdata.vel_down;
		if (g_context.speed_unit == SPEED_UNIT_KMPH) {
			speed_2d *= MPS_TO_KMPH;
			vel_down *= MPS_TO_KMPH;
		} else if (g_context.speed_unit == SPEED_UNIT_MPH) {
			speed_2d *= MPS_TO_MPH;
			vel_down *= MPS_TO_MPH;
		}

		sprintf(labels_text[SPEED2D], "%.2f", speed_2d);
		sprintf(labels_text[HEADING2D], "%.1f", g_gpsdata.heading_2d);
		sprintf(labels_text[VELDOWN], "%.2f", vel_down);
	}

	for (i = 0; i<ROW_COUNT; i++) {
		gtk_label_set_text(GTK_LABEL(labels[i]), labels_text[i]);
	}
}

void update_nav_tab()
{
	char buf[64];
	time_t tm;
	time(&tm);
	struct tm *t = localtime(&tm);
	strftime(buf, sizeof(buf), " Last update time = %H:%M:%S", t);
	gtk_label_set_text(GTK_LABEL(time_label), buf);

	switch (cur_page_num) {
	case PAGE_NUM_BASIC:
		update_nav_basic();
		break;
	case PAGE_NUM_SVINFO:
		update_svinfo();
		break;
	default:
		update_skymap();
	}
}

void nav_tab_on_show()
{
	if (POLL_STATE_TEST(SUSPENDING)) {
		gtk_widget_hide(time_label);
		gtk_widget_hide(basicinfo_table);
		gtk_widget_hide(ubx_svinfo_treeview_sw);
		gtk_widget_hide(ogpsd_svinfo_treeview_sw);
	} else if (POLL_STATE_TEST(RUNNING)) {
		gtk_widget_show(basicinfo_table);
		gtk_widget_show(time_label);
		if (POLL_ENGINE_TEST(OGPSD)) {
			gtk_widget_hide(ubx_svinfo_treeview_sw);
			gtk_widget_show(ogpsd_svinfo_treeview_sw);
		} else {
			gtk_widget_show(ubx_svinfo_treeview_sw);
			gtk_widget_hide(ogpsd_svinfo_treeview_sw);
		}
	}
	update_nav_tab();
}

void notebook_switched(GtkWidget *notebook, GtkNotebookPage *page, int num, gpointer data)
{
	cur_page_num = num;

	if (num != PAGE_NUM_SKYMAP)
		update_nav_tab();
}

GtkWidget * nav_tab_create()
{
	memset(last_hash, 0, sizeof(last_hash));

	GtkWidget *label;
	GtkWidget *vbox = gtk_vbox_new(FALSE, 0);

	/* time label */
	time_label = gtk_label_new("");
	gtk_label_set_selectable(GTK_LABEL(time_label), FALSE);
	gtk_widget_modify_fg(time_label, GTK_STATE_NORMAL, &g_base_colors[ID_COLOR_White]);
	gtk_misc_set_alignment(GTK_MISC(time_label), 0, 0);
	gtk_box_pack_start (GTK_BOX (vbox), time_label, FALSE, FALSE, 5);

	notebook = gtk_notebook_new();
	gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), FALSE);
	gtk_notebook_set_show_border(GTK_NOTEBOOK(notebook), FALSE);
	g_signal_connect (G_OBJECT (notebook), "switch-page", G_CALLBACK (notebook_switched), NULL);

	gtk_box_pack_start (GTK_BOX (vbox), notebook, TRUE, TRUE, 0);

	/* basic table */
	basicinfo_table = create_basic_nav_table();
	GtkWidget *basicinfo_sw = new_scrolled_window(basicinfo_table);
	label = gtk_label_new("     Basic Nav    ");
	gtk_notebook_append_page (GTK_NOTEBOOK (notebook), basicinfo_sw, label);

	GtkWidget *svinfo_vbox = gtk_vbox_new(FALSE, 0);

	/* svinfo treeviews ubx */
	ubx_svinfo_treeview = gtk_tree_view_new ();
	ubx_svinfo_treeview_sw = new_scrolled_window (NULL);
	create_treeview(ubx_svinfo_treeview, ubx_svinfo_treeview_sw, ubx_svinfo_col_names,
			sizeof(ubx_svinfo_col_names)/sizeof(char *));

	ubx_svinfo_store = gtk_list_store_new (sizeof(ubx_svinfo_col_names)/sizeof(char*),
		G_TYPE_UINT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
		G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT);
	gtk_tree_view_set_model(GTK_TREE_VIEW(ubx_svinfo_treeview), GTK_TREE_MODEL(ubx_svinfo_store));

	gtk_container_add(GTK_CONTAINER(svinfo_vbox), ubx_svinfo_treeview_sw);

	/* svinfo trview: ogpsd */
	ogpsd_svinfo_treeview = gtk_tree_view_new ();
	ogpsd_svinfo_treeview_sw = new_scrolled_window (NULL);
	create_treeview(ogpsd_svinfo_treeview, ogpsd_svinfo_treeview_sw, ogpsd_svinfo_col_names,
			sizeof(ogpsd_svinfo_col_names)/sizeof(char *));

	ogpsd_svinfo_store = gtk_list_store_new (sizeof(ogpsd_svinfo_col_names)/sizeof(char*),
		G_TYPE_UINT, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT);
	gtk_tree_view_set_model(GTK_TREE_VIEW(ogpsd_svinfo_treeview), GTK_TREE_MODEL(ogpsd_svinfo_store));

	gtk_container_add(GTK_CONTAINER(svinfo_vbox), ogpsd_svinfo_treeview_sw);

	label = gtk_label_new("       SV List      ");
	gtk_notebook_append_page (GTK_NOTEBOOK (notebook), svinfo_vbox, label);

	/* sky map */
	label = gtk_label_new("      Sky Map    ");
	skymap_da = gtk_drawing_area_new();
	g_signal_connect (skymap_da, "expose-event", G_CALLBACK(skymap_da_expose_event), NULL);
	g_signal_connect (skymap_da, "configure-event", G_CALLBACK(skymap_da_configure_event), NULL);

	GtkWidget *skymap_sw = new_scrolled_window(skymap_da);
	gtk_notebook_append_page (GTK_NOTEBOOK (notebook), skymap_sw, label);

	return vbox;
}
