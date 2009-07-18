#include <gtk/gtk.h>
#include <glib.h>

extern GtkWidget *g_window;

/**
 * font_desc: can be NULL
 */
GtkWidget* hyperlink_label_new(char *text, PangoFontDescription *font_desc,
	gboolean (*on_click)(GtkWidget *widget, GdkEventButton *event, gpointer data), gpointer data)
{
	char buf[256];
	GtkWidget *event_box = gtk_event_box_new();
	GtkWidget *label = gtk_label_new(NULL);

	if (font_desc)
		gtk_widget_modify_font(label, font_desc);

	snprintf(buf, sizeof(buf), "<span foreground=\"blue\" underline=\"single\">%s</span>", text);
	gtk_label_set_markup(GTK_LABEL(label), buf);

	gtk_container_add(GTK_CONTAINER (event_box), label);
	gtk_misc_set_alignment(GTK_MISC (label), 0.0, 0.0);
	gtk_event_box_set_above_child(GTK_EVENT_BOX (event_box), TRUE);

	gtk_widget_set_events(event_box, GDK_BUTTON_PRESS_MASK);
	g_signal_connect (G_OBJECT (event_box), "button_release_event",	G_CALLBACK (on_click), data);

	return event_box;
}

static inline void configure_dialog(GtkWidget *dialog)
{
	gtk_window_set_opacity(GTK_WINDOW(dialog), 0.5);
	GdkColor color;
	gdk_color_parse("#FFA500", &color);
	gtk_widget_modify_bg(GTK_WIDGET(dialog), GTK_STATE_NORMAL, &color);
	gtk_window_set_decorated(GTK_WINDOW(dialog), FALSE);
	gtk_window_set_keep_above (GTK_WINDOW(dialog), TRUE);

}

void warn_dialog(char *msg)
{
	GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(g_window),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_WARNING, GTK_BUTTONS_CLOSE, "%s", msg);
	configure_dialog(dialog);
	gtk_dialog_run(GTK_DIALOG (dialog));
	gtk_widget_destroy(dialog);
}

void info_dialog(char *msg)
{
	GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(g_window),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", msg);
	configure_dialog(dialog);
	gtk_dialog_run(GTK_DIALOG (dialog));
	gtk_widget_destroy(dialog);
}

gboolean confirm_dialog(char *msg)
{
	GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(g_window),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL, "%s", msg);
	configure_dialog(dialog);
	int ret = gtk_dialog_run(GTK_DIALOG (dialog));
	gtk_widget_destroy(dialog);
	return (ret == GTK_RESPONSE_OK);
}

void modify_button_color(GtkButton *button, GdkColor *color, gboolean is_fg)
{
	int i;

	GtkWidget *child = NULL;
	if (is_fg)
		child = gtk_bin_get_child(GTK_BIN(button));
	for (i=GTK_STATE_NORMAL; i<=GTK_STATE_INSENSITIVE; i++) {
		if (is_fg)
			gtk_widget_modify_fg(child, i, color);
		else
			gtk_widget_modify_bg(GTK_WIDGET(button), i, color);
	}
}

GtkWidget *new_scrolled_window(GtkWidget *viewport_child)
{
	GtkWidget *sw = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (sw), GTK_SHADOW_NONE);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw),
			GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

	if (viewport_child != NULL) {
		GtkWidget *viewport = gtk_viewport_new(NULL, NULL);
		gtk_viewport_set_shadow_type(GTK_VIEWPORT (viewport), GTK_SHADOW_NONE);
		gtk_container_add(GTK_CONTAINER(sw), viewport);
		gtk_container_add(GTK_CONTAINER(viewport), viewport_child);
	}

	return sw;
}
