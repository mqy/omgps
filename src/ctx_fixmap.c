#include "omgps.h"
#include "util.h"
#include "customized.h"
#include "xpm_image.h"

static GtkWidget *lockview_button, *srcloc_button, *destloc_button, *set_button,
	*reset_button, *close_button;
static mouse_handler_t mouse_handler;
static point_t clicked_point, src_point, dest_point;
static gboolean src_set = FALSE, dest_set = FALSE;

void ctx_tab_fix_map_on_show()
{
	src_set = dest_set = FALSE;
	clicked_point.x = clicked_point.y = -1;

	gtk_widget_set_sensitive(srcloc_button, FALSE);
	gtk_widget_set_sensitive(destloc_button, FALSE);
	gtk_widget_set_sensitive(set_button, FALSE);
	gtk_widget_set_sensitive(reset_button,
		fabs(g_view.fglayer.repo->lat_fix) > 0 || fabs(g_view.fglayer.repo->lat_fix));
}

static inline void draw_cross_sign(point_t center)
{
	if (center.x < 0 || center.y < 0)
		return;

	const int r = 15;

	gdk_draw_line(g_view.da->window, g_context.crosssign_gc,
		center.x - 15, center.y, center.x + r, center.y);
	gdk_draw_line(g_view.da->window, g_context.crosssign_gc,
		center.x, center.y - 15, center.x, center.y + r);
}

static void fixmap_redraw_view()
{
	map_draw_back_layers(g_view.da->window);
	int w, h;
	xpm_t *xpm;

	if (src_set) {
		xpm = &g_xpm_images[XPM_ID_FIXMAP_SRC];
		w = xpm->width;
		h = xpm->height;
		gdk_draw_pixbuf (g_view.da->window, g_context.crosssign_gc, xpm->pixbuf,
			0, 0, src_point.x - (w>>1), src_point.y - (h>>1), w, h, GDK_RGB_DITHER_NONE, -1, -1);
	}

	if (dest_set) {
		xpm = &g_xpm_images[XPM_ID_FIXMAP_DEST];
		w = xpm->width;
		h = xpm->height;
		gdk_draw_pixbuf (g_view.da->window, g_context.crosssign_gc, xpm->pixbuf,
			0, 0, dest_point.x - (w>>1), dest_point.y - (h>>1), w, h, GDK_RGB_DITHER_NONE, -1, -1);
	}
}

static void lockview_button_toggled(GtkWidget *widget, gpointer data)
{
	src_set = dest_set = FALSE;

	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lockview_button))) {
		map_config_main_view(&mouse_handler, 0x2|0x4|0x8, TRUE, TRUE);
		map_draw_back_layers(g_view.da->window);
		map_set_redraw_func(&fixmap_redraw_view);
	} else {
		clicked_point.x = clicked_point.y = -1;
		map_set_redraw_func(NULL);
		map_config_main_view(NULL, 0x2|0x4|0x8, FALSE, FALSE);
	}
}

static gboolean check(gboolean is_dest)
{
	if (clicked_point.x < 0 || clicked_point.y < 0) {
		warn_dialog("Please set location by clicking map");
		return FALSE;
	}

	if ((is_dest && ! src_set) || (! is_dest && ! dest_set))
		return TRUE;

	point_t point = is_dest? src_point : dest_point;

	int deltax = clicked_point.x - point.x;
	int deltay = clicked_point.y - point.y;

	if (sqrt(deltax * deltax + deltay * deltay) < 10) {
		warn_dialog("Distance on screen is too small, ignore");
		return FALSE;
	}

	gtk_widget_set_sensitive(srcloc_button, FALSE);
	gtk_widget_set_sensitive(destloc_button, FALSE);

	gtk_widget_set_sensitive(set_button, (is_dest && src_set) || (! is_dest && dest_set));

	return TRUE;
}

static void srcloc_button_clicked(GtkWidget *widget, gpointer data)
{
	if (! check(FALSE))
		return;

	src_point = clicked_point;
	src_set = TRUE;
	clicked_point.x = clicked_point.y = -1;

	fixmap_redraw_view();
}

static void destloc_button_clicked(GtkWidget *widget, gpointer data)
{
	if (! check(TRUE))
		return;

	dest_point = clicked_point;
	dest_set = TRUE;
	clicked_point.x = clicked_point.y = -1;

	fixmap_redraw_view();
}

static void set_button_clicked(GtkWidget *widget, gpointer data)
{
	point_t pixel;
	pixel.x = g_view.fglayer.center_pixel.x + (dest_point.x - src_point.x);
	pixel.y = g_view.fglayer.center_pixel.y + (dest_point.y - src_point.y);

	coord_t center_wgs84 = tilepixel_to_wgs84(pixel,
		g_view.fglayer.repo->zoom, g_view.fglayer.repo);

	float lat_fix = center_wgs84.lat - g_view.center_wgs84.lat;
	float lon_fix = center_wgs84.lon - g_view.center_wgs84.lon;

	char buf[256];
	if (fabs(lat_fix) > MAX_LAT_LON_FIX || fabs(lon_fix) > MAX_LAT_LON_FIX) {
		snprintf(buf, sizeof(buf), "lat fix=%f\nlon fix=%f\nMay be incorrect if fix >= 0.1°",
			lat_fix, lon_fix);
		if (! confirm_dialog(buf))
			return;
	}

	g_view.fglayer.repo->lat_fix += lat_fix;
	g_view.fglayer.repo->lon_fix += lon_fix;

	fixmap_update_maplist(g_view.fglayer.repo);

	src_set = dest_set = FALSE;
	clicked_point.x = clicked_point.y = -1;

	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lockview_button), FALSE);
	gtk_widget_set_sensitive(reset_button, TRUE);
	gtk_widget_set_sensitive(set_button, FALSE);

	map_zoom_to(g_view.fglayer.repo->zoom, g_view.center_wgs84, TRUE);
}

static void reset_button_clicked(GtkWidget *widget, gpointer data)
{
	g_view.fglayer.repo->lat_fix = 0;
	g_view.fglayer.repo->lon_fix = 0;

	src_set = dest_set = FALSE;
	clicked_point.x = clicked_point.y = -1;

	gtk_widget_set_sensitive(reset_button, FALSE);

	fixmap_update_maplist(g_view.fglayer.repo);

	map_zoom_to(g_view.fglayer.repo->zoom, g_view.center_wgs84, TRUE);
}

static void close_button_clicked()
{
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lockview_button), FALSE);

	drawingarea_reset_default_mouse_handler();
	switch_to_main_view(CTX_ID_GPS_FIX);
}

static void mouse_released(point_t point, guint time)
{
	if (clicked_point.x >= 0 && clicked_point.y >= 0) {
		draw_cross_sign(clicked_point);
	}

	draw_cross_sign(point);

	gtk_widget_set_sensitive(srcloc_button, TRUE);
	gtk_widget_set_sensitive(destloc_button, TRUE);

	clicked_point = point;
}

GtkWidget* ctx_tab_fix_map_create()
{
	mouse_handler.press_handler = NULL;
	mouse_handler.release_handler = mouse_released;
	mouse_handler.motion_handler = NULL;

	GtkWidget *vbox = gtk_vbox_new(FALSE, 0);
	GtkWidget *title = gtk_label_new("Fix FG map");
	GtkWidget *button_hbox = gtk_hbox_new(TRUE, 0);

	lockview_button = gtk_toggle_button_new_with_label("lockview");
	srcloc_button = gtk_button_new_with_label("src");
	destloc_button = gtk_button_new_with_label("dest");
	set_button = gtk_button_new_with_label("set");
	reset_button = gtk_button_new_with_label("reset");
	close_button = gtk_button_new_with_label("close");

	g_signal_connect (G_OBJECT (lockview_button), "toggled",
		G_CALLBACK (lockview_button_toggled), NULL);
	g_signal_connect (G_OBJECT (srcloc_button), "clicked",
		G_CALLBACK (srcloc_button_clicked), NULL);
	g_signal_connect (G_OBJECT (destloc_button), "clicked",
		G_CALLBACK (destloc_button_clicked), NULL);
	g_signal_connect (G_OBJECT (set_button), "clicked",
		G_CALLBACK (set_button_clicked), NULL);
	g_signal_connect (G_OBJECT (reset_button), "clicked",
		G_CALLBACK (reset_button_clicked), NULL);
	g_signal_connect (G_OBJECT (close_button), "clicked",
		G_CALLBACK (close_button_clicked), NULL);

	gtk_container_add(GTK_CONTAINER(button_hbox), lockview_button);
	gtk_container_add(GTK_CONTAINER(button_hbox), srcloc_button);
	gtk_container_add(GTK_CONTAINER(button_hbox), destloc_button);
	gtk_container_add(GTK_CONTAINER(button_hbox), set_button);
	gtk_container_add(GTK_CONTAINER(button_hbox), reset_button);
	gtk_container_add(GTK_CONTAINER(button_hbox), close_button);

	gtk_container_add(GTK_CONTAINER(vbox), title);
	gtk_container_add(GTK_CONTAINER(vbox), button_hbox);

	return vbox;
}
