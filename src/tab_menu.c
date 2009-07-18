#include "omgps.h"
#include "dbus_intf.h"
#include "gps.h"
#include "sound.h"
#include "util.h"
#include "track.h"
#include "customized.h"

static GtkWidget *start_button, *stop_button, *run_status_label;
static GtkWidget *notebook;

#define MENU_COUNT 6
static TAB_ID_T menu_ids[MENU_COUNT] = {
	TAB_ID_GPS_CFG, TAB_ID_NAV_DATA, TAB_ID_TRACK, TAB_ID_SCRATCH, TAB_ID_MAP_TILE, TAB_ID_SOUND
};
static GtkWidget *menu_buttons[MENU_COUNT];
static GtkWidget *ctx_box;
static CTX_ID_T cur_ctx_id = CTX_ID_NONE;

#define BUTTON_LABEL(bt) \
	gtk_bin_get_child(GTK_BIN(&((GtkButton *)bt)->bin))

#define MENU_SEP "<span foreground=\"#A9A9A9\">&gt;</span>"

void update_tab_on_poll_state_changed()
{
	if (g_tab_id == TAB_ID_MAIN_VIEW || g_tab_id == TAB_ID_MAIN_MENU ||
		g_tab_id == TAB_ID_GPS_CFG || g_tab_id == TAB_ID_TRACK) {
		(g_menus[g_tab_id].on_show)();
	}
}

void update_ui_on_zoom_level_changed()
{
	if (ctx_tab_get_current_id() == CTX_ID_DL_TILES)
		dl_tiles_update_buttons_on_zoom_changed();
}

void menu_tab_on_show()
{
	char buf[80];
	char *run_state = "";
	if (POLL_STATE_TEST(RUNNING)) {
		run_state = "running";
		if (! GTK_WIDGET_SENSITIVE(stop_button))
			gtk_widget_set_sensitive(stop_button, TRUE);
	} else if (POLL_STATE_TEST(SUSPENDING)) {
		run_state = "disconnected";
		if (! GTK_WIDGET_SENSITIVE(start_button))
			gtk_widget_set_sensitive(start_button, TRUE);
	}

	snprintf(buf, sizeof(buf), " GPS status: <span color='red'>%s</span>", run_state);
	gtk_label_set_markup(GTK_LABEL(run_status_label), buf);

	/* nav data */
	gtk_widget_set_sensitive(menu_buttons[1], POLL_STATE_TEST(RUNNING));
}

static void menu_item_button_clicked(GtkWidget *widget, gpointer data)
{
	TAB_ID_T id = (TAB_ID_T)data;
	switch_to_tab(id);
}

static void poll_button_clicked(GtkWidget *widget, gpointer data)
{
	gboolean is_start_bt = (gboolean)data;

	if (POLL_STATE_TEST(RUNNING) == is_start_bt)
		return;

	if (!is_start_bt && !confirm_dialog("Stop GPS?"))
		return;

	if (is_start_bt)
		gtk_widget_set_sensitive(start_button, FALSE);
	else
		gtk_widget_set_sensitive(stop_button, FALSE);

	if (is_start_bt) {
		gtk_label_set_markup(GTK_LABEL(run_status_label),
			" GPS status: <span color='red'>connecting...</span>");
		status_label_set_text("<span color='red'>GPS is connecting...</span>", TRUE);
	} else {
		status_label_set_text("", FALSE);
	}

	notify_poll_thread_suspend_resume();
}

GtkWidget * menu_tab_create()
{
	GtkWidget *vbox = gtk_vbox_new(FALSE, 5);

	/* system status */

	GtkWidget *status_hbox = gtk_hbox_new(TRUE, 10);

	run_status_label = gtk_label_new("");
	gtk_misc_set_alignment(GTK_MISC(run_status_label), 0.0, 0.5);

	gtk_container_add (GTK_CONTAINER (status_hbox), run_status_label);

	GtkWidget *poll_hbox = gtk_hbox_new(TRUE, 5);
	gtk_container_add (GTK_CONTAINER (status_hbox), poll_hbox);

	start_button = gtk_button_new_with_label("Start");
	g_signal_connect (G_OBJECT (start_button), "clicked", G_CALLBACK (poll_button_clicked), (gpointer)TRUE);
	gtk_container_add (GTK_CONTAINER (poll_hbox), start_button);
	if (g_context.run_gps_on_start)
		gtk_widget_set_sensitive(start_button, FALSE);

	stop_button = gtk_button_new_with_label("Stop");
	g_signal_connect (G_OBJECT (stop_button), "clicked", G_CALLBACK (poll_button_clicked), (gpointer)FALSE);
	gtk_container_add (GTK_CONTAINER (poll_hbox), stop_button);
	gtk_widget_set_sensitive(stop_button, FALSE);

	gtk_box_pack_start (GTK_BOX (vbox), status_hbox, FALSE, FALSE, 0);

	GtkWidget *hs;
	int i;

	GtkWidget *gps_runtime_vbox = gtk_vbox_new(FALSE, 0);

	gtk_box_pack_start (GTK_BOX (vbox), gps_runtime_vbox, FALSE, FALSE, 0);

	hs = gtk_hseparator_new();
	gtk_box_pack_start (GTK_BOX (vbox), hs, FALSE, FALSE, 3);

	/* Other menu buttons */

	GtkWidget *table = gtk_table_new((int)ceil(TAB_ID_MAX / 2.0), 2, TRUE);
	gtk_table_set_row_spacings(GTK_TABLE(table), 15);
	gtk_table_set_col_spacings(GTK_TABLE(table), 10);

	menu_item_t *menu;
	int x, y;

	for (i=0; i<sizeof(menu_ids)/sizeof(TAB_ID_T); i++) {
		x = i % 2;
		y = i / 2;
		menu = &g_menus[menu_ids[i]];
		menu_buttons[i] = gtk_button_new_with_label(menu->name);
		gtk_table_attach_defaults(GTK_TABLE(table), menu_buttons[i], x, x + 1, y, y + 1);
		g_signal_connect (G_OBJECT(menu_buttons[i]), "clicked",
			G_CALLBACK (menu_item_button_clicked), (gpointer)menu_ids[i]);
	}

	gtk_box_pack_start (GTK_BOX (vbox), table, FALSE, FALSE, 0);

	GtkWidget *site_label = gtk_label_new(HOME_PAGE);
	gtk_label_set_selectable(GTK_LABEL(site_label), FALSE);
	gtk_widget_modify_fg(site_label, GTK_STATE_NORMAL, &g_base_colors[ID_COLOR_White]);
	gtk_misc_set_alignment(GTK_MISC(site_label), 0.5, 0);
	gtk_box_pack_start (GTK_BOX (vbox), site_label, FALSE, FALSE, 10);

	return vbox;
}

/******************** context tabs ****************************/

static void register_menu_tab(TAB_ID_T id, char *name, menu_create_func_t create, menu_show_func_t show)
{
	g_menus[id].name = name;
	g_menus[id].id = id;
	g_menus[id].create = create;
	g_menus[id].on_show = show;
}

static void register_ctx_tab(int id, menu_create_func_t create, menu_show_func_t show)
{
	g_ctx_panes[id].create = create;
	g_ctx_panes[id].on_show = show;
}

static gboolean page_link_clicked(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
	if (event->button == 1) {
		TAB_ID_T id = (TAB_ID_T)data;
		switch_to_tab(id);
	}
	return FALSE;
}

static void init_panes()
{
	/* Tab'd window */
	notebook = gtk_notebook_new();
	gtk_container_add(GTK_CONTAINER (g_window), notebook);
	gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), FALSE);
	gtk_notebook_set_show_border(GTK_NOTEBOOK(notebook), FALSE);
	gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook), FALSE);

	int i;
	GtkWidget *vbox, *hbox, *label, *sp, *link, *widget;
	TAB_ID_T id;

	PangoFontDescription *font_desc = pango_font_description_from_string (
#if (PLATFORM_FSO)
			"Sans Bold 4"
#else
			"Sans Bold 14"
#endif
	);

	for (i=0; i<TAB_ID_MAX; i++) {
		id = g_menus[i].id;
		widget = (*(g_menus[i].create))();
		if (id == TAB_ID_MAIN_VIEW) {
			gtk_notebook_append_page (GTK_NOTEBOOK (notebook), widget, NULL);
		} else {
			vbox = gtk_vbox_new(FALSE, 0);
			hbox = gtk_hbox_new(FALSE, 5);

			link = hyperlink_label_new(g_menus[TAB_ID_MAIN_VIEW].name, font_desc,
				&page_link_clicked, (gpointer)TAB_ID_MAIN_VIEW);

			label = gtk_label_new("");
			gtk_widget_modify_font(label, font_desc);
			gtk_label_set_markup(GTK_LABEL(label), MENU_SEP);

			gtk_box_pack_start(GTK_BOX(hbox), link, FALSE, FALSE, 5);
			gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);

			if (id != TAB_ID_MAIN_MENU) {
				link = hyperlink_label_new(g_menus[TAB_ID_MAIN_MENU].name, font_desc,
					&page_link_clicked, (gpointer)TAB_ID_MAIN_MENU);

				label = gtk_label_new("");
				gtk_widget_modify_font(label, font_desc);
				gtk_label_set_markup(GTK_LABEL(label), MENU_SEP);

				gtk_box_pack_start(GTK_BOX(hbox), link, FALSE, FALSE, 0);
				gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
			}

			label = gtk_label_new(g_menus[i].name);
			gtk_widget_modify_font(label, font_desc);
			gtk_misc_set_alignment(GTK_MISC (label), 0.0, 0.0);
			gtk_box_pack_start(GTK_BOX(hbox), label, TRUE, TRUE, 0);

			sp = gtk_hseparator_new();
			gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 5);
			gtk_box_pack_start(GTK_BOX(vbox), sp, FALSE, FALSE, 0);
			gtk_box_pack_start(GTK_BOX(vbox), widget, TRUE, TRUE, 0);

			gtk_notebook_append_page (GTK_NOTEBOOK (notebook), vbox, NULL);
		}
	}

	pango_font_description_free (font_desc);

	for (i=0; i<CTX_ID_MAX; i++) {
		widget = (*(g_ctx_panes[i].create))();
		g_ctx_containers[i] = widget;
		ctx_tab_container_add(widget);
	}

	g_context.map_view_frozen = FALSE;

	if (! map_init()) {
		warn_dialog("initialize map module failed");
		exit(0);
	}

	g_init_status = MAP_INITED;

	/* show window */
	gtk_widget_show_all(notebook);

	for (i=0; i<CTX_ID_MAX; i++)
		gtk_widget_hide(g_ctx_containers[i]);
}

void register_ui_panes()
{
	register_menu_tab(TAB_ID_MAIN_VIEW,
		"Main View",
		&view_tab_create,
		&view_tab_on_show);
	register_menu_tab(TAB_ID_MAIN_MENU,
		"Main Menu",
		&menu_tab_create,
		&menu_tab_on_show);
	register_menu_tab(TAB_ID_GPS_CFG,
		"GPS Config",
		&agps_tab_create,
		&agps_tab_on_show);
	register_menu_tab(TAB_ID_MAP_TILE,
		"Map Tile",
		&tile_tab_create,
		&tile_tab_on_show);
	register_menu_tab(TAB_ID_NAV_DATA,
		"GPS Data",
		&nav_tab_create,
		&nav_tab_on_show);
	register_menu_tab(TAB_ID_TRACK,
		"Track",
		&track_tab_create,
		&track_tab_on_show);
	register_menu_tab(TAB_ID_SCRATCH,
		"Scratch",
		&scratch_tab_create,
		&scratch_tab_on_show);
	register_menu_tab(TAB_ID_SOUND,
		"Sound",
		&sound_tab_create,
		&sound_tab_on_show);

	register_ctx_tab(CTX_ID_GPS_FIX,
		&ctx_tab_gps_fix_create,
		&ctx_tab_gps_fix_on_show);
	register_ctx_tab(CTX_ID_AGPS_ONLINE,
		&ctx_tab_agps_online_create,
		&ctx_tab_agps_online_on_show);
	register_ctx_tab(CTX_ID_DL_TILES,
		&ctx_tab_dl_tiles_create,
		&ctx_tab_dl_tiles_on_show);
	register_ctx_tab(CTX_ID_SCRATCH,
		&ctx_tab_scratch_create,
		&ctx_tab_scratch_on_show);
	register_ctx_tab(CTX_ID_FIX_MAP,
		&ctx_tab_fix_map_create,
		&ctx_tab_fix_map_on_show);
	register_ctx_tab(CTX_ID_TRACK_REPLAY,
		&ctx_tab_track_replay_create,
		&ctx_tab_track_replay_on_show);

	init_panes();
}

GtkWidget* ctx_tab_container_create()
{
    ctx_box = gtk_vbox_new(FALSE, 0);
	return ctx_box;
}

void ctx_tab_container_add(GtkWidget *tab)
{
	gtk_container_add(GTK_CONTAINER(ctx_box), tab);
}

CTX_ID_T ctx_tab_get_current_id()
{
	return cur_ctx_id;
}

void switch_to_ctx_tab(CTX_ID_T ctx_id)
{
	if (cur_ctx_id != CTX_ID_NONE)
		gtk_widget_hide(g_ctx_containers[cur_ctx_id]);

	if (ctx_id == CTX_ID_NONE) {
		cur_ctx_id = CTX_ID_NONE;
		return;
	}

	if (ctx_id == CTX_ID_GPS_FIX) {
		map_toggle_menu_button(TRUE);
		if (POLL_STATE_TEST(SUSPENDING)) {
			cur_ctx_id = CTX_ID_NONE;
			return;
		}
	} else {
		map_toggle_menu_button(FALSE);
	}

	gtk_widget_show(g_ctx_containers[ctx_id]);
	g_ctx_panes[ctx_id].on_show();
	cur_ctx_id = ctx_id;
}

void switch_to_tab(TAB_ID_T page_num)
{
	if (g_tab_id == page_num)
		return;

	gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), page_num);
	g_tab_id = page_num;

	(*(g_menus[page_num].on_show))();
}

void switch_to_main_view(CTX_ID_T ctx_id)
{
	switch_to_ctx_tab(ctx_id);

	if (g_tab_id != TAB_ID_MAIN_VIEW)
		switch_to_tab(TAB_ID_MAIN_VIEW);
}
