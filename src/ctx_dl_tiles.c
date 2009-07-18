#include "omgps.h"
#include "tile.h"
#include "util.h"
#include "xpm_image.h"
#include "customized.h"
#include "network.h"

static mouse_handler_t mouse_dlarea_handler;

static GtkWidget *lockview_button, *title_label;
#define NUM_BUTTON 6
static char *level_add_button_labels[NUM_BUTTON] = { "+1", "+2", "+3", "+4", "+5", "+6"};
static char *level_add_button_data[NUM_BUTTON] = { "1", "2", "3", "4", "5", "6" };
static GtkWidget *download_level_buttons[NUM_BUTTON];
static GdkRectangle dlarea, last_dlarea;
static coord_t dlarea_wgs84_tl, dlarea_wgs84_br;
static gboolean draw_dlarea_with_wgs84 = FALSE;
static gboolean dl_area_choosed = FALSE;

static GtkWidget *batchlist_notebook;

static GtkWidget *batchlist_treeview, *batchlist_treeview_sw;
static GtkListStore *batchlist_store = NULL;

static GtkWidget *batchinfo_treeview, *batchinfo_treeview_sw;
static GtkListStore *batchinfo_store = NULL;

static GdkPixbuf *cancel_image = NULL;

#define MIN_SIZE 30
#define AREA_BIG_ENOUGH(point) \
	(dlarea.width > MIN_SIZE && dlarea.height > MIN_SIZE)

typedef enum
{
	COL_BL_LEVELS, /* from zoom / to zoom*/
	COL_BL_NUM_TILES, /* {number of tiles to be downloaded} / {taotal number in range} */
	COL_BL_FAILED_PERCENT,
	COL_BL_CANCEL,
	COL_BL_DONE_PERCENT,
	COL_BL_BATCH,
	COL_BL_COUNT,
} batchlist_cols_t;

typedef enum
{
	COL_BINFO_URL,
	COL_BINFO_ERR,
	COL_BINFO_COUNT,
} batchinfo_cols_t;

static char *batchlist_col_names[] = {"Levels", "Tile Num", "Failed", "", "Progress"};
static char *batchinfo_col_names[] = {"URL", "error"};

void dl_tiles_update_buttons_on_zoom_changed()
{
	int zoom_level_limit = g_view.fglayer.repo->max_zoom - g_view.fglayer.repo->zoom;
	int i;
	for (i=0; i<NUM_BUTTON; i++) {
		gtk_widget_set_sensitive(download_level_buttons[i], (i+1 <= zoom_level_limit));
	}
}

/**
 * <topleft> and <botright>: tile pixel
 */
static void tile_batch_download(int levels, coord_t tl_wgs84, coord_t br_wgs84)
{
	map_repo_t *repo = g_view.fglayer.repo;
	int zoom = repo->zoom;

	if (zoom + levels > repo->max_zoom)
		levels = repo->max_zoom - zoom;

	batch_dl_t *batch = (batch_dl_t*)malloc(sizeof(batch_dl_t));
	if (! batch) {
		warn_dialog("batch download:\n\nunable to allocate memory!");
		return;
	}

	batch->repo = repo;
	batch->min_zoom = zoom + 1;
	batch->max_zoom = zoom + levels;
	batch->tl_wgs84 = tl_wgs84;
	batch->br_wgs84 = br_wgs84;

	/* prepare */

	int exists_size = batch_download_prepare(batch);

	if (exists_size < 0) {
		warn_dialog("batch download:\nallocate memory failed!");
		return;
	}
	float size_est;

	/* MB */
	if (batch->num_in_range == batch->num_dl_total)
		size_est = batch->num_dl_total * 0.01; // estimate
	else {
		int average_size = exists_size / (batch->num_in_range - batch->num_dl_total);
		size_est = 1.0 * average_size * batch->num_dl_total / (1024 * 1024);
	}

	char buf[128];
	if (batch->num_dl_total == 0) {
		snprintf(buf, sizeof(buf), "total %d tiles, already on disk.", batch->num_in_range);
		info_dialog(buf);
		return;
	}

	/* Assume each download takes 1 second */
	int seconds = (int)ceil((1 + 1000.0 / DL_SLEEP_MS) * batch->num_dl_total / TILE_DL_THREADS_LIMIT);
	int h = seconds / 3600;
	int remains = seconds - h * 3600;
	int m = remains / 60;
	int s = remains - m * 60;

	snprintf(buf, sizeof(buf), "tiles: %d of %d, disk space: ~%.2fMB, time: > %d:%d:%d",
		batch->num_dl_total, batch->num_in_range, size_est, h, m, s);

	if (! confirm_dialog(buf)) {
		free(batch);
		return;
	}

	/* batch download */

	log_info("batch download: map=%s, zoom=%d, +levels=%d", repo->name, zoom, levels);
	log_info("%s", buf);

	batch_download(batch);
}

void update_batch_dl_status()
{
	tile_downloader_t *td = (tile_downloader_t *)g_view.fglayer.repo->downloader;
	assert(td);

	LOCK_MUTEX(&(td->lock));

	if (td->batch_count == 0) {
		UNLOCK_MUTEX(&(td->lock));
		return;
	}

	gtk_list_store_clear(batchlist_store);

	batch_dl_t *b;
	int done_percent;
	char levels[32];
	char failed_percent[32];

	for (b = td->batches; b; b=b->next) {
		snprintf(levels, sizeof(levels), "%d - %d", b->min_zoom, b->max_zoom);
		snprintf(failed_percent, sizeof(failed_percent), "%.1f%%",
			100.0 * b->num_dl_failed / b->num_dl_total);
		done_percent = 100 * b->num_dl_done / b->num_dl_total;

		GtkTreeIter iter;
		gtk_list_store_append (batchlist_store, &iter);

		gtk_list_store_set (batchlist_store, &iter,
			COL_BL_LEVELS, levels,
			COL_BL_NUM_TILES, b->num_dl_total,
			COL_BL_FAILED_PERCENT, failed_percent,
			COL_BL_CANCEL, cancel_image,
			COL_BL_DONE_PERCENT, done_percent,
			COL_BL_BATCH, b,
			-1);
	}

	UNLOCK_MUTEX(&(td->lock));
}

static void draw_rectangle(GdkRectangle *rect)
{
	if (rect->width > 0 && rect->height > 0)
		gdk_draw_rectangle (g_view.da->window, g_context.dlarea_gc, FALSE,
			rect->x, rect->y, rect->width, rect->height);
}

/* avoid reversed signal order */
static gboolean dlarea_pressed = FALSE;

static void dlarea_mouse_pressed(point_t point, guint time)
{
	dlarea.x = point.x;
	dlarea.y = point.y;
	dlarea.width = dlarea.height = 0;

	dlarea_pressed = TRUE;
	draw_dlarea_with_wgs84 = FALSE;
}

#define ALAREA_ON_NEW_POINT(point)			\
{											\
	dlarea.x = MIN(point.x, dlarea.x);		\
	dlarea.y = MIN(point.y, dlarea.y);		\
	dlarea.width = abs(point.x - dlarea.x);	\
	dlarea.height = abs(point.y - dlarea.y);\
}

static void dlarea_mouse_motion(point_t point, guint time)
{
	if (! dlarea_pressed)
		return;

	draw_rectangle(&dlarea);
	ALAREA_ON_NEW_POINT(point);
	draw_rectangle(&dlarea);
}

static void dlarea_mouse_released(point_t point, guint time)
{
	if (! dlarea_pressed)
		return;

	draw_rectangle(&dlarea);

	ALAREA_ON_NEW_POINT(point);

	if (AREA_BIG_ENOUGH()) {
		map_draw_back_layers(g_view.da->window);
		draw_rectangle(&dlarea);
		last_dlarea = dlarea;
		dl_area_choosed = TRUE;
	} else {
		/* restore previous area if it is valid */
		dlarea = last_dlarea;
	}

	dlarea_pressed = FALSE;
}

static void dlarea_level_button_clicked(GtkWidget *widget, gpointer data)
{
	log_debug("dlarea_level_button_clicked");
	char *levels_mark = (char *)data;
	int levels = levels_mark[0] - '0';

	if (! dl_area_choosed || ! AREA_BIG_ENOUGH()) {
		warn_dialog("Download area is not specified!");
		return;
	}

	log_debug("guess network connecting...");
	if (! guess_network_is_connecting(TRUE)) {
		if (! confirm_dialog("Seems no network connection,\ncontinue?"))
			return;
	}

	GdkRectangle rect;

	if (! gdk_rectangle_intersect(&dlarea, &g_view.fglayer.visible, &rect)) {
		warn_dialog("The selected area contains no map tiles.");
		return;
	}

	map_repo_t *repo = g_view.fglayer.repo;
	int zoom = repo->zoom;

	point_t tl = {rect.x + g_view.fglayer.tl_pixel.x, rect.y + g_view.fglayer.tl_pixel.y};
	point_t br = {tl.x + rect.width, tl.y + rect.height};
	coord_t tl_wgs84 = tilepixel_to_wgs84(tl, zoom, repo);
	coord_t br_wgs84 = tilepixel_to_wgs84(br, zoom, repo);

	/* try download... */
	tile_batch_download(levels, tl_wgs84, br_wgs84);
}

static void reset_dlarea_data()
{
	memset(&dlarea, 0, sizeof(GdkRectangle));
	memset(&last_dlarea, 0, sizeof(GdkRectangle));
}

static void dlarea_redraw_func()
{
	if (draw_dlarea_with_wgs84) {
		map_repo_t *repo = g_view.fglayer.repo;
		int zoom = repo->zoom;

		point_t center_pixel = wgs84_to_tilepixel(g_view.center_wgs84, zoom, repo);
		point_t tl = wgs84_to_tilepixel(dlarea_wgs84_tl, zoom, repo);
		point_t br = wgs84_to_tilepixel(dlarea_wgs84_br, zoom, repo);

		dlarea.x = tl.x - (center_pixel.x - (g_view.width >> 1));
		dlarea.y = tl.y - (center_pixel.y - (g_view.height >> 1));
		dlarea.width = (br.x - tl.x);
		dlarea.height = (br.y - tl.y);
	}

	map_draw_back_layers(g_view.da->window);

	if (dlarea.width > 0 && dlarea.height > 0) {
		if (draw_dlarea_with_wgs84)
			gdk_gc_set_function(g_context.dlarea_gc, GDK_COPY);
		gdk_draw_rectangle (g_view.da->window, g_context.dlarea_gc, FALSE,
			dlarea.x, dlarea.y, dlarea.width, dlarea.height);
		gdk_gc_set_function(g_context.dlarea_gc, GDK_INVERT);
	}
}

static void toggle_dl_ctx_state(gboolean enable)
{
	if (enable) {
		map_set_redraw_func(&dlarea_redraw_func);
		map_config_main_view(&mouse_dlarea_handler, 0x2|0x4|0x8, TRUE, TRUE);
	} else {
		reset_dlarea_data();
		map_set_redraw_func(NULL);
		map_config_main_view(NULL, 0x2|0x4|0x8, FALSE, FALSE);
	}
}

void ctx_tab_dl_tiles_on_show()
{
	//toggle_fullscreen(TRUE);
	update_batch_dl_status();

	dl_tiles_update_buttons_on_zoom_changed();

	reset_dlarea_data();
	dl_area_choosed = FALSE;
	draw_dlarea_with_wgs84 = FALSE;

	char buf[128];
	snprintf(buf, sizeof(buf), "<span weight='bold'>Download map tiles: %s</span>",
		g_view.fglayer.repo->name);
    gtk_label_set_markup(GTK_LABEL(title_label), buf);
}

static void lockview_button_toggled(GtkWidget *widget, gpointer data)
{
	gboolean enable = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lockview_button));
	toggle_dl_ctx_state(enable);
}

static void close_button_clicked(GtkWidget *widget, gpointer data)
{
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lockview_button))) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lockview_button), FALSE);
	}
	reset_dlarea_data();
	drawingarea_reset_default_mouse_handler();

	switch_to_main_view(CTX_ID_GPS_FIX);
}

static void show_batch_download_range(batch_dl_t *batch)
{
	coord_t center_wgs84;
	center_wgs84.lat = (batch->tl_wgs84.lat + batch->br_wgs84.lat) / 2;
	center_wgs84.lon = (batch->tl_wgs84.lon + batch->br_wgs84.lon) / 2;
	int zoom = batch->min_zoom - 1;

	draw_dlarea_with_wgs84 = TRUE;

	dlarea_wgs84_tl = batch->tl_wgs84;
	dlarea_wgs84_br = batch->br_wgs84;

	/* make sure redraw function is set for batch download view -- which draw rectangle,
	 * instead of the normal
	 * must be called after map_zoom_to()  */
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lockview_button), TRUE);

	//toggle_dl_ctx_state(TRUE);

	map_zoom_to(zoom, center_wgs84, TRUE);
}

/**
 * NOTE: the problem is that: the event is triggered when button is pressed,
 * a button-released garbage is left. When switch to another notebook page,
 * the garbage event may trigger un-wanted actions.
 */
static void batchlist_treeview_row_selected(GtkTreeView *tree_view, gpointer user_data)
{
	GtkTreeSelection *sel = gtk_tree_view_get_selection(tree_view);
	int count = gtk_tree_selection_count_selected_rows (sel);
	if (count == 0)
		return;

	GtkTreeModel *treemodel = gtk_tree_view_get_model(tree_view);
	GList *rows = gtk_tree_selection_get_selected_rows(sel, &treemodel);
	GtkTreePath *path = (GtkTreePath *)(g_list_first(rows)->data);

	batch_dl_t *batch;

	GtkTreeIter iter;
	gtk_tree_model_get_iter (treemodel, &iter, path);
	gtk_tree_model_get (treemodel, &iter, COL_BL_BATCH, &batch, -1);
	gtk_tree_path_free(path);

	show_batch_download_range(batch);
}

static void cancel_batch_button_clicked(GtkCellRenderer *cell, gchar *path_str, gpointer data)
{
	GtkTreeIter iter;
	batch_dl_t *batch;

	GtkTreeModel *treemodel = gtk_tree_view_get_model (GTK_TREE_VIEW(batchlist_treeview));
	GtkTreePath *path = gtk_tree_path_new_from_string (path_str);
	gtk_tree_model_get_iter (treemodel, &iter, path);
	gtk_tree_model_get (treemodel, &iter, COL_BL_BATCH, &batch, -1);
	gtk_tree_path_free (path);

	if (batch->state != BATCH_DL_STATE_FINISHED) {
		if (! confirm_dialog("cancel this batch download?"))
			return;
	}

	download_cancel_batch(batch);

	gtk_list_store_remove (batchlist_store, &iter);
}


static GtkWidget * create_batchlist_treeview()
{
	batchlist_treeview = gtk_tree_view_new ();
	gtk_tree_view_set_reorderable(GTK_TREE_VIEW(batchlist_treeview), FALSE);
	gtk_tree_view_set_rules_hint (GTK_TREE_VIEW (batchlist_treeview), TRUE);

	GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(batchlist_treeview));
	gtk_tree_selection_set_mode (sel, GTK_SELECTION_BROWSE);

	/* For touch screen, better to use "cursor-changed", instead of "row-activated" */
	g_signal_connect (G_OBJECT(batchlist_treeview), "cursor-changed",
		G_CALLBACK (batchlist_treeview_row_selected), NULL);

	batchlist_treeview_sw = new_scrolled_window (NULL);
	gtk_container_add (GTK_CONTAINER (batchlist_treeview_sw), batchlist_treeview);

	/* add columns to the tree view */
	GtkCellRenderer *cell;
	GtkTreeViewColumn *col;

	int i;
	for (i=COL_BL_LEVELS; i<=COL_BL_FAILED_PERCENT; i++) {
		cell = gtk_cell_renderer_text_new();
		col = gtk_tree_view_column_new_with_attributes (batchlist_col_names[i], cell, "text", i, NULL);
		gtk_tree_view_append_column (GTK_TREE_VIEW(batchlist_treeview), col);
	}

	/* cancel column */
	cell = clickable_cell_renderer_pixbuf_new();
	col = gtk_tree_view_column_new_with_attributes("", cell, "pixbuf", COL_BL_CANCEL, NULL);
	g_signal_connect (G_OBJECT (cell), "clicked", G_CALLBACK (cancel_batch_button_clicked), NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW(batchlist_treeview), col);
	gtk_tree_view_column_set_fixed_width(col, 16);

	cell = gtk_cell_renderer_progress_new();
	col = gtk_tree_view_column_new_with_attributes (batchlist_col_names[COL_BL_DONE_PERCENT],
		cell, "value", COL_BL_DONE_PERCENT, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW(batchlist_treeview), col);

	batchlist_store = gtk_list_store_new (COL_BL_COUNT,
		G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING, GDK_TYPE_PIXBUF,
		G_TYPE_INT, G_TYPE_POINTER);
	gtk_tree_view_set_model(GTK_TREE_VIEW(batchlist_treeview), GTK_TREE_MODEL(batchlist_store));

	return batchlist_treeview_sw;
}

static GtkWidget * create_batchinfo_treeview()
{
	batchinfo_treeview = gtk_tree_view_new ();
	gtk_tree_view_set_rules_hint (GTK_TREE_VIEW (batchinfo_treeview), TRUE);
	gtk_tree_view_set_reorderable(GTK_TREE_VIEW (batchinfo_treeview), FALSE);

	GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(batchinfo_treeview));
	gtk_tree_selection_set_mode (sel, GTK_SELECTION_NONE);

	batchinfo_treeview_sw = new_scrolled_window (NULL);
	gtk_container_add (GTK_CONTAINER (batchinfo_treeview_sw), batchinfo_treeview);

	/* add columns to the tree view */
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *col;

	int i;
	for (i=0; i<COL_BINFO_COUNT; i++) {
		renderer = gtk_cell_renderer_text_new();
		col = gtk_tree_view_column_new_with_attributes (batchinfo_col_names[i], renderer, "text", i, NULL);
		gtk_tree_view_append_column (GTK_TREE_VIEW(batchinfo_treeview), col);
	}

	batchinfo_store = gtk_list_store_new (COL_BINFO_COUNT, G_TYPE_STRING, G_TYPE_STRING);
	gtk_tree_view_set_model(GTK_TREE_VIEW(batchinfo_treeview), GTK_TREE_MODEL(batchinfo_store));

	return batchinfo_treeview_sw;
}

void batch_dl_report_error(const char *url, const char *err)
{
	static int i = 0;
	if (i == 100) {
		GtkTreeIter iter;
		gtk_tree_model_get_iter_first(GTK_TREE_MODEL(batchinfo_store), &iter);
		gtk_list_store_remove(batchinfo_store, &iter);
	} else {
		++i;
	}

	GtkTreeIter iter;
	gtk_list_store_append (batchinfo_store, &iter);

	/* The value will be copied or referenced by the store if appropriate. */
	gtk_list_store_set (batchinfo_store, &iter,
		COL_BINFO_URL, url, COL_BINFO_ERR, err, -1);
}

GtkWidget * ctx_tab_dl_tiles_create()
{
	cancel_image = g_xpm_images[XPM_ID_DOWNLOAD_CANCEL].pixbuf;

	mouse_dlarea_handler.press_handler = dlarea_mouse_pressed;
	mouse_dlarea_handler.release_handler = dlarea_mouse_released;
	mouse_dlarea_handler.motion_handler = dlarea_mouse_motion;

	GtkWidget *vbox = gtk_vbox_new(FALSE, 0);

	title_label = gtk_label_new("");
	GtkWidget *sep = gtk_hseparator_new();

	gtk_container_add(GTK_CONTAINER (vbox), title_label);
	gtk_container_add(GTK_CONTAINER (vbox), sep);

	/* function buttons */
	lockview_button = gtk_toggle_button_new_with_label("lock view");
	g_signal_connect (G_OBJECT (lockview_button), "toggled",
			G_CALLBACK (lockview_button_toggled), NULL);
	GtkWidget *close_button = gtk_button_new_with_label("close");
	g_signal_connect (G_OBJECT (close_button), "clicked",
			G_CALLBACK (close_button_clicked), NULL);

	GtkWidget *button_vbox = gtk_vbox_new(FALSE, 0);

	GtkWidget *hbox = gtk_hbox_new(FALSE, 3);
	gtk_box_pack_start(GTK_BOX(button_vbox), hbox, FALSE, FALSE, 10);

	GtkWidget *tip_label = gtk_label_new("To choose download area: lock view then"
		" drag a rectangle on map.");
	gtk_label_set_justify(GTK_LABEL(tip_label), GTK_JUSTIFY_LEFT);
	gtk_misc_set_alignment(GTK_MISC(tip_label), 0, 0.5);
	gtk_label_set_line_wrap(GTK_LABEL(tip_label), TRUE);

	gtk_box_pack_start(GTK_BOX(button_vbox), tip_label, TRUE, TRUE, 0);

	/* level buttons */
	GtkWidget *hbox_1 = gtk_hbox_new(TRUE, 2);
	int i;
	for (i=0; i<NUM_BUTTON; i++) {
		download_level_buttons[i] = gtk_button_new_with_label(level_add_button_labels[i]);
		gtk_container_add (GTK_CONTAINER (hbox_1), download_level_buttons[i]);
		g_signal_connect (G_OBJECT (download_level_buttons[i]), "clicked",
			G_CALLBACK (dlarea_level_button_clicked), level_add_button_data[i]);
	}

	gtk_container_add(GTK_CONTAINER (hbox), lockview_button);
	gtk_container_add(GTK_CONTAINER (hbox), hbox_1);
	gtk_container_add(GTK_CONTAINER (hbox), close_button);

	batchlist_notebook = gtk_notebook_new();
	gtk_widget_set_size_request(batchlist_notebook, -1, 160);

	gtk_notebook_set_scrollable(GTK_NOTEBOOK(batchlist_notebook), FALSE);
	gtk_notebook_set_show_border(GTK_NOTEBOOK(batchlist_notebook), FALSE);

	GtkWidget *tab_label = gtk_label_new("action");
	gtk_notebook_append_page (GTK_NOTEBOOK (batchlist_notebook), button_vbox, tab_label);

	GtkWidget *batchlist = create_batchlist_treeview();
	tab_label = gtk_label_new("batches");
	gtk_notebook_append_page (GTK_NOTEBOOK (batchlist_notebook), batchlist, tab_label);

	GtkWidget *batchinfo = create_batchinfo_treeview();
	tab_label = gtk_label_new("error log");
	gtk_notebook_append_page (GTK_NOTEBOOK (batchlist_notebook), batchinfo, tab_label);

	gtk_container_add(GTK_CONTAINER(vbox), batchlist_notebook);

	return vbox;
}
