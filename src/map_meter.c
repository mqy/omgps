#include "omgps.h"

static int acceptable_counts[] = { 1, 2, 3,	5, 6, 10, 12, 15, 20, 30, 60 };

static void draw_longitude_ruler(PangoLayout *layout, GdkDrawable *canvas, int topx, int topy,
		int width, int height, double lat)
{
	double w_meters = g_pixel_meters[g_view.fglayer.repo->zoom] * width * fabs(
			cos(lat / 180 * M_PI));

	int text_width, text_height;
	pango_layout_set_text(layout, "99999.9km", -1);
	pango_layout_get_size(layout, &text_width, &text_height);
	text_width /= PANGO_SCALE;
	text_height /= PANGO_SCALE;

	int segments = (int) ceil(1.0 * width / text_width);
	if (segments == 0)
		return;
	else if (segments > 5)
		segments = 5;
	if (text_width * segments > width)
		--segments;

	double unit_seg = floor(w_meters / segments);
	int pixels_seg = (int) floor(1.0 * width / segments);

	gdk_draw_rectangle(canvas, g_context.ruler_rect_gc, TRUE, topx, topy, width, height);

	int w, h;
	double seg;
	int i;
	char buf[32];
	int pixel_offset = topx;

	for (i = 1; i <= segments; i++) {
		seg = unit_seg * i;
		if (seg >= 1000) {
			seg /= 1000;
			snprintf(buf, sizeof(buf), "%.1lfkm", seg);
		} else {
			snprintf(buf, sizeof(buf), "%dm", (int) seg);
		}

		pango_layout_set_text(layout, buf, -1);
		pango_layout_get_size(layout, &w, &h);
		w /= PANGO_SCALE;

		pixel_offset += pixels_seg;

		gdk_draw_layout(canvas, g_context.ruler_text_gc, pixel_offset - w - 3, topy, layout);
		gdk_draw_line(canvas, g_context.ruler_line_gc, pixel_offset, topy, pixel_offset, topy
				+ height);
	}
}

static void draw_latitude_lines(PangoLayout *layout, GdkDrawable *canvas, double tl_lat,
		double br_lat, int region_topx, int region_topy, int region_width, int region_height)
{
	int text_width, text_height;
	pango_layout_set_text(layout, "-90", -1);
	pango_layout_get_size(layout, &text_width, &text_height);
	text_width /= PANGO_SCALE;
	text_height /= PANGO_SCALE;

	/* latitude: the bigger the smaller y-coordinate value in view */
	int lat_1 = (int) ceil(tl_lat + 1);
	int lat_2 = (int) floor(br_lat - 1);
	if (lat_1 > 90)
		lat_1 = 90;
	if (lat_2 < -90)
		lat_2 = -90;

	int lat_diff = lat_1 - lat_2;
	if (lat_diff < 0)
		return;

	int half_height = text_height >> 1;
	int line_topy = region_topy + text_height;
	int line_boty = region_topy + region_height - half_height;
	int min_dist = text_height << 1;
	int dist;

	int lat, i, y, h;
	/* last drawn latitude y-coordinate in view */
	int last_y = 0;
	coord_t wgs84;
	point_t pt;
	char buf[20];
	int right_x = region_topx + region_width;
	int j, n;
	double last_lat = 0, delta;
	int minutes;
	gboolean degree_drawn = FALSE;

	for (i = 0; i <= lat_diff; i++) {
		lat = lat_2 + i;
		wgs84.lat = lat;
		wgs84.lon = 0;
		pt = wgs84_to_tilepixel(wgs84, g_view.fglayer.repo->zoom, g_view.fglayer.repo);
		y = pt.y - g_view.fglayer.tl_pixel.y;
		if (i == 0) {
			last_y = y;
			last_lat = lat;
			continue;
		}

		dist = last_y - y;

		if (dist < min_dist)
			continue;

		/* now, we can draw at least one line, try dividing the range into minutes */

		n = 1;
		for (j = 0; j < sizeof(acceptable_counts) / sizeof(int); j++) {
			if (min_dist * acceptable_counts[j] > dist)
				break;
			n = acceptable_counts[j];
		}

		minutes = 60 / n;
		delta = 1.0 * (lat - last_lat) / n;
		last_y = y;
		last_lat = lat;
		--n;

		gboolean is_degree = FALSE;

		for (j = 0; j <= n; j++) {
			wgs84.lat = lat + j * delta;
			wgs84.lon = 0;
			pt = wgs84_to_tilepixel(wgs84, g_view.fglayer.repo->zoom, g_view.fglayer.repo);
			y = pt.y - g_view.fglayer.tl_pixel.y;
			if (y >= line_topy && y <= line_boty) {
				gdk_draw_line(canvas, g_context.grid_line_gc, region_topx, y, right_x, y);
				if (j == 0) {
					snprintf(buf, sizeof(buf), "%02d°", lat);
					is_degree = degree_drawn = TRUE;
				} else {
					if (degree_drawn)
						snprintf(buf, sizeof(buf), "%02d'", j * minutes);
					else {
						snprintf(buf, sizeof(buf), "%02d°%02d'", lat, j * minutes);
						degree_drawn = TRUE;
					}
					is_degree = FALSE;
				}
				pango_layout_set_text(layout, buf, -1);
				pango_layout_get_size(layout, &text_width, &h);
				text_width /= PANGO_SCALE;
				gdk_draw_line(canvas, g_context.grid_line_gc, region_topx, y, right_x, y);
				y -= half_height;
				gdk_draw_rectangle(canvas, g_context.ruler_rect_gc, TRUE, region_topx, y,
						text_width + 1, text_height);
				gdk_draw_layout(canvas, (is_degree
						? g_context.grid_text_gc
						: g_context.ruler_text_gc), region_topx, y, layout);
			}
		}
	}
}

static void draw_longitude_lines(PangoLayout *layout, GdkDrawable *canvas, double tl_lon,
		double br_lon, int region_topx, int region_topy, int region_width, int region_height)
{
	int text_width, text_height;
	pango_layout_set_text(layout, "-180°55'", -1);
	pango_layout_get_size(layout, &text_width, &text_height);
	text_width /= PANGO_SCALE;
	text_height /= PANGO_SCALE;

	int lon_1 = (int) (floor(tl_lon - 1));
	int lon_2 = (int) (ceil(br_lon + 1));
	if (lon_1 < -180)
		lon_1 = -180;
	if (lon_2 > 180)
		lon_2 = 180;

	int lon_diff = lon_2 - lon_1;

	if (lon_diff < 0)
		return;

	int min_dist = text_width + 2;
	int lon, i, x;
	int line_leftx = region_topx + (text_width >> 1);
	int line_rightx = region_topx + region_width - (text_width >> 1);
	int last_x = 0;
	coord_t wgs84;
	point_t pt;
	char buf[20];
	int bot_y = region_topy + region_height, h;
	int dist;
	int j, n;
	double last_lon = 0, delta;
	int minutes;
	gboolean degree_drawn = FALSE;

	for (i = 0; i <= lon_diff; i++) {
		lon = lon_1 + i;
		wgs84.lat = 0;
		wgs84.lon = lon;
		pt = wgs84_to_tilepixel(wgs84, g_view.fglayer.repo->zoom, g_view.fglayer.repo);
		x = pt.x - g_view.fglayer.tl_pixel.x;
		if (i == 0) {
			last_x = x;
			last_lon = lon;
			continue;
		}

		dist = x - last_x;
		if (dist < min_dist)
			continue;

		/* now, we can draw at least one line, try dividing the range into minutes */

		n = 1;
		for (j = 0; j < sizeof(acceptable_counts) / sizeof(int); j++) {
			if (min_dist * acceptable_counts[j] > dist)
				break;
			n = acceptable_counts[j];
		}

		minutes = 60 / n;
		delta = 1.0 * (lon - last_lon) / n;
		last_x = x;
		last_lon = lon;
		--n;

		gboolean is_degree = FALSE;

		for (j = 0; j <= n; j++) {
			wgs84.lat = 0;
			wgs84.lon = lon + j * delta;
			pt = wgs84_to_tilepixel(wgs84, g_view.fglayer.repo->zoom, g_view.fglayer.repo);
			x = pt.x - g_view.fglayer.tl_pixel.x;
			if (x >= line_leftx && x <= line_rightx) {
				gdk_draw_line(canvas, g_context.grid_line_gc, x, region_topy, x, bot_y);
				if (j == 0) {
					snprintf(buf, sizeof(buf), "%02d°", lon + (j == 0 ? 0 : 1));
					is_degree = degree_drawn = TRUE;
				} else {
					if (degree_drawn)
						snprintf(buf, sizeof(buf), "%02d'", j * minutes);
					else {
						snprintf(buf, sizeof(buf), "%02d°%02d'", lon, j * minutes);
						degree_drawn = TRUE;
					}
					is_degree = FALSE;
				}
				pango_layout_set_text(layout, buf, -1);
				pango_layout_get_size(layout, &text_width, &h);
				text_width /= PANGO_SCALE;
				x -= text_width >> 1;
				gdk_draw_rectangle(canvas, g_context.ruler_rect_gc, TRUE, x, region_topy,
						text_width + 1, text_height);
				gdk_draw_layout(canvas, (is_degree
						? g_context.grid_text_gc
						: g_context.ruler_text_gc), x, region_topy, layout);
			}
		}
	}
}

/**
 * return font height
 */
static int draw_rulers(GdkDrawable *canvas, PangoLayout *layout)
{
	PangoFontDescription *desc = pango_font_description_from_string("Monospace 14px");
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);

	/* Get font height */
	pango_layout_set_text(layout, "-88", -1);
	int width, height;
	pango_layout_get_size(layout, &width, &height);
	height /= PANGO_SCALE;

	point_t pt = g_view.fglayer.tl_pixel;
	pt.x += g_view.fglayer.visible.x;
	pt.y += g_view.fglayer.visible.y;

	map_repo_t *repo = g_view.fglayer.repo;

	coord_t tl_wgs84 = tilepixel_to_wgs84(pt, repo->zoom, repo);

	/* bottom ruler */
	pt.x += g_view.fglayer.visible.width;
	pt.y += g_view.fglayer.visible.height;
	coord_t br_wgs84 = tilepixel_to_wgs84(pt, repo->zoom, repo);

	int topx = g_view.fglayer.visible.x;
	int topy = g_view.fglayer.visible.y;
	int canvas_width = g_view.fglayer.visible.width;
	int canvas_height = g_view.fglayer.visible.height;

	draw_longitude_ruler(layout, canvas, topx, topy, canvas_width, height, tl_wgs84.lat);

	draw_longitude_ruler(layout, canvas, topx, topy + canvas_height - height, canvas_width, height,
			br_wgs84.lat);

	return height;
}

static void draw_latlon_grid(GdkDrawable *canvas, PangoLayout *layout, int ruler_height)
{
	PangoFontDescription *desc = pango_font_description_from_string("Monospace 14px");
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);

	point_t pt = g_view.fglayer.tl_pixel;
	pt.x += g_view.fglayer.visible.x;
	pt.y += g_view.fglayer.visible.y;
	coord_t tl_wgs84 = tilepixel_to_wgs84(pt, g_view.fglayer.repo->zoom, g_view.fglayer.repo);

	/* bottom ruler */
	pt.x += g_view.fglayer.visible.width;
	pt.y += g_view.fglayer.visible.height;
	coord_t br_wgs84 = tilepixel_to_wgs84(pt, g_view.fglayer.repo->zoom, g_view.fglayer.repo);

	int topx = g_view.fglayer.visible.x;
	int topy = g_view.fglayer.visible.y;
	int canvas_width = g_view.fglayer.visible.width;
	int canvas_height = g_view.fglayer.visible.height;

	/* longitude lines */
	draw_longitude_lines(layout, canvas, tl_wgs84.lon, br_wgs84.lon, topx, topy + ruler_height,
			canvas_width, canvas_height - (ruler_height << 1));

	/* latitude lines */
	draw_latitude_lines(layout, canvas, tl_wgs84.lat, br_wgs84.lat, topx, topy + ruler_height,
			canvas_width, canvas_height - (ruler_height << 1));
}

void draw_map_meter(GdkDrawable *canvas)
{
	PangoContext *context = gtk_widget_create_pango_context(g_view.da);
	PangoLayout *layout = pango_layout_new(context);
	int ruler_height = 0;

	if (g_context.show_rulers)
		ruler_height = draw_rulers(canvas, layout);

	if (g_context.show_latlon_grid)
		draw_latlon_grid(canvas, layout, ruler_height);

	g_object_unref(layout);
	g_object_unref(context);
}
