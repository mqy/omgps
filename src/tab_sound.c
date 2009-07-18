#define _ATFILE_SOURCE
#include <fcntl.h> /* Definition of AT_* constants */
#include <unistd.h>
#include <dirent.h>

#include "omgps.h"
#include "sound.h"
#include "util.h"
#include "customized.h"

static GtkWidget *enable_sound_button = NULL, *file_list = NULL, *change_button, *cur_file_label;
static int last_idx = 0;

void sound_tab_on_show()
{
	if (enable_sound_button == NULL)
		return;

	if (g_context.sound_enabled != gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(enable_sound_button))) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enable_sound_button), g_context.sound_enabled);
	}

	char *file = gtk_combo_box_get_active_text(GTK_COMBO_BOX(file_list));
	if (! file)
		return;

	gboolean changed = (! g_cfg->last_sound_file || strcmp(g_cfg->last_sound_file, file) != 0);
	gtk_widget_set_sensitive(change_button, changed);
}

static void enable_sound_button_toggled(GtkWidget *widget, gpointer data)
{
	g_context.sound_enabled = ! g_context.sound_enabled;
	toggle_sound(g_context.sound_enabled);
}

static void change_button_clicked(GtkWidget *widget, gpointer data)
{
	int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(file_list));
	char *file = gtk_combo_box_get_active_text(GTK_COMBO_BOX(file_list));
	if (idx == 0 || ! file) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enable_sound_button), FALSE);
	} else {
		if (set_plugin(file)) {
			g_cfg->last_sound_file = file;
			last_idx = idx;
			gtk_label_set_text(GTK_LABEL(cur_file_label), file);
		} else {
			gtk_combo_box_set_active(GTK_COMBO_BOX(file_list), last_idx);
			warn_dialog("Set sound config file failed: please check the file");
		}
	}
	gtk_widget_set_sensitive(change_button, FALSE);
}

static void file_list_changed (GtkComboBox *widget, gpointer user_data)
{
	int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(file_list));
	char *file = gtk_combo_box_get_active_text(GTK_COMBO_BOX(file_list));
	if (! file || idx == 0) {
		gtk_widget_set_sensitive(change_button, TRUE);
		return;
	}
	gboolean changed = (! g_cfg->last_sound_file || strcmp(g_cfg->last_sound_file, file) != 0);
	gtk_widget_set_sensitive(change_button, changed);
}

static void add_sound_files(GtkWidget *vbox, DIR *dp)
{
	int i = 1, len;
	struct dirent *ep;
	struct stat st;
	char *file, *p;

	enable_sound_button = gtk_check_button_new_with_label("Enable sound");
	g_signal_connect (G_OBJECT (enable_sound_button), "toggled",
		G_CALLBACK (enable_sound_button_toggled), NULL);
	gtk_box_pack_start (GTK_BOX (vbox), enable_sound_button, FALSE, FALSE, 10);

	cur_file_label = gtk_label_new("");
	gtk_misc_set_alignment(GTK_MISC(cur_file_label), 0.0, 0.5);

	GtkWidget *hbox = gtk_hbox_new(FALSE, 0);

	file_list =  gtk_combo_box_new_text();

	change_button = gtk_button_new_with_label("Change");
	gtk_widget_set_sensitive(change_button, FALSE);
	g_signal_connect (G_OBJECT (change_button), "clicked", G_CALLBACK (change_button_clicked), NULL);

	gtk_combo_box_append_text (GTK_COMBO_BOX(file_list), "-- please select --");
	gtk_combo_box_set_active (GTK_COMBO_BOX(file_list), 0);

	char buf[256], buf1[256];
	/* sound.py */
	while ((ep = readdir (dp))) {
		if (ep->d_type != DT_LNK && ep->d_type != DT_REG)
			continue;

		p = strstr(ep->d_name, ".py");
		if (! p || strlen(p) != 3)
			continue;

		file = ep->d_name;

		snprintf(buf, sizeof(buf), "%s/%s", OMGPS_SOUND_TOP_DIR, file);

		if (ep->d_type == DT_LNK) {
			len = readlink(buf, buf1, sizeof(buf1));
			if (len <= 0 || len == sizeof(buf1))
				continue;
			buf1[len] = '\0';
			if (stat(buf1, &st) != 0 || ! S_ISREG(st.st_mode))
				continue;
		} else if (ep->d_type != DT_REG) {
			continue;
		}

		gtk_combo_box_append_text (GTK_COMBO_BOX(file_list), file);

		snprintf(buf1, sizeof(buf), "%s/%s", g_context.config_dir, file);
		symlink(buf, buf1);

		gboolean is = (g_cfg->last_sound_file && strcmp(g_cfg->last_sound_file, file) == 0);
		if (is) {
			gtk_label_set_text(GTK_LABEL(cur_file_label), file);
			gtk_combo_box_set_active (GTK_COMBO_BOX(file_list), i);
			last_idx = i;
		}
		++i;
	}
	closedir (dp);


	/* NOTE: gtk_combo_box_append_text() triggers "changed" event */
	g_signal_connect (G_OBJECT (file_list), "changed", G_CALLBACK (file_list_changed), NULL);

	GtkWidget *hbox0 = gtk_hbox_new(FALSE, 0);

	GtkWidget *label0 = gtk_label_new("Current file: ");
	gtk_misc_set_alignment(GTK_MISC(label0), 0.0, 0.5);

	gtk_box_pack_start (GTK_BOX (hbox0), label0, FALSE, FALSE, 3);
	gtk_box_pack_start (GTK_BOX (hbox0), cur_file_label, TRUE, TRUE, 0);

	gtk_box_pack_start(GTK_BOX(hbox), file_list, TRUE, TRUE, 3);
	gtk_box_pack_start(GTK_BOX(hbox), change_button, FALSE, FALSE, 0);

	gtk_box_pack_start (GTK_BOX (vbox), hbox0, FALSE, FALSE, 5);

	gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 5);
}

GtkWidget * sound_tab_create()
{
	GtkWidget *vbox = gtk_vbox_new(FALSE, 0);

	GtkWidget *desc = gtk_label_new(" Sound for speed, weak signal, and low power");
	gtk_label_set_selectable(GTK_LABEL(desc), FALSE);
	gtk_widget_modify_fg(desc, GTK_STATE_NORMAL, &g_base_colors[ID_COLOR_White]);
	gtk_misc_set_alignment(GTK_MISC(desc), 0.0, 0.5);

	gtk_box_pack_start (GTK_BOX (vbox), desc, FALSE, FALSE, 10);

	DIR *dp = opendir (OMGPS_SOUND_TOP_DIR);

	if (dp == NULL) {
		GtkWidget *tip = gtk_label_new("");
		char buf[128];
		snprintf(buf, sizeof(buf), " No sound module in %s/, \n Get it from %s", OMGPS_SOUND_TOP_DIR, HOME_PAGE);
		gtk_label_set_text(GTK_LABEL(tip), buf);
		gtk_label_set_selectable(GTK_LABEL(tip), FALSE);
		gtk_misc_set_alignment(GTK_MISC(tip), 0.0, 0.5);
		gtk_box_pack_start (GTK_BOX (vbox), tip, FALSE, FALSE, 5);
	} else {
		add_sound_files(vbox, dp);
	}

	return vbox;
}
