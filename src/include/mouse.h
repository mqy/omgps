#ifndef MOUSE_H_
#define MOUSE_H_

#include <glib.h>
#include <gtk/gtk.h>

typedef void (*mouse_event_handler_t)(point_t pt, guint time);

typedef struct __mouse_handler_t
{
	mouse_event_handler_t press_handler; // can be NULL
	mouse_event_handler_t release_handler;  // can be NULL
	mouse_event_handler_t motion_handler; // can be NULL
} mouse_handler_t;

extern void drawingarea_reset_default_mouse_handler();

extern void drawingarea_set_default_mouse_handler(mouse_handler_t *handler);

extern void drawingarea_set_current_mouse_handler(mouse_handler_t *handler);

extern void drawingarea_init_mouse_handler(GtkWidget *widget, guint masks);

#endif /* MOUSE_H_ */
