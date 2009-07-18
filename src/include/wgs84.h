#ifndef WGS84_H_
#define WGS84_H_

#include "map_repo.h"

/* known maps use 256-pixel tiles */
#define TILE_SIZE					256

#define WGS84_SEMI_MAJOR_AXIS		6378137
#define WGS84_FLATENING_FACTOR		(1.0 / 298.257223563)

typedef struct __point_t
{
	int x;
	int y;
} point_t;

typedef struct __coord_t
{
	double lat;
	double lon;
} coord_t;

typedef struct _llh_ecef_t
{
	double lat;
	double lon;
	double h;
	double x;
	double y;
	double z;
} llh_ecef_t;

extern void init_tile_converter(int zoom_levels);

extern point_t wgs84_to_tile(coord_t wgs84, int zoom, map_repo_t *repo);
extern point_t wgs84_to_tilepixel(coord_t wgs84, int zoom, map_repo_t *repo);
extern coord_t tilepixel_to_wgs84(point_t tilepixel, int zoom, map_repo_t *repo);

extern void wgs84_lla_to_ecef(llh_ecef_t *data);
extern void wgs84_ecef_to_lla(llh_ecef_t *data);

#endif /* WGS84_H_ */
