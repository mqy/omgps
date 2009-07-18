#include <dirent.h>

#include "omgps.h"
#include "xpm_image.h"
#include "util.h"
#include "track.h"
#include "customized.h"

#define TRACK_FILE_EXT	".txt"

static char *track_file_path = NULL;
static char *track_file_name = NULL;
static track_group_t *tracks = NULL;

static GtkWidget *colorlist, *change_color_button, *new_track_button, *stop_track_button;
static int cur_color_idx = 0;

static GtkWidget *filelist_treeview, *filelist_treeview_sw;
static GtkListStore *filelist_store = NULL;
static char *filelist_treeview_col_names[] = {"File name", "Start time", "End time", "Records"};

static GtkWidget *replay_button, *delete_button, *export_gpx_button;
static char *replay_file = NULL;
static char *replay_file_path = NULL;

static U4 previous_gps_tow = 0u;
static U4 first_gps_tow = 0u;

static void add_file_to_list(GtkTreeIter *iter, char *filepath, char *filename);

static char *get_full_path(char *fullpath, int buf_len, char *file)
{
	if (fullpath == NULL)
		fullpath = (char *)malloc(buf_len);
	snprintf(fullpath, buf_len, "%s/%s", g_context.track_dir, file);
	return fullpath;
}

static int create_record_filename()
{
	struct stat st;

	const int size = 256;
	track_file_path = (char *)malloc(size);
	if (track_file_path == NULL)
		return -1;

	int n = snprintf(track_file_path, size, "%s/", g_context.track_dir);
	if (n <= 0)
		return -1;

	track_file_name = track_file_path + n;

	if (stat(track_file_path, &st) < 0 && mkdir(track_file_path, 0700) < 0)
		return -1;

	time_t tm;
	time(&tm);
	struct tm *t = localtime(&tm);
	char buf[30];
	if (strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", t) <= 0)
		return -1;
	n = snprintf((char *)(track_file_path+n), size-n, "%s"TRACK_FILE_EXT, buf);
	if(n <= 0)
		return -1;
	return 0;
}

char *get_cur_replay_filepath()
{
	return replay_file_path;
}

/**
 * May be called by polling thread or main thread. protected by gdk UI lock.
 */
int track_save(gboolean all, gboolean _free)
{
	int i, count = 0;

	if (! tracks || tracks->count == 0)
		goto END;

	struct stat st;
	gboolean empty = (stat(track_file_path, &st) < 0) || st.st_size == 0;

	/* append, create file if not exists */
	FILE *fp = fopen(track_file_path, "a");

	if (! fp) {
		log_warn("can't open track records file: %s", track_file_path);
		return 0;
	}

	if (empty) {
		fprintf(fp, TRACK_HEAD_LABEL_1"%-10u\n"
			TRACK_HEAD_LABEL_2"%-10u\n"
			TRACK_HEAD_LABEL_3"%-10u\n", (U4)tracks->starttime, 0u, 0u);
	}

	count = all? tracks->count : tracks->count / 3;
	for (i=0; i<count; i++) {
		fprintf(fp, "%f\t%f\t%u\n", tracks->tps[i].wgs84.lat,
			tracks->tps[i].wgs84.lon, tracks->tps[i].time_offset);
	}

	fclose(fp);

	/* update end time and count */
	fp = fopen(track_file_path, "r+");

	/* end time: time of last valid record */
	time_t endtime = tracks->starttime + tracks->tps[tracks->count - 1].time_offset;

	int offset = strlen(TRACK_HEAD_LABEL_1) + 11 + strlen(TRACK_HEAD_LABEL_2);
	fseek(fp, offset, SEEK_SET);
	fprintf(fp, "%-10u", (U4)endtime);
	offset += 11 + strlen(TRACK_HEAD_LABEL_3);
	fseek(fp, offset, SEEK_SET);
	fprintf(fp, "%-10u", (U4)(tracks->total_count - (tracks->count - count)));

	fclose(fp);

	/* add to list */

	if (all) {
		GtkTreeIter iter;
		gtk_list_store_insert (filelist_store, &iter, 0);
		add_file_to_list(&iter, track_file_path, track_file_name);
	}

END:

	if (_free) {
		free(tracks);
		tracks = NULL;
		free(track_file_path);
		track_file_path = NULL;
	}

	return count;
}

void track_cleanup()
{
	/* better to do this after death of polling thread */
	if (g_context.track_enabled)
		track_save(TRUE, TRUE);

	track_replay_cleanup();
}

/**
 * May be called by polling thread or main thread. protected by gdk UI lock.
 */
static gboolean track_init(gboolean create_recording_file_path)
{
	if (tracks == NULL) {
		if (! (tracks = (track_group_t *)malloc(sizeof(track_group_t)))) {
			char *msg = "Track: can't allocate memory. exit";
			log_error(msg);
			warn_dialog(msg);
			exit(0);
		}
	}

	tracks->count = 0;
	tracks->total_count = 0;
	tracks->starttime = 0;
	tracks->last_drawn_index = 0;

	previous_gps_tow = (U4)0;
	first_gps_tow = (U4)0;

	if (create_recording_file_path) {
		if (track_file_path)
			free(track_file_path);

		if (create_record_filename() != 0) {
			char *msg = "create file name for track recording failed.";
			log_error(msg);
			warn_dialog(msg);
			return FALSE;
		} else {
			log_info("new track record file: %s", track_file_path);
		}
	}

	return TRUE;
}

/**
 * OS time is not reliable, so we use GPS time to determine relative span.
 * GPS tow can be smaller than previous when cross GPS week.
 * Anyway, restart a new log file when time reverse detected
 */
void track_add(/*double lat, double lon, U4 gps_tow*/)
{
	static const float threshold = TRACK_MAX_DELTA * (180.0 / (WGS84_SEMI_MAJOR_AXIS * M_PI));

	/* NOTE: make max frequency is 1 HZ, since we record time offset with unit of seconds */
	//if (g_gpsdata.llh_itow - previous_gps_tow < 1000)
	//	return;

	trackpoint_t *tp;

	if (tracks->count > 0) {
		tp = &(tracks->tps[tracks->count-1]);
		float lat_delta = (float)fabs(g_gpsdata.lat - tp->wgs84.lat);
		float lon_delta = (float)fabs(g_gpsdata.lon - tp->wgs84.lon);
		if (sqrt(lat_delta * lat_delta + lon_delta * lon_delta) < threshold)
			return;

		/* new GPS week: split track file */
		if (g_context.track_enabled && (g_gpsdata.llh_itow < previous_gps_tow)) {
			track_save(TRUE, FALSE);
			track_init(TRUE);
			return;
		}
	} else {
		tracks->starttime = time(NULL);
		first_gps_tow = g_gpsdata.llh_itow;
	}

	tp = &(tracks->tps[tracks->count]);
	/* Copy values */
	tp->wgs84.lat = g_gpsdata.lat;
	tp->wgs84.lon = g_gpsdata.lon;
	tp->id = tracks->total_count;
	tp->time_offset = (g_gpsdata.llh_itow - first_gps_tow) / (U4)1000;

	previous_gps_tow = g_gpsdata.llh_itow;

	++tracks->count;
	++tracks->total_count;

	if (tracks->count >= TRACK_MAX_IN_MEM_RECORDS) {
		int saved_count = 0;
		if (g_context.track_enabled)
			saved_count = track_save(FALSE, FALSE);
		if (saved_count == 0) {
			/* lose data but we need space */
			saved_count = tracks->count / 2;
		}
		/* move the remaining elements to array head */
		tracks->count -= saved_count;
		memcpy(&tracks->tps[0], &(tracks->tps[saved_count]), tracks->count * sizeof(trackpoint_t));
	}
}

/**
 * Offset is the offset in pixmap coordinate, tl_pixelx/y are global tile coordinate.
 */
void track_draw(GdkPixmap *canvas, gboolean refresh, GdkRectangle *rect)
{
	if (! tracks || tracks->count < 2)
		return;

	int last_index = tracks->last_drawn_index;

	int start_idx, j;
	if (refresh || last_index < tracks->tps[0].id) {
		start_idx = 0;
	} else {
		start_idx = last_index - tracks->tps[0].id;
		/* too much un-drawn points, being flushed due to space limitation */
		if (start_idx < 0)
			start_idx = 0;
	}
	map_view_tile_layer_t *layer = &(g_view.fglayer);

	int min_x, max_x, min_y, max_y;
	min_x = g_view.width;
	max_x = max_y = 0;
	min_y = g_view.height;

	for (j = start_idx; j < tracks->count; j++) {
		tracks->tps[j].pixel = wgs84_to_tilepixel(tracks->tps[j].wgs84, layer->repo->zoom, layer->repo);
		tracks->tps[j].inview = POINT_IN_RANGE(tracks->tps[j].pixel, layer->tl_pixel, layer->br_pixel);
		/* window coordinate */
		tracks->tps[j].pixel.x -= layer->tl_pixel.x;
		tracks->tps[j].pixel.y -= layer->tl_pixel.y;

		if (! refresh) {
			if (tracks->tps[j].pixel.x > max_x)
				max_x = tracks->tps[j].pixel.x;
			if (tracks->tps[j].pixel.x < min_x)
				min_x = tracks->tps[j].pixel.x;

			if (tracks->tps[j].pixel.y > max_y)
				max_y = tracks->tps[j].pixel.y;
			if (tracks->tps[j].pixel.y < min_y)
				min_y = tracks->tps[j].pixel.y;
		}
	}

	for (j = start_idx + 1; j < tracks->count; j++) {
		if (tracks->tps[j].inview && tracks->tps[j-1].inview) {
			gdk_draw_line(canvas, g_context.track_gc, tracks->tps[j-1].pixel.x, tracks->tps[j-1].pixel.y,
				tracks->tps[j].pixel.x, tracks->tps[j].pixel.y);
		}
	}
	tracks->last_drawn_index = tracks->total_count - 1;

	if (! refresh && rect) {
		rect->x = min_x;
		rect->y = min_y;
		rect->width = max_x - min_x;
		rect->height = max_y - min_y;
	}
}

static void add_file_to_list(GtkTreeIter *iter, char *filepath, char *filename)
{
	U4 start_time, end_time, record_count;

	char st_buf[30];
	char et_buf[30];
	struct tm *tm;
	time_t tt;

	if (! filepath) {
		log_warn("track file path is NULL");
		return;
	}

	FILE *fp = fopen(filepath, "r");
	if (! fp) {
		log_warn("Open file path %s failed", filepath);
		return;
	}
	/* head */
	fscanf(fp, TRACK_HEAD_LABEL_1"%u\n", &start_time);
	fscanf(fp, TRACK_HEAD_LABEL_2"%u\n", &end_time);
	fscanf(fp, TRACK_HEAD_LABEL_3"%u\n", &record_count);

	tt = start_time;
	tm = localtime(&tt);
	strftime(st_buf, sizeof(st_buf), "%Y-%m-%d %H:%M:%S", tm);

	tt = end_time;
	tm = localtime(&tt);
	strftime(et_buf, sizeof(et_buf), "%Y-%m-%d %H:%M:%S", tm);

	gtk_list_store_set (filelist_store, iter,
		0, filename,
		1, &st_buf[11],
		2, strncmp(st_buf, et_buf, 10) == 0? &et_buf[11] : et_buf,
		3, record_count,
		-1);

	fclose(fp);
}

static void new_track_button_clicked(GtkWidget *widget, gpointer data)
{
	if (! track_init(TRUE))
		return;

	g_context.track_enabled = TRUE;
	gtk_widget_set_sensitive(new_track_button, FALSE);
	gtk_widget_set_sensitive(stop_track_button, TRUE);

	ctx_gpsfix_on_track_state_changed();
}

static void stop_track_button_clicked(GtkWidget *widget, gpointer data)
{
	if (! g_context.track_enabled)
		return;

	g_context.track_enabled = FALSE;

	gtk_widget_set_sensitive(new_track_button, TRUE);
	gtk_widget_set_sensitive(stop_track_button, FALSE);

	track_save(TRUE, FALSE);

	ctx_gpsfix_on_track_state_changed();
}

static void track_colorlist_set_initial_color()
{
	int idx = ID_COLOR_Blue;
	gdk_gc_set_rgb_fg_color(g_context.track_gc, &g_base_colors[idx]);
	gtk_combo_box_set_active(GTK_COMBO_BOX(colorlist), idx);
	cur_color_idx = idx;
}

static void change_color_button_clicked(GtkWidget *widget, gpointer data)
{
	int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(colorlist));
	if (idx == cur_color_idx)
		return;

	cur_color_idx = idx;
	gdk_gc_set_rgb_fg_color(g_context.track_gc, &g_base_colors[cur_color_idx]);
	/* when notebook changes to page 0, the view is redrawn automatically and
	 * top layer is updated */
}

void track_tab_on_show ()
{
	gtk_combo_box_set_active(GTK_COMBO_BOX(colorlist), cur_color_idx);
	gtk_widget_set_sensitive(replay_button, FALSE);
	gtk_widget_set_sensitive(delete_button, FALSE);
	gtk_widget_set_sensitive(export_gpx_button, FALSE);
	gtk_widget_set_sensitive(change_color_button, FALSE);

	if (POLL_STATE_TEST(RUNNING)) {
		gtk_widget_set_sensitive(new_track_button, ! g_context.track_enabled);
		gtk_widget_set_sensitive(stop_track_button, g_context.track_enabled);
	} else {
		gtk_widget_set_sensitive(new_track_button, FALSE);
		gtk_widget_set_sensitive(stop_track_button, FALSE);
	}
}

static void colorlist_changed (GtkComboBox *widget, gpointer user_data)
{
	int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(colorlist));
	if (idx == cur_color_idx)
		return;
	gtk_widget_set_sensitive(change_color_button, TRUE);
}

static void create_colorlist()
{
	GtkListStore *store = gtk_list_store_new (1, GDK_TYPE_PIXBUF);
	colorlist = gtk_combo_box_new_with_model(GTK_TREE_MODEL(store));
	GtkCellRenderer *cell = gtk_cell_renderer_pixbuf_new();
	gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(colorlist), cell, TRUE);
	gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (colorlist), cell, "pixbuf", 0, NULL);

	GtkTreeIter iter;
	int i;

	for (i=0; i<BASE_COLOR_COUNT; i++) {
		gtk_list_store_append (store, &iter);
		gtk_list_store_set (store, &iter, 0, g_base_color_pixbufs[i], -1);
	}
	g_object_unref (store);

	g_signal_connect (G_OBJECT (colorlist), "changed", G_CALLBACK (colorlist_changed), NULL);
}

static char * get_selected_file(GtkTreeIter *iter)
{
	GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(filelist_treeview));
	int count = gtk_tree_selection_count_selected_rows (sel);
	if (count == 0)
		return NULL;

	char *file = NULL;

	GtkTreeModel *treemodel = gtk_tree_view_get_model(GTK_TREE_VIEW(filelist_treeview));
	GList *rows = gtk_tree_selection_get_selected_rows(sel, &treemodel);
	GtkTreePath *path = (GtkTreePath *)(g_list_first(rows)->data);

	gtk_tree_model_get_iter (treemodel, iter, path);
	gtk_tree_model_get (treemodel, iter, 0, &file,	-1);
	gtk_tree_path_free(path);

	return file;
}

static void replay_button_clicked(GtkWidget *widget, gpointer data)
{
	GtkTreeIter iter;
	char *file = get_selected_file(&iter);
	if (file) {
		if (replay_file_path)
			free(replay_file_path);

		replay_file = file;
		replay_file_path = get_full_path(NULL, 256, file);

		switch_to_main_view(CTX_ID_TRACK_REPLAY);
	} else {
		info_dialog("Replay: no file selected!");
	}
}

static void delete_button_clicked(GtkWidget *widget, gpointer data)
{
	GtkTreeIter iter;
	char *file = get_selected_file(&iter);
	if (file) {
		char buf[64];
		snprintf(buf, sizeof(buf), "Delete file:\n\n%s", file);
		if (confirm_dialog(buf)) {
			gtk_list_store_remove (filelist_store, &iter);
			char buf[256];
			char *path = get_full_path(buf, sizeof(buf), file);
			unlink(path);
		}
	} else {
		info_dialog("Delete: no file selected!");
	}
}

static void export_gpx(char *file)
{
	char fullpath[256];
	FILE *fp_src = NULL, *fp_dest = NULL;

	get_full_path(fullpath, sizeof(fullpath), file);

	fp_src = fopen(fullpath, "r");
	if (! fp_src) {
		warn_dialog("Unable to open src file");
		return;
	}

	/* NOTE: now fullpath refers to dest file */
	sprintf(fullpath + strlen(fullpath) - 4, "%s", ".gpx");

	fp_dest = fopen(fullpath, "w+");
	if (! fp_dest) {
		warn_dialog("Unable to open dest file");
		fclose(fp_src);
		return;
	}

	gboolean ret = TRUE;

	/* head */
	U4 start_time, end_time, record_count;
	fscanf(fp_src, TRACK_HEAD_LABEL_1"%u\n", &start_time);
	fscanf(fp_src, TRACK_HEAD_LABEL_2"%u\n", &end_time);
	fscanf(fp_src, TRACK_HEAD_LABEL_3"%u\n", &record_count);

	double lat, lon;
	U4 time_offset;
	int n;

	char tm_buf[64];
	time_t tt = start_time;
	struct tm *tm = gmtime(&tt);

	fprintf(fp_dest,
		"<?xml version=\"1.0\"?>\n<gpx version=\"1.1\" creator=\"omgps - http://code.google.com/p/omgps/\"\n "
		"xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n "
		"xmlns=\"http://www.topografix.com/GPX/1/1\"\n"
		"xsi:schemaLocation=\"http://www.topografix.com/GPX/1/1 http://www.topografix.com/GPX/1/1/gpx.xsd\">\n");

	strftime(tm_buf, sizeof(tm_buf), "%Y-%m-%dT%H:%M:%SZ", tm);
	fprintf(fp_dest, "<time>%s</time>\n", tm_buf);
	fprintf(fp_dest, "<trk>\n");

	strftime(tm_buf, sizeof(tm_buf), "%Y%m%d_%H%M%S", tm);
	fprintf(fp_dest, "<name>omgps--%s", tm_buf);
	tt = end_time;
	tm = gmtime(&tt);
	strftime(tm_buf, sizeof(tm_buf), "%Y%m%d_%H%M%S", tm);
	fprintf(fp_dest, "-%s</name>\n", tm_buf);
	fprintf(fp_dest, "<trkseg>\n");

	while (TRUE) {
		n = fscanf(fp_src, "%lf\t%lf\t%u\n", &lat, &lon, &time_offset);
		if (n == EOF)
			break;
		else if (n != 3) {
			warn_dialog("Read src file failed");
			ret = FALSE;
			goto END;
		}
		tt = start_time + time_offset;
		tm = gmtime(&tt);
		strftime(tm_buf, sizeof(tm_buf), "%Y-%m-%dT%H:%M:%SZ", tm);
		fprintf(fp_dest, "<trkpt lat=\"%f\" lon=\"%f\"><time>%s</time></trkpt>\n",
				lat, lon, tm_buf);
	}
	fprintf(fp_dest, "</trkseg>\n</trk>\n</gpx>\n");

END:

	if (fp_src)
		fclose(fp_src);

	if (fp_dest)
		fclose(fp_dest);

	if (! ret)
		unlink(fullpath);
	else {
		char buf[256];
		snprintf(buf, sizeof(buf), "The .gpx file was exported as: %s", fullpath);
		info_dialog(buf);
	}
}

static void export_gpx_button_clicked(GtkWidget *widget, gpointer data)
{
	GtkTreeIter iter;
	char *file = get_selected_file(&iter);
	if (file) {
		export_gpx(file);
	} else {
		info_dialog("Export .gpx file: no file selected!");
	}
}

static void replay_filelist_treeview_row_selected (GtkTreeView *tree_view, gpointer user_data)
{
	if (! GTK_WIDGET_SENSITIVE(replay_button))
		gtk_widget_set_sensitive(replay_button, TRUE);

	if (! GTK_WIDGET_SENSITIVE(delete_button))
		gtk_widget_set_sensitive(delete_button, TRUE);

	if (! GTK_WIDGET_SENSITIVE(export_gpx_button))
		gtk_widget_set_sensitive(export_gpx_button, TRUE);
}

static GtkWidget * create_replay_pane()
{
	GtkWidget *vbox = gtk_vbox_new(FALSE, 5);

	/* file list treeview */

	filelist_treeview = gtk_tree_view_new ();
	gtk_tree_view_set_rules_hint (GTK_TREE_VIEW (filelist_treeview), TRUE);
	gtk_tree_view_set_reorderable(GTK_TREE_VIEW (filelist_treeview), FALSE);

	GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(filelist_treeview));
	gtk_tree_selection_set_mode (sel, GTK_SELECTION_SINGLE);

	g_signal_connect (G_OBJECT(filelist_treeview), "cursor-changed",
		G_CALLBACK (replay_filelist_treeview_row_selected), NULL);

	filelist_treeview_sw = new_scrolled_window (NULL);
	gtk_container_add (GTK_CONTAINER (filelist_treeview_sw), filelist_treeview);

	/* add columns to the tree view */
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *col;
	int i;
	int col_count = sizeof (filelist_treeview_col_names) / sizeof (char *);

	for (i=0; i<col_count; i++) {
		renderer = gtk_cell_renderer_text_new();
		col = gtk_tree_view_column_new_with_attributes (filelist_treeview_col_names[i],
			renderer, "text", i, NULL);
		if (i == 0) {
			gtk_tree_view_column_set_clickable(col, TRUE);
			gtk_tree_view_column_set_sort_order(col, GTK_SORT_DESCENDING);
			gtk_tree_view_column_set_sort_column_id(col, 0);
			gtk_tree_view_column_set_sort_indicator(col, TRUE);
		} else {
			gtk_tree_view_column_set_clickable(col, FALSE);
		}
		gtk_tree_view_append_column (GTK_TREE_VIEW(filelist_treeview), col);
	}

	filelist_store = gtk_list_store_new (col_count,
		G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);

	GtkTreeSortable *sortable = GTK_TREE_SORTABLE(filelist_store);
	gtk_tree_sortable_set_sort_column_id(sortable, 0, GTK_SORT_DESCENDING);

	gtk_tree_view_set_model(GTK_TREE_VIEW(filelist_treeview), GTK_TREE_MODEL(filelist_store));

	GtkWidget *hbox = gtk_hbox_new(TRUE, 5);

	replay_button = gtk_button_new_with_label("Replay");
	g_signal_connect (G_OBJECT(replay_button), "clicked",
		G_CALLBACK (replay_button_clicked), NULL);

	delete_button = gtk_button_new_with_label("Delete");
	g_signal_connect (G_OBJECT(delete_button), "clicked",
		G_CALLBACK (delete_button_clicked), NULL);

	export_gpx_button = gtk_button_new_with_label("Export .gpx");
	g_signal_connect (G_OBJECT(	export_gpx_button), "clicked",
		G_CALLBACK (export_gpx_button_clicked), NULL);

	gtk_container_add(GTK_CONTAINER(hbox), replay_button);
	gtk_container_add(GTK_CONTAINER(hbox), delete_button);
	gtk_container_add(GTK_CONTAINER(hbox), export_gpx_button);

	gtk_box_pack_start(GTK_BOX(vbox), filelist_treeview_sw, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

	/* populate files into filelist treeview */

	GtkTreeIter iter;
	struct dirent *ep;
	char buf[256];
	char *file_path = NULL;
	int len, ext_len = strlen(TRACK_FILE_EXT);
	char *fname;

	DIR *dp = opendir (g_context.track_dir);
	if (dp == NULL) {
		warn_dialog("unable to list track record files");
	} else {
		while ((ep = readdir (dp))) {
			fname = ep->d_name;
			len = strlen(fname);
			if (ep->d_type == DT_REG && len > ext_len &&
				(strncmp(&fname[len-ext_len], TRACK_FILE_EXT, ext_len) == 0)) {

				gtk_list_store_prepend (filelist_store, &iter);

				file_path = get_full_path(buf, sizeof(buf), fname);
				add_file_to_list(&iter, file_path, fname);

				if (replay_file && strcmp(replay_file, fname) == 0)
					gtk_tree_selection_select_iter (sel, &iter);
			}
		}
		closedir (dp);
	}

	return vbox;
}

GtkWidget * track_tab_create()
{
	track_init(g_context.track_enabled);

	GtkWidget *vbox = gtk_vbox_new(FALSE, 5);

	/* Track record */

	GtkWidget *button_hbox = gtk_hbox_new(FALSE, 5);
	gtk_box_pack_start(GTK_BOX(vbox), button_hbox, FALSE, FALSE, 5);

	new_track_button = gtk_button_new_with_label("New track");
	g_signal_connect (G_OBJECT(new_track_button), "clicked",
		G_CALLBACK (new_track_button_clicked), NULL);
	gtk_container_add (GTK_CONTAINER(button_hbox), new_track_button);

	stop_track_button = gtk_button_new_with_label("Stop track");
	g_signal_connect (G_OBJECT(stop_track_button), "clicked",
		G_CALLBACK (stop_track_button_clicked), NULL);
	gtk_container_add (GTK_CONTAINER(button_hbox), stop_track_button);

	/* color */
	GtkWidget *color_hbox = gtk_hbox_new(FALSE, 5);
	gtk_box_pack_start(GTK_BOX(vbox), color_hbox, FALSE, FALSE, 0);

	GtkWidget *color_label = gtk_label_new("Set line color: ");
	gtk_misc_set_alignment(GTK_MISC(color_label), 0.0, 0.5);
	gtk_box_pack_start (GTK_BOX(color_hbox), color_label, FALSE, FALSE, 5);

	create_colorlist();
	gtk_box_pack_start (GTK_BOX(color_hbox), colorlist, TRUE, TRUE, 5);

	change_color_button = gtk_button_new_with_label("Change");
	g_signal_connect (G_OBJECT (change_color_button), "clicked",
		G_CALLBACK (change_color_button_clicked), NULL);
	gtk_box_pack_start (GTK_BOX(color_hbox), change_color_button, TRUE, TRUE, 0);

	track_colorlist_set_initial_color();

	gtk_box_pack_start(GTK_BOX(vbox), create_replay_pane(), TRUE, TRUE, 5);

	return vbox;
}
