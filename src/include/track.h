#ifndef TRACK_H_
#define TRACK_H_

#define TRACK_HEAD_LABEL_1 "start time: "
#define TRACK_HEAD_LABEL_2 "end time: "
#define TRACK_HEAD_LABEL_3 "record count: "

/* FIXME: configurable */
#define TRACK_MAX_PACC 				20

/* unit: meter */
#define TRACK_MAX_DELTA 			2

#define TRACK_MAX_IN_MEM_RECORDS	300

typedef struct __trackpoint_t
{
	int id;

	/* without position fix */
	coord_t wgs84;

	float height;

	/* time offset to start time, second.
	 * This means the minimal GPS fetch interval is limited to 1 second */
	U4 time_offset;

	/* global */
	point_t pixel;
	gboolean inview;

} trackpoint_t;

typedef struct __track_group_t
{
	/* include those on-disk */
	int total_count;

	time_t starttime; /* System time */

	/* count into track point array */
	int count;
	trackpoint_t tps[TRACK_MAX_IN_MEM_RECORDS];

	/* incremental draw index -- global index */
	int last_drawn_index;
} track_group_t;

extern GtkWidget* ctx_tab_track_replay_create();
extern void ctx_tab_track_replay_on_show();

extern GtkWidget * track_tab_create();
extern void track_tab_on_show();

extern char *get_cur_replay_filepath();
extern void track_replay_cleanup();

extern void track_add(/*double lat, double lon, U4 gps_tow*/);
extern void track_cleanup();
extern gboolean track_save(gboolean all, gboolean free);
extern void track_draw(GdkPixmap *canvas, gboolean refresh, GdkRectangle *rect);
extern void track_replay_centralize();

#endif /* TRACK_H_ */
