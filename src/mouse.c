#include "omgps.h"

static mouse_handler_t *default_handler = NULL;
static mouse_handler_t *current_handler = NULL;

static gboolean mouse_press_handler(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
	if (current_handler && current_handler->press_handler) {
		if(event->button == 1) {
			point_t point = {(int)event->x, (int)event->y};
			current_handler->press_handler(point, event->time);
		}
	}
	return TRUE;
}

static gboolean mouse_release_handler(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
	if (current_handler && current_handler->release_handler) {
		if(event->button == 1) {
			point_t point = {(int)event->x, (int)event->y};
			current_handler->release_handler(point, event->time);
		}
	}
	return TRUE;
}

static gboolean mouse_motion_handler(GtkWidget *widget, GdkEventMotion *event, gpointer data)
{
	if (current_handler && current_handler->motion_handler) {
		point_t point;
		GdkModifierType state;
		gdk_window_get_pointer (event->window, &point.x, &point.y, &state);
		if (state & GDK_BUTTON1_MASK)
			current_handler->motion_handler(point, event->time);
	}
	return TRUE;
}

void drawingarea_set_default_mouse_handler(mouse_handler_t *h)
{
	current_handler = default_handler = h;
}

void drawingarea_set_current_mouse_handler(mouse_handler_t *h)
{
	current_handler = h;
}

void drawingarea_reset_default_mouse_handler()
{
	current_handler = default_handler;
}

void drawingarea_init_mouse_handler(GtkWidget *widget, guint masks)
{
	if (masks & GDK_BUTTON_PRESS_MASK) {
		g_signal_connect (widget, "button-press-event",
			G_CALLBACK (mouse_press_handler), NULL);
	}

	if (masks & GDK_BUTTON_RELEASE_MASK) {
		g_signal_connect (widget, "button-release-event",
			G_CALLBACK (mouse_release_handler), NULL);
	}

	if (masks & GDK_POINTER_MOTION_MASK) {
		g_signal_connect (widget, "motion-notify-event",
			G_CALLBACK (mouse_motion_handler), NULL);
	}
}
