#include "omgps.h"
#include "tile.h"
#include "xpm_image.h"
#include "util.h"
#include "customized.h"

static GtkWidget *show_rulers_button, *show_latlon_grid_button;
static GtkWidget *maplist_treeview, *maplist_treeview_sw;
static GtkWidget *set_fg_button, *set_bg_button, *clear_bg_button, *dl_button, *fixmap_button;
static GtkListStore *maplist_store = NULL;
static GdkPixbuf *downloading_image, *yes_image;

static float alpha_values[ALPHA_LEVELS] = {0.25, 0.5, 0.75};
static char *alpha_text[ALPHA_LEVELS] = {"25%", "50%", "75%"};
static GtkWidget *alpha_radios[ALPHA_LEVELS];
static map_repo_t *selected_repo = NULL;

typedef enum
{
	COL_ML_MAP_NAME = 0,
	COL_ML_FG,
	COL_ML_BG,
	COL_ML_DOWNLOADING,
	COL_ML_LATLON_FIX,
	COL_ML_REPO,
	COL_ML_TYPE,
	COL_ML_COUNT,
} maplist_cols_t;

typedef enum
{
	LAYER_TYPE_NONE,
	LAYER_TYPE_FG,
	LAYER_TYPE_BG
} layer_type_t;

float* get_alpha_values()
{
	return alpha_values;
}

static gboolean maplist_update_batch_info (GtkTreeModel *model,
	GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
	map_repo_t *repo = NULL;
	gtk_tree_model_get (model, iter, COL_ML_REPO, &repo, -1);
	tile_downloader_t *td = (tile_downloader_t *)repo->downloader;

	LOCK_MUTEX(&(td->lock));
	gtk_list_store_set (GTK_LIST_STORE (model), iter, COL_ML_DOWNLOADING,
		(td->unfinished_batch_count > 0)? downloading_image : NULL, -1);
	UNLOCK_MUTEX(&(td->lock));

	return FALSE;
}

static void show_rulers_button_toggled(GtkWidget *widget, gpointer data)
{
	g_context.show_rulers = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
}

static void show_latlon_grid_button_toggled(GtkWidget *widget, gpointer data)
{
	g_context.show_latlon_grid = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
}

void tile_tab_on_show()
{
	int i;
	gboolean chosen;
	for (i=0; i<ALPHA_LEVELS; i++) {
		chosen = (i == g_view.bg_alpha_idx);
		if (chosen != gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(alpha_radios[i])))
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(alpha_radios[i]), chosen);
	}

	gtk_widget_set_sensitive(set_fg_button, FALSE);
	gtk_widget_set_sensitive(set_bg_button, FALSE);
	gtk_widget_set_sensitive(clear_bg_button, g_view.bglayer.repo != NULL);
	gtk_widget_set_sensitive(dl_button, FALSE);
	gtk_widget_set_sensitive(fixmap_button, FALSE);
}

static gboolean set_fg_map(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
	map_repo_t *repo;
	int type;
	gtk_tree_model_get (model, iter, COL_ML_REPO, &repo, COL_ML_TYPE, &type, -1);

	gtk_list_store_set (GTK_LIST_STORE (model), iter,
		COL_ML_FG, NULL, COL_ML_TYPE, LAYER_TYPE_NONE, -1);

	if (selected_repo == repo) {
		gtk_list_store_set (GTK_LIST_STORE (model), iter,
			COL_ML_FG, yes_image, COL_ML_TYPE, LAYER_TYPE_FG, -1);
	}

	return FALSE;
}

static gboolean set_bg_map(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
	map_repo_t *repo;
	gboolean clear = (gboolean)data;

	int type;

	gtk_tree_model_get (model, iter, COL_ML_REPO, &repo, COL_ML_TYPE, &type, -1);

	if (clear) {
		log_debug("clear: %s", repo->name);
		if (type == LAYER_TYPE_FG) {
			gtk_list_store_set (GTK_LIST_STORE (model), iter, COL_ML_BG, NULL, -1);
		} else {
			gtk_list_store_set (GTK_LIST_STORE (model), iter,
				COL_ML_BG, NULL, COL_ML_TYPE, LAYER_TYPE_NONE, -1);
		}
	} else {
		if (selected_repo == repo) {
			gtk_list_store_set (GTK_LIST_STORE (model), iter,
				COL_ML_BG, yes_image, COL_ML_TYPE, LAYER_TYPE_BG, -1);
		} else {
			gtk_list_store_set (GTK_LIST_STORE (model), iter, COL_ML_BG, NULL, -1);
		}
	}

	return FALSE;
}

static void set_fg_button_clicked(GtkWidget *widget, gpointer data)
{
	if (g_view.fglayer.repo == selected_repo)
		return;

	g_view.invalidate = TRUE;
	tilecache_cleanup(g_view.fglayer.tile_cache, FALSE);

	gtk_tree_model_foreach(GTK_TREE_MODEL(maplist_store), set_fg_map, NULL);
	g_view.fglayer.repo = selected_repo;

	gtk_widget_set_sensitive(set_fg_button, FALSE);
}

static void set_bg_button_clicked(GtkWidget *widget, gpointer data)
{
	if (g_view.bglayer.repo == selected_repo)
		return;

	g_view.invalidate = TRUE;
	tilecache_cleanup(g_view.bglayer.tile_cache, FALSE);

	gtk_tree_model_foreach(GTK_TREE_MODEL(maplist_store), set_bg_map, (gpointer)FALSE);
	g_view.bglayer.repo = selected_repo;

	gtk_widget_set_sensitive(set_bg_button, FALSE);
	gtk_widget_set_sensitive(clear_bg_button, TRUE);
}

static void clear_bg_button_clicked(GtkWidget *widget, gpointer data)
{
	if (! g_view.bglayer.repo)
		return;

	g_view.invalidate = TRUE;

	tilecache_cleanup(g_view.bglayer.tile_cache, FALSE);

	gtk_tree_model_foreach(GTK_TREE_MODEL(maplist_store), set_bg_map, (gpointer)TRUE);
	g_view.bglayer.repo = NULL;

	gtk_widget_set_sensitive(clear_bg_button, FALSE);
}

static void dl_button_clicked(GtkWidget *widget, gpointer data)
{
	if (! batch_download_check(selected_repo)) {
		warn_dialog("max unfinished batch limit reached, ignore");
		return;
	}

	clear_bg_button_clicked(clear_bg_button, NULL);

	set_fg_button_clicked(set_fg_button, NULL);

	switch_to_main_view(CTX_ID_DL_TILES);

	/* make sure redraw function is set for batch download view -- which draw rectangle,
	 * instead of the normal */
	//gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lockview_button), TRUE);
}

static void fixmap_button_clicked(GtkWidget *widget, gpointer data)
{
	/* disable map overlay */
	if (g_view.bglayer.repo == selected_repo)
		clear_bg_button_clicked(clear_bg_button, NULL);

	set_fg_button_clicked(set_fg_button, NULL);

	switch_to_main_view(CTX_ID_FIX_MAP);
}

static void add_map_to_table(map_repo_t *repo, void *arg)
{
	GtkTreeIter iter;
	gtk_list_store_append (maplist_store, &iter);

	int type;
	if (repo == g_view.fglayer.repo)
		type = LAYER_TYPE_FG;
	else if (repo == g_view.bglayer.repo)
		type = LAYER_TYPE_BG;
	else
		type = LAYER_TYPE_NONE;

	char buf[64];
	if (fabs(repo->lat_fix > 1e-6 || fabs(repo->lon_fix) > 1e-6))
		snprintf(buf, sizeof(buf), "lat=%f, lon=%f", repo->lat_fix, repo->lon_fix);
	else
		buf[0] = '\0';

	/* NOTE: COL_ML_ACTIVATABLE */
	gtk_list_store_set (maplist_store, &iter,
		COL_ML_MAP_NAME, repo->name,
		COL_ML_FG, (type == LAYER_TYPE_FG)? yes_image : NULL,
		COL_ML_BG, (type == LAYER_TYPE_BG)? yes_image : NULL,
		COL_ML_DOWNLOADING, NULL,
		COL_ML_LATLON_FIX, buf,
		COL_ML_REPO, repo,
		COL_ML_TYPE, type,
		-1);
}

void fixmap_update_maplist(map_repo_t *repo)
{
	gtk_list_store_clear(GTK_LIST_STORE(maplist_store));

	mapcfg_iterate_maplist(add_map_to_table, NULL);
}

static void alpha_radio_toggled (GtkWidget *widget, gpointer user_data)
{
	int idx = (int)user_data;

	if (g_view.bg_alpha_idx == idx)
		return;

	g_view.bg_alpha_idx = idx;
	if (g_view.bglayer.repo != NULL)
		g_view.invalidate = TRUE;
}

static void maplist_treeview_row_selected (GtkTreeView *tree_view, gpointer user_data)
{
	GtkTreeSelection *sel = gtk_tree_view_get_selection(tree_view);
	int count = gtk_tree_selection_count_selected_rows (sel);
	if (count == 0)
		return;

	GtkTreeIter iter;

	GtkTreeModel *treemodel = gtk_tree_view_get_model(tree_view);
	GList *rows = gtk_tree_selection_get_selected_rows(sel, &treemodel);
	GtkTreePath *path = (GtkTreePath *)(g_list_first(rows)->data);

	gtk_tree_model_get_iter (treemodel, &iter, path);
	gtk_tree_model_get (treemodel, &iter, COL_ML_REPO, &selected_repo, -1);

	/* set sensitivity of buttons */

	gboolean sensitive = (selected_repo != g_view.fglayer.repo) && (selected_repo != g_view.bglayer.repo);

	gtk_widget_set_sensitive(set_fg_button, sensitive);
	gtk_widget_set_sensitive(set_bg_button, sensitive);

	gtk_widget_set_sensitive(dl_button, TRUE);
	gtk_widget_set_sensitive(fixmap_button, TRUE);

	/* show download batches list */

	tile_downloader_t *td = (tile_downloader_t *) selected_repo->downloader;

	if (td->batch_count == 0)
		return;

	gtk_tree_model_foreach(GTK_TREE_MODEL(maplist_store), maplist_update_batch_info, NULL);

	gtk_tree_path_free(path);
}

static void create_maplist_treeview()
{
	maplist_treeview = gtk_tree_view_new ();
	gtk_tree_view_set_rules_hint (GTK_TREE_VIEW (maplist_treeview), TRUE);
	gtk_tree_view_set_reorderable(GTK_TREE_VIEW (maplist_treeview), FALSE);

	GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(maplist_treeview));
	gtk_tree_selection_set_mode (sel, GTK_SELECTION_SINGLE);
	g_signal_connect (G_OBJECT(maplist_treeview), "cursor-changed",
		G_CALLBACK (maplist_treeview_row_selected), NULL);

	maplist_treeview_sw = new_scrolled_window (NULL);
	gtk_container_add (GTK_CONTAINER (maplist_treeview_sw), maplist_treeview);

	/* add columns to the tree view */
	GtkCellRenderer *cell;
	GtkTreeViewColumn *col;

	cell = gtk_cell_renderer_text_new();
	col = gtk_tree_view_column_new_with_attributes ("Map name", cell, "text", COL_ML_MAP_NAME, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW(maplist_treeview), col);

	cell = gtk_cell_renderer_pixbuf_new();
	col = gtk_tree_view_column_new_with_attributes (" FG ", cell, "pixbuf", COL_ML_FG, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW(maplist_treeview), col);

	cell = gtk_cell_renderer_pixbuf_new();
	col = gtk_tree_view_column_new_with_attributes (" BG", cell, "pixbuf", COL_ML_BG, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW(maplist_treeview), col);

	/* download column */
	cell = gtk_cell_renderer_pixbuf_new();
	col = gtk_tree_view_column_new_with_attributes("", cell, "pixbuf", COL_ML_DOWNLOADING, NULL);
	gtk_tree_view_column_set_fixed_width(col, 16);
	gtk_tree_view_append_column (GTK_TREE_VIEW(maplist_treeview), col);

	cell = gtk_cell_renderer_text_new();
	col = gtk_tree_view_column_new_with_attributes ("Lat/lon fix(°)", cell, "text", COL_ML_LATLON_FIX, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW(maplist_treeview), col);

	/* populate map list */

	maplist_store = gtk_list_store_new (COL_ML_COUNT,
		G_TYPE_STRING, GDK_TYPE_PIXBUF, GDK_TYPE_PIXBUF, GDK_TYPE_PIXBUF,
		G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_INT);

	gtk_tree_view_set_model(GTK_TREE_VIEW(maplist_treeview), GTK_TREE_MODEL(maplist_store));

	mapcfg_iterate_maplist(add_map_to_table, NULL);
}

GtkWidget * tile_tab_create()
{
	downloading_image = g_xpm_images[XPM_ID_DOWNLOADING].pixbuf;
	yes_image = g_xpm_images[XPM_ID_YES].pixbuf;

	GtkWidget *vbox = gtk_vbox_new(FALSE, 5);

	/* Toggle buttons */

	GtkWidget *meter_hbox = gtk_hbox_new(FALSE, 0);

	show_rulers_button = gtk_check_button_new_with_label("Show ruler");
	gtk_container_add (GTK_CONTAINER (meter_hbox), show_rulers_button);
	gtk_toggle_button_set_mode (GTK_TOGGLE_BUTTON (show_rulers_button), TRUE);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON (show_rulers_button), g_context.show_rulers);
	g_signal_connect (G_OBJECT (show_rulers_button), "toggled",
		G_CALLBACK (show_rulers_button_toggled), NULL);

	show_latlon_grid_button = gtk_check_button_new_with_label("Show lat/lon grid");
	gtk_container_add (GTK_CONTAINER (meter_hbox), show_latlon_grid_button);
	gtk_toggle_button_set_mode (GTK_TOGGLE_BUTTON (show_latlon_grid_button), TRUE);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON (show_latlon_grid_button), g_context.show_latlon_grid);
	g_signal_connect (G_OBJECT (show_latlon_grid_button), "toggled",
		G_CALLBACK (show_latlon_grid_button_toggled), NULL);

	/* map list treeview */
	create_maplist_treeview();

	/* set background map alpha level */

	GtkWidget *alpha_hbox = gtk_hbox_new(FALSE, 5);

	GtkWidget *alpha_label = gtk_label_new(" BG map alpha: ");
	gtk_misc_set_alignment(GTK_MISC(alpha_label), 0.0, 0.5);
	gtk_container_add (GTK_CONTAINER (alpha_hbox), alpha_label);

	int i;
	alpha_radios[0] = gtk_radio_button_new_with_label(NULL, alpha_text[0]);
	for (i=1; i<ALPHA_LEVELS; i++) {
		alpha_radios[i] = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(alpha_radios[0]),
				alpha_text[i]);
	}

	for (i=0; i<ALPHA_LEVELS; i++) {
		g_signal_connect (G_OBJECT (alpha_radios[i]), "toggled",
			G_CALLBACK (alpha_radio_toggled), (gpointer)i);
		gtk_container_add(GTK_CONTAINER (alpha_hbox), alpha_radios[i]);
	}

	GtkWidget *button_hbox = gtk_hbox_new(TRUE, 5);

	set_fg_button = gtk_button_new_with_label("Set FG");
	g_signal_connect (G_OBJECT(set_fg_button), "clicked", G_CALLBACK (set_fg_button_clicked), NULL);
	gtk_container_add(GTK_CONTAINER(button_hbox), set_fg_button);

	set_bg_button = gtk_button_new_with_label("Set BG");
	g_signal_connect (G_OBJECT(set_bg_button), "clicked", G_CALLBACK (set_bg_button_clicked), NULL);
	gtk_container_add(GTK_CONTAINER(button_hbox), set_bg_button);

	clear_bg_button = gtk_button_new_with_label("Clear BG");
	g_signal_connect (G_OBJECT(clear_bg_button), "clicked", G_CALLBACK (clear_bg_button_clicked), NULL);
	gtk_container_add(GTK_CONTAINER(button_hbox), clear_bg_button);

	dl_button = gtk_button_new_with_label("Download");
	g_signal_connect (G_OBJECT(dl_button), "clicked", G_CALLBACK (dl_button_clicked), NULL);
	gtk_container_add(GTK_CONTAINER(button_hbox), dl_button);

	fixmap_button = gtk_button_new_with_label("Fix Map");
	g_signal_connect (G_OBJECT(fixmap_button), "clicked", G_CALLBACK (fixmap_button_clicked), NULL);
	gtk_container_add(GTK_CONTAINER(button_hbox), fixmap_button);

	gtk_box_pack_start(GTK_BOX (vbox), meter_hbox, FALSE, FALSE, 10);
	gtk_box_pack_start(GTK_BOX(vbox), maplist_treeview_sw, TRUE, TRUE, 5);
	gtk_box_pack_start(GTK_BOX(vbox), button_hbox, FALSE, FALSE, 5);
	gtk_box_pack_start(GTK_BOX(vbox), alpha_hbox, FALSE, FALSE, 5);

	return vbox;
}
