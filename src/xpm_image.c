#include "xpm_image.h"
#include "util.h"

/* These images are critical to program, and they are fairly small.
 * Include them into ELF file. */

#include "xpm/track_replay_flag.xpm"

#include "xpm/track_on.xpm"
#include "xpm/track_off.xpm"

#include "xpm/position_valid.xpm"
#include "xpm/position_invalid.xpm"

#include "xpm/position_heading.xpm"

#include "xpm/yes.xpm"
#include "xpm/downloading.xpm"
#include "xpm/download_cancel.xpm"

#include "xpm/poll_engine_ubx.xpm"
#include "xpm/poll_engine_fso.xpm"

#include "xpm/fixmap_src.xpm"
#include "xpm/fixmap_dest.xpm"

#include "xpm/sv_in_use.xpm"
#include "xpm/sv_signal.xpm"
#include "xpm/sv_no_signal.xpm"

#include "xpm/letter_0.xpm"
#include "xpm/letter_1.xpm"
#include "xpm/letter_2.xpm"
#include "xpm/letter_3.xpm"
#include "xpm/letter_4.xpm"
#include "xpm/letter_5.xpm"
#include "xpm/letter_6.xpm"
#include "xpm/letter_7.xpm"
#include "xpm/letter_8.xpm"
#include "xpm/letter_9.xpm"
#include "xpm/letter_dot.xpm"
#include "xpm/letter_minus.xpm"
#include "xpm/letter_slash.xpm"
#include "xpm/unit_m.xpm"
#include "xpm/unit_mps.xpm"
#include "xpm/unit_mph.xpm"
#include "xpm/unit_kmph.xpm"

static void xpm_reg(GdkDrawable *drawable, XPM_ID_T id, gchar *data[])
{
	GdkBitmap* mask;
	GdkColor color;
	gdk_color_parse("#FFFFFF", &color);

	g_xpm_images[id].pixbuf = NULL;

	GdkPixmap *pixmap = gdk_pixmap_create_from_xpm_d(drawable, &mask, &color, data);

	int w, h;
	gdk_drawable_get_size(pixmap, &w, &h);

	GdkPixbuf *pixbuf = gdk_pixbuf_get_from_drawable (NULL, pixmap, NULL, 0, 0, 0, 0, w, h);

	g_object_unref(pixmap);

	if (! pixbuf)
		return;

	g_xpm_images[id].pixbuf = gdk_pixbuf_add_alpha (pixbuf, TRUE,
		color.red >> 8, color.green >> 8, color.blue >> 8);
	g_xpm_images[id].width = w;
	g_xpm_images[id].height = h;

	g_object_unref(pixbuf);
}

void xpm_image_init(GdkDrawable *drawable)
{
	xpm_reg(drawable, XPM_ID_TRACK_REPLAY_FLAG,	track_replay_flag_xpm);
	xpm_reg(drawable, XPM_ID_POSITION_VALID,	position_valid_xpm);
	xpm_reg(drawable, XPM_ID_POSITION_INVALID,	position_invalid_xpm);
	xpm_reg(drawable, XPM_ID_POSITION_HEADING,	position_heading_xpm);
	xpm_reg(drawable, XPM_ID_YES,				yes_xpm);
	xpm_reg(drawable, XPM_ID_DOWNLOADING,		downloading_xpm);
	xpm_reg(drawable, XPM_ID_DOWNLOAD_CANCEL,	download_cancel_xpm);
	xpm_reg(drawable, XPM_ID_POLLENGINE_UBX,	poll_engine_ubx_xpm);
	xpm_reg(drawable, XPM_ID_POLLENGINE_FSO,	poll_engine_fso_xpm);
	xpm_reg(drawable, XPM_ID_TRACK_ON,			track_on_xpm);
	xpm_reg(drawable, XPM_ID_TRACK_OFF,			track_off_xpm);
	xpm_reg(drawable, XPM_ID_FIXMAP_SRC,		fixmap_src_xpm);
	xpm_reg(drawable, XPM_ID_FIXMAP_DEST,		fixmap_dest_xpm);
	xpm_reg(drawable, XPM_ID_SV_IN_USE,			sv_in_use_xpm);
	xpm_reg(drawable, XPM_ID_SV_SIGNAL,			sv_signal_xpm);
	xpm_reg(drawable, XPM_ID_SV_NO_SIGNAL,		sv_no_signal_xpm);

	xpm_reg(drawable, XPM_ID_LETTER_0,			letter_0_xpm);
	xpm_reg(drawable, XPM_ID_LETTER_1,			letter_1_xpm);
	xpm_reg(drawable, XPM_ID_LETTER_2,			letter_2_xpm);
	xpm_reg(drawable, XPM_ID_LETTER_3,			letter_3_xpm);
	xpm_reg(drawable, XPM_ID_LETTER_4,			letter_4_xpm);
	xpm_reg(drawable, XPM_ID_LETTER_5,			letter_5_xpm);
	xpm_reg(drawable, XPM_ID_LETTER_6,			letter_6_xpm);
	xpm_reg(drawable, XPM_ID_LETTER_7,			letter_7_xpm);
	xpm_reg(drawable, XPM_ID_LETTER_8,			letter_8_xpm);
	xpm_reg(drawable, XPM_ID_LETTER_9,			letter_9_xpm);
	xpm_reg(drawable, XPM_ID_LETTER_dot,		letter_dot_xpm);
	xpm_reg(drawable, XPM_ID_LETTER_minus,		letter_minus_xpm);
	xpm_reg(drawable, XPM_ID_LETTER_slash,		letter_slash_xpm);
	xpm_reg(drawable, XPM_ID_UNIT_m,			unit_m_xpm);
	xpm_reg(drawable, XPM_ID_UNIT_mps,			unit_mps_xpm);
	xpm_reg(drawable, XPM_ID_UNIT_mph,			unit_mph_xpm);
	xpm_reg(drawable, XPM_ID_UNIT_kmph,			unit_kmph_xpm);
}

void xpm_image_cleanup()
{
	int i;
	for (i=0; i<XPM_ID_MAX; i++) {
		if (g_xpm_images[i].pixbuf != NULL) {
			g_object_unref(g_xpm_images[i].pixbuf);
			g_xpm_images[i].pixbuf = NULL;
		}
	}
}
