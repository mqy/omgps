#ifndef XPM_IMAGE_H_
#define XPM_IMAGE_H_

#include <gdk/gdk.h>

typedef struct __xpm_t
{
	GdkPixbuf *pixbuf;
	int width;
	int height;
} xpm_t;

typedef enum
{
	XPM_ID_NONE = -1,
	XPM_ID_TRACK_REPLAY_FLAG = 0,
	XPM_ID_POSITION_VALID,
	XPM_ID_POSITION_INVALID,
	XPM_ID_POSITION_HEADING,
	XPM_ID_YES,
	XPM_ID_DOWNLOADING,
	XPM_ID_DOWNLOAD_CANCEL,
	XPM_ID_POLLENGINE_UBX,
	XPM_ID_POLLENGINE_FSO,
	XPM_ID_TRACK_ON,
	XPM_ID_TRACK_OFF,
	XPM_ID_FIXMAP_SRC,
	XPM_ID_FIXMAP_DEST,
	XPM_ID_SV_IN_USE,
	XPM_ID_SV_SIGNAL,
	XPM_ID_SV_NO_SIGNAL,

	XPM_ID_LETTER_0,
	XPM_ID_LETTER_1,
	XPM_ID_LETTER_2,
	XPM_ID_LETTER_3,
	XPM_ID_LETTER_4,
	XPM_ID_LETTER_5,
	XPM_ID_LETTER_6,
	XPM_ID_LETTER_7,
	XPM_ID_LETTER_8,
	XPM_ID_LETTER_9,
	XPM_ID_LETTER_dot,
	XPM_ID_LETTER_minus,
	XPM_ID_LETTER_slash,
	XPM_ID_UNIT_m,
	XPM_ID_UNIT_mps,
	XPM_ID_UNIT_mph,
	XPM_ID_UNIT_kmph,
	XPM_ID_MAX
} XPM_ID_T;

extern xpm_t g_xpm_images[XPM_ID_MAX];

extern void xpm_image_init(GdkDrawable *drawable);
extern void xpm_image_cleanup();

#endif /* XPM_IMAGE_H_ */
