#ifndef TILE_CONFIG_H_
#define TILE_CONFIG_H_

#include <glib.h>
#include <gdk/gdk.h>
#include <Python.h>

#define MAP_MAX_BG_COLORS	5
#define MAX_ZOOM_LEVELS		30

typedef struct __map_repo_t
{
	char *name;
	int min_zoom;
	int max_zoom;
	char *image_type;
	PyObject *urlfunc;

	/* additional runtime data */

	char *dir;

	/* from/to settings.txt */
	int zoom;
	float lat_fix;
	float lon_fix;

	void *downloader;

} map_repo_t;

typedef void (*iterate_maplist_func)(map_repo_t *repo, void *arg);

extern gboolean mapcfg_load();
extern void mapcfg_cleanup();
extern map_repo_t *mapcfg_get_repo(char *map_name);
extern map_repo_t *mapcfg_get_default_repo(char *map_name);
extern void mapcfg_iterate_maplist(iterate_maplist_func f, void *arg);
extern map_repo_t *mapcfg_get_ith_repo(int ith);
extern char *mapcfg_get_dl_url(map_repo_t *repo, int zoom, int x, int y);

#endif /* TILE_CONFIG_H_ */
