#include "customized.h"

/**
 * clickable GtkCellRendererPixbuf
 */

enum
{
	CLICKED,
	LAST_SIGNAL
};

typedef struct _ClickableCellRendererPixbuf
{
	GtkCellRendererPixbuf parent;
} ClickableCellRendererPixbuf;

typedef struct _ClickableCellRendererPixbufClass
{
	GtkCellRendererPixbufClass parent_class;
	void (* clicked) (ClickableCellRendererPixbuf *cell_renderer, const gchar *path);
} ClickableCellRendererPixbufClass;

static guint cell_signals[LAST_SIGNAL] = { 0 };
static GType cell_type = 0;

static gint clickable_cell_renderer_pixbuf_activate(GtkCellRenderer *cell, GdkEvent *event,
		GtkWidget *widget, const gchar *path, GdkRectangle *background_area,
		GdkRectangle *cell_area, GtkCellRendererState flags);
static void clickable_cell_renderer_pixbuf_init(ClickableCellRendererPixbuf *cellpixbuf);
static void clickable_cell_renderer_pixbuf_class_init(ClickableCellRendererPixbufClass *cls);

static const GTypeInfo cell_info = {
	sizeof(ClickableCellRendererPixbufClass),
	NULL, NULL,
	(GClassInitFunc) clickable_cell_renderer_pixbuf_class_init,
	NULL, NULL,	sizeof(ClickableCellRendererPixbuf), 0,
	(GInstanceInitFunc) clickable_cell_renderer_pixbuf_init
};

GType clickable_cell_renderer_pixbuf_get_type(void)
{
	if (cell_type != 0)
		return cell_type;

	cell_type = g_type_register_static(GTK_TYPE_CELL_RENDERER_PIXBUF,
			"ClickableCellRendererPixbuf", &cell_info, 0);
	return cell_type;
}

static void clickable_cell_renderer_pixbuf_init(ClickableCellRendererPixbuf *cell)
{
	GtkCellRenderer *renderer = GTK_CELL_RENDERER(&(cell->parent));
	renderer->mode = GTK_CELL_RENDERER_MODE_ACTIVATABLE;
}

static gint clickable_cell_renderer_pixbuf_activate(GtkCellRenderer *cell, GdkEvent *event,
	GtkWidget *widget, const gchar *path, GdkRectangle *background_area,
		GdkRectangle *cell_area, GtkCellRendererState flags)
{
	// FIXME: check the clicked area
	g_signal_emit(cell, cell_signals[CLICKED], 0, path);
	return FALSE;
}

static void clickable_cell_renderer_pixbuf_class_init(ClickableCellRendererPixbufClass *cls)
{
	GtkCellRendererClass *cell_class = GTK_CELL_RENDERER_CLASS(cls);
	GObjectClass *object_class = G_OBJECT_CLASS(cls);
	cell_class->activate = clickable_cell_renderer_pixbuf_activate;
	cell_signals[CLICKED] = g_signal_new(g_intern_static_string("clicked"),
			G_OBJECT_CLASS_TYPE (object_class), G_SIGNAL_RUN_LAST,
			G_STRUCT_OFFSET (ClickableCellRendererPixbufClass, clicked), NULL, NULL,
			gtk_marshal_VOID__STRING, G_TYPE_NONE, 1, G_TYPE_STRING);
}

GtkCellRenderer *clickable_cell_renderer_pixbuf_new(void)
{
	return g_object_new(clickable_cell_renderer_pixbuf_get_type(), NULL);
}
