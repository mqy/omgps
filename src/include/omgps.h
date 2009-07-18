#ifndef OMGPS_H_
#define OMGPS_H_

#include <glib.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>

#include "wgs84.h"
#include "map_repo.h"
#include "tile.h"
#include "mouse.h"
#include "colors.h"
#include "gps.h"
#include "util.h"
#include "config.h"

/* Help debugging non-GPS related modules on development platform */
#define PLATFORM_FSO		TRUE

#define TOP_DIR				".omgps"

/* alpha blending levels: 25%, 50%, 75% */
#define ALPHA_LEVELS 		3

/* real-world fix should not bigger than this value,
 * else that map must be totally useless */
#define MAX_LAT_LON_FIX		0.1

#define HOME_PAGE			"http://omgps.projects.openmoko.org"

typedef enum {
	SETTINGS_LOADED,
	DOWNLOADER_INITED,
	MAP_INITED,
	WINDOW_SHOWN
} app_init_state_t;

typedef struct __cfg_t
{
	double last_center_lat;
	double last_center_lon;

	double last_lat;
	double last_lon;
	float last_alt;
	float last_pacc;

	char *agps_user;
	char *agps_pwd;

	char *last_map_name;
	char *last_sound_file;
} cfg_t;

typedef struct __map_view_tile_layer_t
{
	map_repo_t *repo;

	tilecache_t *tile_cache;

	/* top left and bottom right pixels of drawing area window in tile pixel coordinate */
	point_t tl_pixel;
	point_t br_pixel;

	/* view center_pixel pixel in tile coordinate */
	point_t center_pixel;

	point_t tl_tile;
	point_t br_tile;

	GdkRectangle visible;

	int tile_rows;
	int tile_cols;

	GdkPixbuf* tile_pixbuf;

} map_view_tile_layer_t;

typedef struct __map_view_t
{
	GtkWidget *da;
	int width;
	int height;

	coord_t center_wgs84;

	/* POS: offset in screen coordinate */
	point_t pos_offset;
	coord_t pos_wgs84;

	/* for misc drawing */
	GdkPixmap* pixmap;

	/* for alpha blending */
	GdkPixbuf* tile_pixbuf;
	gboolean tile_pixbuf_valid;

	map_view_tile_layer_t fglayer;
	map_view_tile_layer_t bglayer;

	/* sky map for SVs */
	GdkPixbuf *sky_pixbuf;

	int bg_alpha_idx;

	gboolean invalidate;

} map_view_t;

typedef struct __context_t
{
	char *top_dir;
	char *config_dir;
	char *maps_dir;
	char *screenshot_dir;
	char *track_dir;

	/* nav enabled or not. If enabled, GPS chip is powered on else off*/
	poll_state_t poll_state;
	poll_engine_t poll_engine;

	gboolean uart_conflict;
	gboolean time_synced;

	gboolean fullscreen;

	/* when GPS is running, don't draw current location to view */
	gboolean map_view_frozen;

	/* FIXME: configurable? */
	gboolean run_gps_on_start;

	gboolean cursor_in_view;

	gboolean dl_if_absent;

	gboolean suspend_disabled;

	gboolean sound_enabled;

	gboolean show_rulers;
	gboolean show_latlon_grid;
	gboolean track_enabled;

	/* internal speed unit is m/s. the speed_unit is used for display etc. */
	speed_unit_t speed_unit;

	/* draw lines etc. */

	GdkGC *pos_hacc_circle_gc;
	GdkGC *track_gc;
	GdkGC *scratch_gc;
	GdkGC *dlarea_gc;
	GdkGC *crosssign_gc;
	GdkGC *drawingarea_bggc;

	GdkGC *ruler_line_gc;
	GdkGC *ruler_text_gc;
	GdkGC *ruler_rect_gc;

	GdkGC *grid_line_gc;
	GdkGC *grid_text_gc;

	GdkGC *heading_gc;

	GdkGC *skymap_gc;

} context_t;

typedef enum
{
	TAB_ID_NONE = -1,
	TAB_ID_MAIN_VIEW = 0,
	TAB_ID_MAIN_MENU,
	TAB_ID_GPS_CFG,
	TAB_ID_MAP_TILE,
	TAB_ID_NAV_DATA,
	TAB_ID_TRACK,
	TAB_ID_SCRATCH,
	TAB_ID_SOUND,
	TAB_ID_MAX,
} TAB_ID_T;

typedef enum
{
	CTX_ID_NONE = -1,
	CTX_ID_GPS_FIX = 0,
	CTX_ID_AGPS_ONLINE,
	CTX_ID_DL_TILES,
	CTX_ID_SCRATCH,
	CTX_ID_FIX_MAP,
	CTX_ID_TRACK_REPLAY,
	CTX_ID_MAX
} CTX_ID_T;

typedef GtkWidget* (*menu_create_func_t)();
typedef void (*menu_show_func_t)();

typedef struct __menu_item_t
{
	char *name;
	TAB_ID_T id;
	menu_create_func_t create;
	menu_show_func_t on_show;
} menu_item_t;

typedef struct __ctx_tab_item_t
{
	menu_create_func_t create;
	menu_show_func_t on_show;
} ctx_tab_item_t;

typedef void (*map_redraw_view_func_t)();

#define POINT_IN_RANGE(test, tl, br) \
	(test.x >= tl.x && test.x <= br.x) && ((test.y >= tl.y && test.y <= br.y))

/******************* globals ***************************/

extern int				g_init_status;
extern map_view_t 		g_view;
extern cfg_t *			g_cfg;

extern context_t		g_context;
extern gps_data_t		g_gpsdata;
extern const char *		g_base_color_names[BASE_COLOR_COUNT];
extern GdkColor 		g_base_colors[BASE_COLOR_COUNT];
extern GdkPixbuf *		g_base_color_pixbufs[BASE_COLOR_COUNT];
extern double			g_pixel_meters[MAX_ZOOM_LEVELS];
//extern char 			g_ubx_receiver_versions[64];

extern GtkWidget *		g_window;
extern menu_item_t		g_menus[TAB_ID_MAX];
extern ctx_tab_item_t	g_ctx_panes[CTX_ID_MAX];
extern GtkWidget *		g_ctx_containers[CTX_ID_MAX];
extern TAB_ID_T			g_tab_id;

/******************* omgps.c ***************************/

extern void main_exit();

/******************* gc_resource.c ********************/

extern void drawing_init(GtkWidget *window);
extern void drawing_cleanup();

/******************* tab_nav.c ************************/

extern void update_nav_tab();

/***************** notebook tabs ************************/

extern GtkWidget * menu_tab_create();
extern void menu_tab_on_show();

extern GtkWidget * view_tab_create();
extern void view_tab_on_show();

extern GtkWidget * agps_tab_create();
extern void agps_tab_on_show();

extern GtkWidget * nav_tab_create();
extern void nav_tab_on_show();

extern GtkWidget * scratch_tab_create();
extern void scratch_tab_on_show();

extern GtkWidget * sound_tab_create();
extern void sound_tab_on_show();

/***************** tab_menu.c ***************************/

extern void switch_to_tab(TAB_ID_T page_num);
extern void switch_to_main_view(CTX_ID_T id);
extern void switch_to_ctx_tab(CTX_ID_T ctx_id);
extern void register_ui_panes();

extern GtkWidget* ctx_tab_container_create();
extern void ctx_tab_container_add(GtkWidget *tab);
extern void ctx_tab_hide_window(CTX_ID_T page_num);
extern CTX_ID_T ctx_tab_get_current_id();
extern void update_ui_on_zoom_level_changed();

/***************** context tabs *************************/

extern GtkWidget * ctx_tab_gps_fix_create();
extern void ctx_tab_gps_fix_on_show();

extern GtkWidget* ctx_tab_agps_online_create();
extern void ctx_tab_agps_online_on_show();

extern GtkWidget* ctx_tab_dl_tiles_create();
extern void ctx_tab_dl_tiles_on_show();

extern GtkWidget* ctx_tab_scratch_create();
extern void ctx_tab_scratch_on_show();

extern void dl_tiles_update_buttons_on_zoom_changed();

/***************** ruler.c ******************************/

void draw_map_meter(GdkDrawable *canvas);

/***************** tab_view.c ***************************/

extern gboolean map_init();
extern void map_cleanup();
extern void map_invalidate_view(gboolean redraw);
extern void map_centralize();
extern void map_redraw_background_map();
extern void map_redraw_view();
extern void status_label_set_text(char *status_text, gboolean is_markup);
extern void map_config_main_view(mouse_handler_t *mouse_handler, int flag,
	gboolean disable_buttons, gboolean frozen);
extern void map_toggle_menu_button(gboolean eanable);
extern void map_set_redraw_func(map_redraw_view_func_t func);
extern void map_zoom_to(int zoom, coord_t center_wgs84, gboolean redraw);
extern void map_draw_back_layers(GdkDrawable *canvas);
extern void toggle_fullscreen(gboolean full);

/***************** tab_nav.c ****************************/

extern void update_gps_sys_runtime_info();

/**************** settings.c ***************************/

extern cfg_t* settings_load();
extern gboolean settings_save();

#endif /* OMGPS_H_ */
