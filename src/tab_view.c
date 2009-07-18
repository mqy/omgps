#include <pthread.h>
#include <signal.h>

#include "util.h"
#include "omgps.h"
#include "tile.h"
#include "network.h"
#include "customized.h"
#include "track.h"
#include "gps.h"

static GtkWidget *drawingarea;
static GtkWidget *menu_button, *center_button, *zoomin_button, *zoomout_button, *fullscreen_button;
static GtkWidget *zoom_label, *status_label;
static PangoLayout *tile_info_text_layout = NULL;

static mouse_handler_t mouse_handler;
static point_t clicked_point = {-1, -1};;
static guint clicked_time;

static pthread_mutex_t change_zoom_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t change_zoom_cond = PTHREAD_COND_INITIALIZER;
static pthread_t change_zoom_thread_tid = 0;
static gboolean stop = FALSE;

/* see macro MAX_ZOOM_LEVELS */
static char zoom_label_textarray [MAX_ZOOM_LEVELS][3];

/* map overlay, alpha-pixel RGB value cache */
static guchar *pixel_rgb_table = NULL;

static int screen_w = 0, screen_h = 0;

static U4 drawingarea_event_masks =
	GDK_BUTTON_PRESS_MASK |
	GDK_BUTTON_RELEASE_MASK |
	GDK_POINTER_MOTION_MASK |
	GDK_POINTER_MOTION_HINT_MASK |
	GDK_LEAVE_NOTIFY_MASK |
	GDK_EXPOSURE_MASK;

static void map_init_zoom();
static void* change_zoom_routine(void *args);
static void center_button_clicked(GtkWidget *widget, gpointer data);
static void map_redraw_view_default();
static void map_invalidate_pixbuf(GdkRectangle *area,
	gboolean update_fg, gboolean update_bg, gboolean dl_if_absent);

static map_redraw_view_func_t map_redraw_view_func = map_redraw_view_default;

#define PIXBUF_DIM \
	g_view.fglayer.visible.x, g_view.fglayer.visible.y,	\
	g_view.fglayer.visible.x, g_view.fglayer.visible.y,	\
	g_view.fglayer.visible.width, g_view.fglayer.visible.height

#define DRAW_PIXBUF(drawable, pixbuf) 					\
	gdk_draw_pixbuf (drawable, 							\
	g_context.drawingarea_bggc, pixbuf, PIXBUF_DIM,		\
	GDK_RGB_DITHER_NORMAL, -1, -1)

#define DRAW_BG(layer) \
	if (layer.visible.width < g_view.width || 			\
		layer.visible.height < g_view.height) {			\
		gdk_draw_rectangle (drawingarea->window, 		\
			g_context.drawingarea_bggc,					\
			TRUE, 0, 0, g_view.width, g_view.height);	\
	}

void map_set_redraw_func(map_redraw_view_func_t func)
{
	if (func == NULL) {
		map_redraw_view_func = (POLL_STATE_TEST(RUNNING))?
			map_redraw_view_gps_running : map_redraw_view_default;
	} else {
		map_redraw_view_func = func;
	}
}

void map_cleanup()
{
	stop = TRUE;

	if (change_zoom_thread_tid > 0) {
		LOCK_MUTEX(&change_zoom_lock);
		pthread_cond_signal(&change_zoom_cond);
		UNLOCK_MUTEX(&change_zoom_lock);
		sleep_ms(100);
		pthread_kill(change_zoom_thread_tid, SIGUSR1);
		pthread_join(change_zoom_thread_tid, NULL);
		change_zoom_thread_tid = 0;
	}

	if (tile_info_text_layout)
		g_object_unref(tile_info_text_layout);

	tilecache_cleanup(g_view.fglayer.tile_cache, TRUE);
	g_view.fglayer.tile_cache = NULL;

	tilecache_cleanup(g_view.bglayer.tile_cache, TRUE);
	g_view.bglayer.tile_cache = NULL;
}

/**
 * Redraw tile-pixbuf and meter back.
 */
inline void map_draw_back_layers(GdkDrawable *canvas)
{
	GdkPixbuf *pixbuf;
	if (g_view.bglayer.repo && g_view.tile_pixbuf_valid)
		pixbuf = g_view.tile_pixbuf;
	else
		pixbuf = g_view.fglayer.tile_pixbuf;

	DRAW_PIXBUF (canvas, pixbuf);
	if (g_context.show_rulers || g_context.show_latlon_grid)
		draw_map_meter(canvas);
}

static void map_redraw_view_default()
{
	map_draw_back_layers(drawingarea->window);
}

void map_redraw_view()
{
	(*map_redraw_view_func)();
}

static gboolean map_update_view_range(map_view_tile_layer_t *tile_layer)
{
	int max_tile_no = (1 << tile_layer->repo->zoom);
	int max_pixel = (max_tile_no + 1) * TILE_SIZE - 1;

	/* get view and tiles in tile pixel coordinate */
	int view_tl_pixel_x = tile_layer->center_pixel.x - (g_view.width >> 1);
	int view_tl_pixel_y = tile_layer->center_pixel.y - (g_view.height >> 1);

	int view_br_pixel_x = view_tl_pixel_x + g_view.width;
	int view_br_pixel_y = view_tl_pixel_y + g_view.height;

	GdkRectangle world = {0, 0, max_pixel, max_pixel};
	GdkRectangle view_rect = { view_tl_pixel_x,  view_tl_pixel_y, view_br_pixel_x, view_br_pixel_y};
	if (! gdk_rectangle_intersect(&world, &view_rect, NULL)) {
		return FALSE;
	}

	int view_tl_tile_x = (int)floor(1.0 * view_tl_pixel_x / TILE_SIZE);
	int view_tl_tile_y = (int)floor(1.0 * view_tl_pixel_y / TILE_SIZE);

	int view_br_tile_x = (int)floor(1.0 * view_br_pixel_x / TILE_SIZE);
	int view_br_tile_y = (int)floor(1.0 * view_br_pixel_y / TILE_SIZE);

	if (view_tl_tile_x < 0)
		view_tl_tile_x = 0;
	if (view_tl_tile_y < 0)
		view_tl_tile_y = 0;
	if (view_br_tile_x >= max_tile_no)
		view_br_tile_x = max_tile_no - 1;
	if (view_br_tile_y >= max_tile_no)
		view_br_tile_y = max_tile_no - 1;

	/* least tile rows and columns too fill current view */
	int rows = view_br_tile_y - view_tl_tile_y + 1;
	int cols = view_br_tile_x - view_tl_tile_x + 1;

	/* coordinates of drawing window in tile coordinate */
	tile_layer->tl_pixel.x = view_tl_pixel_x;
	tile_layer->tl_pixel.y = view_tl_pixel_y;
	tile_layer->br_pixel.x = view_br_pixel_x;
	tile_layer->br_pixel.y = view_br_pixel_y;

	/* tiles surrounding window in tile coordinate */
	tile_layer->tile_rows = rows;
	tile_layer->tile_cols = cols;
	tile_layer->tl_tile.x = view_tl_tile_x;
	tile_layer->tl_tile.y = view_tl_tile_y;
	tile_layer->br_tile.x = view_br_tile_x;
	tile_layer->br_tile.y = view_br_tile_y;

	return TRUE;
}

/**
 * NOTE: caller must not hold download lock when call this function!
 */
void map_front_download_callback_func(map_repo_t *repo, int zoom, int x, int y)
{
	LOCK_UI();

	if (g_tab_id != TAB_ID_MAIN_VIEW)
		goto END;

	GdkRectangle tile_rect = {0, 0, TILE_SIZE, TILE_SIZE};
	map_view_tile_layer_t *layer = NULL;
	GdkRectangle area;
	gboolean update_fg = FALSE;
	gboolean update_bg = FALSE;

	if ((repo == g_view.fglayer.repo)) {
		layer = &g_view.fglayer;
		update_fg = TRUE;
	} else if (repo == g_view.bglayer.repo) {
		layer = &g_view.bglayer;
		update_bg = TRUE;
	}

	if (layer && zoom == layer->repo->zoom) {
		tile_rect.x = x * TILE_SIZE - layer->tl_pixel.x;
		tile_rect.y = y * TILE_SIZE - layer->tl_pixel.y;
		GdkRectangle view_rect = { 0,  0, g_view.width, g_view.height};

		/* NOTE: intersect with fg layer */
		if (gdk_rectangle_intersect(&tile_rect, &view_rect, &area)) {
			map_invalidate_pixbuf(&area, update_fg, update_bg, FALSE);
			(*map_redraw_view_func)();
		}
	}

END:

	UNLOCK_UI();
}

static tile_t * get_tile(tilecache_t *tile_cache, map_repo_t *repo, int tx, int ty, gboolean dl_if_absent)
{
	tile_t *tile = NULL;

	tile = tilecache_get(tile_cache, repo->zoom, tx, ty);
	if (tile != NULL)
		return tile;

	/* file path */
	char buf[256];
	struct stat st;

	/* this also create tile path */
	if (! format_tile_file_path(repo, repo->zoom, tx, ty, buf, sizeof(buf)))
		return NULL;

	if (stat(buf, &st) != 0) {
		if (dl_if_absent && count_network_interfaces() > 0) {
			/* SPECIAL NOTE: also synchronize access to Python interpreter! */
			char * url = mapcfg_get_dl_url(repo, repo->zoom, tx, ty);
			if (! url) {
				log_warn("download tile: can't get url for map: %s", repo->name);
				return NULL;
			}
			add_front_download_task(repo, repo->zoom, tx, ty, strdup(buf), url);
		}
	} else {
		//log_debug("file: %s", buf);
		GError *error = NULL;
		GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(buf, &error);
		if (pixbuf) {
			tile = (tile_t*) malloc(sizeof(tile_t));
			if (tile == NULL) {
				log_warn("allocate memory for tile_t failed\n");
				return NULL;
			}

			tile->cached = FALSE;
			tile->zoom = repo->zoom;
			tile->x = tx;
			tile->y = ty;
			tile->pixbuf = pixbuf;

			if (! tilecache_add(tile_cache, tile))
				log_warn("add tile to cache failed.");
		} else {
			log_warn("load image failed: %s\n", error->message);
			g_error_free(error);
		}
	}

	return tile;
}

static void map_update_tile_pixbuf(map_view_tile_layer_t *layer, gboolean is_fg, gboolean dl_if_absent)
{
	map_repo_t *repo = layer->repo;

	int i, j, src_x, src_y, dest_x, dest_y;
	int ts = TILE_SIZE;

	GdkRectangle view_rect = { 0,  0, g_view.width, g_view.height};

	GdkRectangle tile_rect, tile_draw_rect;
	tile_rect.width = tile_rect.height = ts;

	tile_t *tile;

	int offset_x = layer->tl_tile.x * ts - layer->tl_pixel.x;
	int offset_y = layer->tl_tile.y * ts - layer->tl_pixel.y;

	gboolean tl_set = FALSE;

	GdkGC *gc = g_context.drawingarea_bggc;

	/* pixels relative to window top left */
	layer->visible.x = layer->visible.y = 0;
	point_t br = {0, 0};

	for (i=0; i<layer->tile_rows; i++) { /* row */
		for (j=0; j<layer->tile_cols; j++) { /* col */
			tile = get_tile(layer->tile_cache, repo,
				layer->tl_tile.x + j, layer->tl_tile.y + i, dl_if_absent);

			tile_rect.x = offset_x + j * ts;
			tile_rect.y = offset_y + i * ts;

			if (! gdk_rectangle_intersect(&view_rect, &tile_rect, &tile_draw_rect))
				continue;

			src_x = tile_draw_rect.x - tile_rect.x;
			src_y = tile_draw_rect.y - tile_rect.y;
			dest_x = tile_draw_rect.x;
			dest_y = tile_draw_rect.y;

			if (! tl_set) {
				layer->visible.x = dest_x;
				layer->visible.y = dest_y;
				tl_set = TRUE;
			}

			br.x = dest_x + tile_draw_rect.width;
			br.y = dest_y + tile_draw_rect.height;

			if (tile) {
#if (1)
				gdk_draw_pixbuf (g_view.pixmap, g_context.drawingarea_bggc, tile->pixbuf,
					src_x, src_y, dest_x, dest_y, tile_draw_rect.width, tile_draw_rect.height,
					GDK_RGB_DITHER_NONE, -1, -1);
#else
				guchar *pixel = gdk_pixbuf_get_pixels (tile->pixbuf) +
					(src_y * gdk_pixbuf_get_rowstride (tile->pixbuf)) +
					(src_x * gdk_pixbuf_get_n_channels (tile->pixbuf));

				if (! gdk_pixbuf_get_has_alpha (tile->pixbuf)) {
					gdk_draw_rgb_image (g_view.pixmap, g_context.drawingarea_bggc,
						dest_x, dest_y, tile_draw_rect.width, tile_draw_rect.height,
			            GDK_RGB_DITHER_NORMAL, pixel, gdk_pixbuf_get_rowstride (tile->pixbuf));
				} else {
					gdk_draw_rgb_32_image (g_view.pixmap, g_context.drawingarea_bggc,
						dest_x, dest_y, tile_draw_rect.width, tile_draw_rect.height,
						GDK_RGB_DITHER_MAX, pixel, gdk_pixbuf_get_rowstride (tile->pixbuf));
				}
#endif
				if (! tile->cached) {
					g_object_unref(tile->pixbuf);
					tile->pixbuf = NULL;
					free(tile);
					tile = NULL;
				}
			} else {
				gdk_draw_rectangle (g_view.pixmap, gc, TRUE, dest_x, dest_y,
					tile_draw_rect.width, tile_draw_rect.height);
			}
		}
	}

	if (br.x > layer->visible.x && br.y > layer->visible.y) {
		/* coordinates of tile area in window coordinate */
		layer->visible.width = br.x - layer->visible.x;
		layer->visible.height = br.y - layer->visible.y;

		/* left top corner */
		gdk_pixbuf_get_from_drawable (layer->tile_pixbuf, g_view.pixmap, NULL,
			layer->visible.x, layer->visible.y,	layer->visible.x, layer->visible.y,
			layer->visible.width, layer->visible.height);
	} else {
		layer->visible.width = 0;
		layer->visible.height = 0;
		log_warn("invalid view range: 0 size");
	}
}

/**
 * compose to tile_pixbuf
 */
static void map_overlay_alpha_blending(GdkRectangle *bg_rect)
{
	map_view_tile_layer_t *fg = &g_view.fglayer;
	map_view_tile_layer_t *bg = &g_view.bglayer;

	guchar *pixels = gdk_pixbuf_get_pixels (g_view.tile_pixbuf);
	guchar *pixels_fg = gdk_pixbuf_get_pixels (fg->tile_pixbuf);
	guchar *pixels_bg = gdk_pixbuf_get_pixels (bg->tile_pixbuf);

	GdkRectangle rect;

	if (! gdk_rectangle_intersect(bg_rect, &fg->visible, &rect)) {
		log_debug("no overlay");
		return;
	}

	int x = MAX(bg_rect->x, rect.x);
	int y = MAX(bg_rect->y, rect.y);
	int w = MIN(bg_rect->width, rect.width);
	int h = MIN(bg_rect->height, rect.height);

	/* bg alpha index */
	int bidx = g_view.bg_alpha_idx;
	/* fg alpha index */
	int fidx = ALPHA_LEVELS - bidx - 1;

	int i, j, k, off_0, off_1, off_2;

	if (pixel_rgb_table == NULL) {
		pixel_rgb_table = (guchar *) malloc(ALPHA_LEVELS * 256);
		if (! pixel_rgb_table) {
			warn_dialog("Unable to allocate memory, exit");
			exit(0);
		}
		float *alpha_values = get_alpha_values();
		for (i=0; i<ALPHA_LEVELS; i++) {
			k = i << 8;
			for (j=0; j<=255; j++) {
				pixel_rgb_table[k + j] = (guchar)(floor(alpha_values[i] * j));
			}
		}
	}

	guchar *fpixel = &pixel_rgb_table[fidx << 8];
	guchar *bpixel = &pixel_rgb_table[bidx << 8];

	int max_x = x + w;
	int max_y = y + h;

	for (i=y; i<max_y; i++) {
		for (j=x; j<max_x; j++) {
			off_0 = (i * g_view.width + j) * 3;
			off_1 = off_0 + 1;
			off_2 = off_0 + 2;

			/* FIXME: assume 8-bit color? */
			pixels[off_0] = fpixel[pixels_fg[off_0]] + bpixel[pixels_bg[off_0]];
			pixels[off_1] = fpixel[pixels_fg[off_1]] + bpixel[pixels_bg[off_1]];
			pixels[off_2] = fpixel[pixels_fg[off_2]] + bpixel[pixels_bg[off_2]];
		}
	}
}

#define TEST_ALPHA_BLENDING(zoom)						\
	(g_view.bglayer.repo && 						\
		(g_view.bglayer.repo->max_zoom >= zoom && 	\
		g_view.bglayer.repo->min_zoom <= zoom))

static void map_invalidate_pixbuf(GdkRectangle *area, gboolean update_fg,
	gboolean update_bg, gboolean dl_if_absent)
{
	map_view_tile_layer_t *fg = &g_view.fglayer;

	/* If out of range don't display bg map */
	gboolean alpha_blending = TEST_ALPHA_BLENDING(fg->repo->zoom);

	if (update_fg)
		map_update_tile_pixbuf(fg, TRUE, dl_if_absent);

	if (alpha_blending) {

		if (! g_view.tile_pixbuf) {
			g_view.tile_pixbuf = gdk_pixbuf_get_from_drawable (NULL, g_view.pixmap,
				gdk_rgb_get_colormap(), 0, 0, 0, 0, screen_w, screen_h);
		}

		if (! g_view.bglayer.tile_pixbuf) {
			g_view.bglayer.tile_pixbuf = gdk_pixbuf_get_from_drawable (NULL, g_view.pixmap,
				gdk_rgb_get_colormap(), 0, 0, 0, 0, screen_w, screen_h);
		}

		map_view_tile_layer_t *bg = &g_view.bglayer;
		bg->repo->zoom = g_view.fglayer.repo->zoom;
		bg->center_pixel = wgs84_to_tilepixel(g_view.center_wgs84, bg->repo->zoom, bg->repo);

		if (map_update_view_range(bg)) {
			if (update_bg || ! g_view.tile_pixbuf_valid) {
				map_update_tile_pixbuf(bg, FALSE, dl_if_absent);
				g_view.tile_pixbuf_valid = TRUE;
			}
			map_overlay_alpha_blending(area? area: &fg->visible);
		} else {
			g_view.tile_pixbuf_valid = FALSE;
		}
	} else {
		if (g_view.tile_pixbuf) {
			g_object_unref(g_view.tile_pixbuf);
			g_object_unref(g_view.bglayer.tile_pixbuf);
			g_view.tile_pixbuf = NULL;
			g_view.bglayer.tile_pixbuf = NULL;
		}
		g_view.tile_pixbuf_valid = FALSE;
	}
}
/**
 * Call this function when the backend tile_pixmap needs to be re-constructed.
 */
void map_invalidate_view(gboolean redraw)
{
	if (map_update_view_range(&g_view.fglayer)) {
		map_invalidate_pixbuf(NULL, TRUE, TRUE, g_context.dl_if_absent);
		/* For "keep cursor in view" */
		poll_ui_on_view_range_changed();
	} else {
		gdk_draw_rectangle (drawingarea->window, g_context.drawingarea_bggc,
			TRUE, 0, 0, g_view.width, g_view.height);
	}

	if (redraw) {
		DRAW_BG(g_view.fglayer);
		(*map_redraw_view_func)();
	}
}

void map_zoom_to(int zoom, coord_t center_wgs84, gboolean redraw)
{
	gtk_widget_set_sensitive(zoomin_button, zoom < g_view.fglayer.repo->max_zoom);
	gtk_widget_set_sensitive(zoomout_button, zoom > g_view.fglayer.repo->min_zoom);

	g_view.fglayer.repo->zoom = zoom;

	g_view.fglayer.center_pixel = wgs84_to_tilepixel(center_wgs84,
		g_view.fglayer.repo->zoom, g_view.fglayer.repo);
	g_view.center_wgs84 = center_wgs84;
	g_view.pos_offset.x = g_view.width >> 1;
	g_view.pos_offset.y = g_view.height >> 1;

	update_ui_on_zoom_level_changed();

	gtk_label_set_text(GTK_LABEL(zoom_label), zoom_label_textarray[zoom]);

	/* invalidate view */
	map_invalidate_view(redraw);
}

static void* change_zoom_routine(void *args)
{
	gboolean is_zoom_in = (gboolean)args;
	stop = FALSE;

	int hi = (is_zoom_in)?
		g_view.fglayer.repo->max_zoom - g_view.fglayer.repo->zoom :
		g_view.fglayer.repo->zoom - g_view.fglayer.repo->min_zoom;
	int zoom = g_view.fglayer.repo->zoom;
	int i;

	for (i=1; !stop && i<=hi; i++) {
		LOCK_UI();
		zoom = is_zoom_in? g_view.fglayer.repo->zoom + i : g_view.fglayer.repo->zoom - i;
		gtk_label_set_text(GTK_LABEL(zoom_label), zoom_label_textarray[zoom]);
		UNLOCK_UI();
		sleep_ms(300);
	}

	int multiply = 1 << abs(zoom - g_view.fglayer.repo->zoom);
	if (multiply != 1) {
		LOCK_UI();
		map_zoom_to(zoom, g_view.center_wgs84, TRUE);
		UNLOCK_UI();
	}

	change_zoom_thread_tid = 0;

	return NULL;
}

static void zoom_button_pressed(GtkWidget *widget, gpointer data)
{
	if (change_zoom_thread_tid == 0) {
		if (pthread_create(&change_zoom_thread_tid, NULL, change_zoom_routine, data) != 0) {
			warn_dialog("can not create thread for zoom in/out");
		}
	}
}

static void zoom_button_released(GtkWidget *widget, gpointer data)
{
	stop = TRUE;
}

static gboolean drawing_area_configure_event (GtkWidget *widget, GdkEventConfigure *evt, gpointer data)
{
	static int w = 0, h = 0;

	g_view.width = evt->width;
	g_view.height = evt->height;

	/* create with screen display size to avoid frequently create/destroy.
	 * User may change display orientation */
	GdkScreen *screen = gdk_screen_get_default();
	screen_w = gdk_screen_get_width(screen);
	screen_h = gdk_screen_get_height(screen);

	if (w != screen_w || h != screen_h) {

		w = screen_w;
		h = screen_h;

		if (g_view.pixmap) {
			g_object_unref (g_view.pixmap);
			g_view.pixmap = NULL;
		}
		g_view.pixmap = gdk_pixmap_new (drawingarea->window, w, h, -1);

		if (g_view.fglayer.tile_pixbuf != NULL) {
			g_object_unref(g_view.fglayer.tile_pixbuf);
			g_view.fglayer.tile_pixbuf = NULL;
		}
		g_view.fglayer.tile_pixbuf = gdk_pixbuf_get_from_drawable (NULL, g_view.pixmap,
			gdk_rgb_get_colormap(), 0, 0, 0, 0, w, h);

		/* alpha blending related */

		if (g_view.tile_pixbuf) {
			g_object_unref(g_view.tile_pixbuf);
			g_view.tile_pixbuf = NULL;
		}

		if (g_view.bglayer.tile_pixbuf) {
			g_object_unref(g_view.bglayer.tile_pixbuf);
			g_view.bglayer.tile_pixbuf = NULL;
		}

		if (g_view.bglayer.repo) {
			g_view.tile_pixbuf = gdk_pixbuf_get_from_drawable (NULL, g_view.pixmap,
				gdk_rgb_get_colormap(), 0, 0, 0, 0, w, h);

			g_view.bglayer.tile_pixbuf = gdk_pixbuf_get_from_drawable (NULL, g_view.pixmap,
				gdk_rgb_get_colormap(), 0, 0, 0, 0, w, h);
		}

		g_view.tile_pixbuf_valid = FALSE;
	}

	map_invalidate_view(TRUE);

	return FALSE;
}

static gboolean drawing_area_expose_event (GtkWidget *widget, GdkEventExpose *evt, gpointer data)
{
	if (g_view.invalidate) {
		g_view.invalidate = FALSE;
		int zoom = g_view.fglayer.repo->zoom;
		if (g_view.bglayer.repo)
			zoom = MIN(zoom, g_view.bglayer.repo->max_zoom);
		map_zoom_to(zoom, g_view.center_wgs84, TRUE);
	} else {
		(*map_redraw_view_func)();
	}
	return FALSE;
}

void map_centralize()
{
	g_context.cursor_in_view = TRUE;
	/* NOTE: see also view_tab_create() */
	modify_button_color(GTK_BUTTON(center_button), &g_base_colors[ID_COLOR_Red], TRUE);

	if (ctx_tab_get_current_id() == CTX_ID_TRACK_REPLAY && g_context.map_view_frozen == TRUE) {
		track_replay_centralize();
	} else {
		if ((g_view.pos_offset.x == (g_view.width >> 1)) && g_view.pos_offset.y == (g_view.height >> 1))
			return;

		g_view.fglayer.center_pixel = wgs84_to_tilepixel(g_view.pos_wgs84,
			g_view.fglayer.repo->zoom, g_view.fglayer.repo);

		g_view.center_wgs84 = g_view.pos_wgs84;
		g_view.pos_offset.x = g_view.width >> 1;
		g_view.pos_offset.y = g_view.height >> 1;

		map_invalidate_view(TRUE);
	}
}

static void center_button_clicked(GtkWidget *widget, gpointer data)
{
	map_centralize();
}

static void exit_button_clicked(GtkWidget *widget, gpointer data)
{
	main_exit();
}

void toggle_fullscreen(gboolean full)
{
	if (full == g_context.fullscreen)
		return;

	if (full) {
		gtk_window_fullscreen(GTK_WINDOW(g_window));
	} else {
		gtk_window_unfullscreen(GTK_WINDOW(g_window));
	}
	g_context.fullscreen = full;
}

static void fullscreen_button_clicked(GtkWidget *widget, gpointer data)
{
	gtk_button_set_label(GTK_BUTTON(fullscreen_button), g_context.fullscreen? "Full" : "Unfull");
	toggle_fullscreen(! g_context.fullscreen);
}

/**
 * The following functions are not present in arm-angstrom-linux-gnueabi (16-Sep-2008):
 * gtk_adjustment_configure(), gtk_adjustment_set_lower(), gtk_adjustment_set_upper()
 * and gtk_scale_button_set_orintation(). */
static void map_init_zoom()
{
	gtk_label_set_text(GTK_LABEL(zoom_label), zoom_label_textarray[g_view.fglayer.repo->zoom]);
	gtk_widget_set_sensitive(zoomin_button, TRUE);
	gtk_widget_set_sensitive(zoomout_button, TRUE);
	if (g_view.fglayer.repo->zoom == g_view.fglayer.repo->max_zoom)
		gtk_widget_set_sensitive(zoomin_button, FALSE);
	else if (g_view.fglayer.repo->zoom == g_view.fglayer.repo->min_zoom)
		gtk_widget_set_sensitive(zoomout_button, FALSE);
}

void status_label_set_text(char *status, gboolean is_markup)
{
	if(! GTK_WIDGET_VISIBLE(status_label))
		gtk_widget_show(status_label);

	if(is_markup)
		gtk_label_set_markup(GTK_LABEL(status_label), status);
	else
		gtk_label_set_text(GTK_LABEL(status_label), status);
}

void map_config_main_view(mouse_handler_t *mouse_handler, int flag,
	gboolean disable_buttons, gboolean frozen)
{
	gboolean enable = !disable_buttons;

	if (flag & 0x1)
		gtk_widget_set_sensitive(menu_button, enable);

	if (flag & 0x2) {
		gtk_widget_set_sensitive(zoomin_button, enable);
		gtk_widget_set_sensitive(zoomout_button, enable);
	}

	if (flag & 0x4)
		gtk_widget_set_sensitive(center_button, enable);

	if (flag & 0x8)
		gtk_widget_set_sensitive(fullscreen_button, enable);

	g_context.map_view_frozen = frozen;

	if (! frozen) {
		map_init_zoom();
		map_invalidate_view(TRUE);
	}

	if (mouse_handler)
		drawingarea_set_current_mouse_handler(mouse_handler);
	else
		drawingarea_reset_default_mouse_handler();
}

void map_toggle_menu_button(gboolean enable)
{
	gtk_widget_set_sensitive(menu_button, enable);
}

static void mouse_pressed(point_t point, guint time)
{
	clicked_point = point;
	clicked_time = time;
}

static inline void draw_cross(point_t point)
{
	const int r = 15;

	gdk_draw_line(drawingarea->window, g_context.crosssign_gc,
		point.x - r, point.y, point.x + r, point.y);
	gdk_draw_line(drawingarea->window, g_context.crosssign_gc,
		point.x, point.y - r, point.x, point.y + r);
}

static inline void show_lat_lon(point_t point)
{
	point.x += g_view.fglayer.tl_pixel.x;
	point.y += g_view.fglayer.tl_pixel.y;
	coord_t wgs84 = tilepixel_to_wgs84(point, g_view.fglayer.repo->zoom, g_view.fglayer.repo);

	char buf[128];
	snprintf(buf, sizeof(buf), "Clicked on: lat=%f, lon=%f",
		wgs84.lat, wgs84.lon);
	status_label_set_text(buf, TRUE);
}

static void mouse_released(point_t point, guint time)
{
	if (clicked_point.x == -1)
		return;

	int diff_x = point.x - clicked_point.x;
	int diff_y = point.y - clicked_point.y;
	int dist = sqrt(diff_x * diff_x + diff_y * diff_y);

	clicked_point.x = clicked_point.y = -1;

	time -= clicked_time;
	if (time < 200 || time > 3000)
		return;

	if (dist <= 10) {
		draw_cross(point);
		show_lat_lon(point);
		gdk_flush();
		sleep_ms(500);
		draw_cross(point);
		return;
	} else if (dist <= 30) {
		return;
	}

	/* When pan to map edge, avoid displaying blank map */
	int center_x = g_view.fglayer.center_pixel.x - diff_x;
	int center_y = g_view.fglayer.center_pixel.y - diff_y;

	/* get view and tiles in tile pixel coordinate */
	int view_tl_pixel_x = center_x - (g_view.width >> 1);
	int view_tl_pixel_y = center_y - (g_view.height >> 1);

	int view_br_pixel_x = view_tl_pixel_x + g_view.width;
	int view_br_pixel_y = view_tl_pixel_y + g_view.height;

	int max_pixel = (1 << g_view.fglayer.repo->zoom) * TILE_SIZE - 1;

	GdkRectangle world = {0, 0, max_pixel, max_pixel};
	GdkRectangle view_rect = { view_tl_pixel_x,  view_tl_pixel_y, view_br_pixel_x, view_br_pixel_y};
	if (! gdk_rectangle_intersect(&world, &view_rect, NULL))
		return;

	g_view.fglayer.center_pixel.x = center_x;
	g_view.fglayer.center_pixel.y = center_y;
	g_view.center_wgs84 = tilepixel_to_wgs84(g_view.fglayer.center_pixel,
		g_view.fglayer.repo->zoom, g_view.fglayer.repo);

	g_context.cursor_in_view = FALSE;
	GtkStyle *st = gtk_rc_get_style(center_button);
	modify_button_color(GTK_BUTTON(center_button), &(st->fg[GTK_STATE_NORMAL]), TRUE);

	map_invalidate_view(TRUE);
}

static void setup_drawingarea_mouse_handlers()
{
	mouse_handler.press_handler = mouse_pressed;
	mouse_handler.release_handler = mouse_released;
	mouse_handler.motion_handler = NULL;

	drawingarea_set_default_mouse_handler(&mouse_handler);
}

/**
 * Map repository may be changed at runtime.
 */
gboolean map_init()
{
	init_tile_converter(MAX_ZOOM_LEVELS);

	/* Not accurate, just see the earth as a standard sphere */

	long double d = 2.0 * M_PI * WGS84_SEMI_MAJOR_AXIS;
	int i;

	for (i=0; i<MAX_ZOOM_LEVELS; i++) {
		g_pixel_meters[i] = (double)(d / TILE_SIZE / (1 << i));
		sprintf(zoom_label_textarray[i], "%2d", i);
	}

	/* tile cache */
	g_view.fglayer.tile_cache = tilecache_new(TILE_CACHE_CAPACITY);
	g_view.bglayer.tile_cache = tilecache_new(TILE_CACHE_CAPACITY);

	g_view.pos_wgs84.lat = g_cfg->last_lat;
	g_view.pos_wgs84.lon = g_cfg->last_lon;

	g_view.center_wgs84.lat = g_cfg->last_center_lat;
	g_view.center_wgs84.lon = g_cfg->last_center_lon;

	setup_drawingarea_mouse_handlers();

	g_view.fglayer.center_pixel = wgs84_to_tilepixel(g_view.center_wgs84,
		g_view.fglayer.repo->zoom, g_view.fglayer.repo);

	map_init_zoom();

	return TRUE;
}

static void menu_button_clicked(GtkWidget *widget, gpointer data)
{
	switch_to_tab(TAB_ID_MAIN_MENU);
}

void view_tab_on_show()
{
	int ctx_num = ctx_tab_get_current_id();
	(g_ctx_panes[ctx_num].on_show)();

	poll_update_ui();
}

static GtkWidget * new_toolbar_button(GtkWidget *box, char *label)
{
	GtkWidget *button = gtk_button_new_with_label(label);
	gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_HALF);
	gtk_button_set_focus_on_click(GTK_BUTTON(button), FALSE);
	gtk_container_add (GTK_CONTAINER (box), button);
	return button;
}

gboolean status_label_clicked(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
	status_label_set_text("", FALSE);
	return FALSE;
}

GtkWidget * view_tab_create()
{
	g_view.da = drawingarea = gtk_drawing_area_new();
    gtk_widget_set_app_paintable(drawingarea, TRUE);
    gtk_widget_set_double_buffered(drawingarea, FALSE);

	gtk_widget_set_events (drawingarea, drawingarea_event_masks);
	drawingarea_init_mouse_handler(drawingarea, drawingarea_event_masks);

	g_signal_connect (drawingarea, "configure-event",
		G_CALLBACK(drawing_area_configure_event), NULL);

	g_signal_connect (drawingarea, "expose-event",
		G_CALLBACK(drawing_area_expose_event), NULL);

	GtkWidget *topbox = gtk_hbox_new(TRUE, 2);

	menu_button = new_toolbar_button(topbox, "Menu");
	g_signal_connect (G_OBJECT (menu_button), "clicked",
		G_CALLBACK (menu_button_clicked), NULL);

	zoomin_button = new_toolbar_button(topbox, "+");
	g_signal_connect (G_OBJECT (zoomin_button), "pressed",
		G_CALLBACK (zoom_button_pressed), (gpointer)TRUE);
	g_signal_connect (G_OBJECT (zoomin_button), "released",
		G_CALLBACK (zoom_button_released), NULL);

	zoom_label = gtk_label_new("");
	gtk_container_add (GTK_CONTAINER (topbox), zoom_label);

	zoomout_button = new_toolbar_button(topbox, "--");
	g_signal_connect (G_OBJECT (zoomout_button), "pressed",
		G_CALLBACK (zoom_button_pressed), (gpointer)FALSE);
	g_signal_connect (G_OBJECT (zoomout_button), "released",
		G_CALLBACK (zoom_button_released), NULL);

	center_button = new_toolbar_button(topbox, "Center");
	g_signal_connect (G_OBJECT (center_button), "clicked",
		G_CALLBACK (center_button_clicked), NULL);
	/* NOTE: see also mouse_released() */
	modify_button_color(GTK_BUTTON(center_button), &g_base_colors[ID_COLOR_Red], TRUE);

	fullscreen_button = new_toolbar_button(topbox, "Full");
	g_signal_connect (G_OBJECT (fullscreen_button), "clicked",
		G_CALLBACK (fullscreen_button_clicked), NULL);

	GtkWidget *exit_button = new_toolbar_button(topbox, "Exit");
	g_signal_connect (G_OBJECT (exit_button), "clicked",
		G_CALLBACK (exit_button_clicked), NULL);

	GtkWidget *bot_vbox = gtk_vbox_new(FALSE, 0);
	GtkWidget *hs = gtk_hseparator_new();
	gtk_container_add (GTK_CONTAINER (bot_vbox), hs);
	status_label = gtk_label_new("");
	gtk_label_set_justify(GTK_LABEL(status_label), GTK_JUSTIFY_LEFT);
	gtk_label_set_use_markup(GTK_LABEL(status_label), TRUE);

	GtkWidget *event_box = gtk_event_box_new();
	gtk_container_add(GTK_CONTAINER (event_box), status_label);
	gtk_event_box_set_above_child(GTK_EVENT_BOX (event_box), TRUE);
	gtk_container_add (GTK_CONTAINER (bot_vbox), event_box);

	gtk_widget_set_events(event_box, GDK_BUTTON_PRESS_MASK);
	g_signal_connect (G_OBJECT (event_box), "button_release_event",
		G_CALLBACK (status_label_clicked), NULL);

	GtkWidget *ctx_vbox = gtk_vbox_new(FALSE, 0);

	/* Various context sub tabs under drawing area  */
	GtkWidget *ctx = ctx_tab_container_create();
	gtk_container_add(GTK_CONTAINER (ctx_vbox), ctx);
	gtk_container_add(GTK_CONTAINER (ctx_vbox), bot_vbox);

	GtkWidget *vbox = gtk_vbox_new(FALSE, 0);
	gtk_box_pack_start(GTK_BOX (vbox), topbox, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX (vbox), drawingarea, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX (vbox), ctx_vbox, FALSE, FALSE, 0);

	return vbox;
}
