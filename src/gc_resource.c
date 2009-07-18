#include "colors.h"
#include "xpm_image.h"
#include "omgps.h"

GdkColor	g_base_colors[BASE_COLOR_COUNT];
GdkPixbuf*	g_base_color_pixbufs[BASE_COLOR_COUNT];
/* about how many meters each pixel represents in map */
double		g_pixel_meters[MAX_ZOOM_LEVELS];
const char*	g_base_color_names[BASE_COLOR_COUNT] = {
		COLOR_Aqua,	COLOR_Gray,	COLOR_Navy,	COLOR_Silver, COLOR_Black, COLOR_Green,
		COLOR_Olive, COLOR_Teal, COLOR_Blue, COLOR_Lime, COLOR_Purple, COLOR_White,
		COLOR_Fuchsia, COLOR_Maroon, COLOR_Red,	COLOR_Yellow };

static GdkPixbuf * create_color_pixbuf(int width, int height, GdkColor *color)
{
	GdkPixbuf *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);
	guchar *pixel = gdk_pixbuf_get_pixels(pixbuf);
	int n = width * height;

	int i;
	for (i = 0; i < n; i++) {
		pixel[0] = color->red & 0xFF;
		pixel[1] = color->green & 0xFF;
		pixel[2] = color->blue & 0xFF;
		pixel += 3;
	}
	return pixbuf;
}

static void init_gcs()
{
	g_context.drawingarea_bggc = NULL;
	g_context.pos_hacc_circle_gc = NULL;
	g_context.track_gc = NULL;
	g_context.scratch_gc = NULL;
	g_context.dlarea_gc = NULL;
	g_context.crosssign_gc = NULL;
	g_context.ruler_line_gc = NULL;
	g_context.ruler_rect_gc = NULL;
	g_context.ruler_text_gc = NULL;
	g_context.grid_line_gc = NULL;
	g_context.grid_text_gc = NULL;
	g_context.heading_gc = NULL;
}

static void unref_gcs()
{
	if (g_context.drawingarea_bggc)
		g_object_unref(g_context.drawingarea_bggc);
	if (g_context.pos_hacc_circle_gc)
		g_object_unref(g_context.pos_hacc_circle_gc);
	if (g_context.track_gc)
		g_object_unref(g_context.track_gc);
	if (g_context.scratch_gc)
		g_object_unref(g_context.scratch_gc);
	if (g_context.dlarea_gc)
		g_object_unref(g_context.dlarea_gc);
	if (g_context.crosssign_gc)
		g_object_unref(g_context.crosssign_gc);
	if (g_context.ruler_line_gc)
		g_object_unref(g_context.ruler_line_gc);
	if (g_context.ruler_text_gc)
		g_object_unref(g_context.ruler_text_gc);
	if (g_context.ruler_rect_gc)
		g_object_unref(g_context.ruler_rect_gc);
	if (g_context.grid_line_gc)
		g_object_unref(g_context.grid_line_gc);
	if (g_context.grid_text_gc)
		g_object_unref(g_context.grid_text_gc);
	if (g_context.heading_gc)
		g_object_unref(g_context.heading_gc);
	if (g_context.skymap_gc)
		g_object_unref(g_context.skymap_gc);
}

/**
 * Call in exposure event handler.
 */
static void create_gcs(GtkWidget *widget)
{
	g_context.drawingarea_bggc = widget->style->white_gc;

	GdkWindow *win = widget->window;

	g_context.pos_hacc_circle_gc = gdk_gc_new(win);
	gdk_gc_set_rgb_fg_color(g_context.pos_hacc_circle_gc, &g_base_colors[ID_COLOR_Olive]);
	gdk_gc_set_line_attributes(g_context.pos_hacc_circle_gc, 1, GDK_LINE_SOLID, GDK_CAP_ROUND,
			GDK_JOIN_ROUND);

	g_context.track_gc = gdk_gc_new(win);
	gdk_gc_set_line_attributes(g_context.track_gc, 3, GDK_LINE_SOLID, GDK_CAP_ROUND, GDK_JOIN_ROUND);

	g_context.scratch_gc = gdk_gc_new(win);
	gdk_gc_set_rgb_fg_color(g_context.scratch_gc, &g_base_colors[ID_COLOR_Teal]);
	gdk_gc_set_line_attributes(g_context.scratch_gc, 3, GDK_LINE_SOLID, GDK_CAP_ROUND,
			GDK_JOIN_ROUND);

	g_context.dlarea_gc = gdk_gc_new(win);
	gdk_gc_set_rgb_fg_color(g_context.dlarea_gc, &g_base_colors[ID_COLOR_Red]);
	gdk_gc_set_line_attributes(g_context.dlarea_gc, 3, GDK_LINE_ON_OFF_DASH, GDK_CAP_ROUND,
			GDK_JOIN_ROUND);
	gdk_gc_set_function(g_context.dlarea_gc, GDK_INVERT);

	g_context.crosssign_gc = gdk_gc_new(win);
	gdk_gc_set_rgb_fg_color(g_context.crosssign_gc, &g_base_colors[ID_COLOR_Black]);
	gdk_gc_set_line_attributes(g_context.crosssign_gc, 3, GDK_LINE_SOLID, GDK_CAP_ROUND,
			GDK_JOIN_ROUND);
	gdk_gc_set_function(g_context.crosssign_gc, GDK_INVERT);

	g_context.ruler_line_gc = gdk_gc_new(win);
	gdk_gc_set_rgb_fg_color(g_context.ruler_line_gc, &g_base_colors[ID_COLOR_White]);
	gdk_gc_set_line_attributes(g_context.ruler_line_gc, 1, GDK_LINE_SOLID, GDK_CAP_ROUND,
			GDK_JOIN_ROUND);

	g_context.ruler_text_gc = gdk_gc_new(win);
	gdk_gc_set_rgb_fg_color(g_context.ruler_text_gc, &g_base_colors[ID_COLOR_White]);

	g_context.ruler_rect_gc = gdk_gc_new(win);
	gdk_gc_set_rgb_fg_color(g_context.ruler_rect_gc, &g_base_colors[ID_COLOR_Gray]);
	gdk_gc_set_line_attributes(g_context.ruler_rect_gc, 1, GDK_LINE_SOLID, GDK_CAP_ROUND,
			GDK_JOIN_ROUND);
	gdk_gc_set_fill(g_context.ruler_rect_gc, GDK_SOLID);

	g_context.grid_line_gc = gdk_gc_new(win);
	gdk_gc_set_rgb_fg_color(g_context.grid_line_gc, &g_base_colors[ID_COLOR_Silver]);
	gdk_gc_set_line_attributes(g_context.grid_line_gc, 1, GDK_LINE_ON_OFF_DASH, GDK_CAP_ROUND,
			GDK_JOIN_ROUND);

	g_context.grid_text_gc = gdk_gc_new(win);
	gdk_gc_set_rgb_fg_color(g_context.grid_text_gc, &g_base_colors[ID_COLOR_Blue]);

	g_context.heading_gc = gdk_gc_new(win);
	gdk_gc_set_rgb_fg_color(g_context.heading_gc, &g_base_colors[ID_COLOR_Red]);
	gdk_gc_set_line_attributes(g_context.heading_gc, 5, GDK_LINE_SOLID, GDK_CAP_ROUND,
			GDK_JOIN_ROUND);
	gdk_gc_set_function(g_context.heading_gc, GDK_INVERT);

	g_context.skymap_gc = gdk_gc_new(win);
	gdk_gc_set_rgb_fg_color(g_context.skymap_gc, &g_base_colors[ID_COLOR_Red]);
	gdk_gc_set_line_attributes(g_context.skymap_gc, 1, GDK_LINE_SOLID, GDK_CAP_ROUND,
			GDK_JOIN_ROUND);
}

void drawing_init(GtkWidget *window)
{
	g_view.bg_alpha_idx = 1;
	g_view.bglayer.repo = NULL;

	g_view.tile_pixbuf = NULL;
	g_view.pixmap = NULL;
	g_view.fglayer.tile_pixbuf = NULL;
	g_view.bglayer.tile_pixbuf = NULL;

	g_view.sky_pixbuf = NULL;

	int i;
	for (i = 0; i < BASE_COLOR_COUNT; i++) {
		gdk_color_parse(g_base_color_names[i], &g_base_colors[i]);
		g_base_color_pixbufs[i] = create_color_pixbuf(64, 8, &g_base_colors[i]);
	}

	init_gcs();

	xpm_image_init(g_window->window);

	create_gcs(window);
}

void drawing_cleanup()
{
	int i;
	for (i = 0; i < GDK_COLORSPACE_RGB; i++) {
		if (g_base_color_pixbufs[i])
			g_object_unref(g_base_color_pixbufs[i]);
	}

	unref_gcs();

	xpm_image_cleanup();

	if (g_view.pixmap)
		g_object_unref(g_view.pixmap);

	if (g_view.tile_pixbuf)
		g_object_unref(g_view.tile_pixbuf);

	if (g_view.fglayer.tile_pixbuf)
		g_object_unref(g_view.fglayer.tile_pixbuf);

	if (g_view.bglayer.tile_pixbuf)
		g_object_unref(g_view.bglayer.tile_pixbuf);

	if (g_view.sky_pixbuf)
		g_object_unref(g_view.sky_pixbuf);
}
