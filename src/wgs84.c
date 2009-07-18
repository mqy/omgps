#include <math.h>
#include <stdlib.h>
#include "wgs84.h"

#define DEG_TO_RAD	(M_PI / 180)
#define RAD_TO_DEG	(180 / M_PI)

/**
 * see:
 *
 * http://en.wikipedia.org/wiki/Mercator_projection
 * http://wiki.openstreetmap.org/wiki/Slippy_map_tilenames
 */

/* cache */
static double *cc;

void init_tile_converter(int zoom_levels)
{
	if (cc)
		free(cc);
	cc = (double *)malloc(sizeof(double) * 3 * zoom_levels);

	int i, off, size = TILE_SIZE;

	for (i=0; i<zoom_levels; i++) {
		off = i * 3;
		cc[off] = size / 360.0;
		cc[off+1] = size / (2 * M_PI);
		cc[off+2] = size / 2;
		size *= 2;
	}
}

/************ Spherical projection ********************************************/

inline point_t wgs84_to_tilepixel(coord_t wgs84, int zoom, map_repo_t *repo)
{
	wgs84.lat -= repo->lat_fix;
	wgs84.lon -= repo->lon_fix;

	int off = zoom * 3;

	double d = cc[off+2];
	double sin_lat = sin(DEG_TO_RAD * wgs84.lat);
	if (sin_lat < -0.999999) sin_lat = -0.999999;
	if (sin_lat > 0.999999)  sin_lat = 0.999999;
	point_t point = {
		(int)(d + wgs84.lon * cc[off]),
		(int)(d - 0.5 * log ((1 + sin_lat) / (1 - sin_lat)) * cc[off+1])
	};
	return point;
}

inline coord_t tilepixel_to_wgs84(point_t pixel, int zoom, map_repo_t *repo)
{
	int off = zoom * 3;
	double g = (cc[off+2] - pixel.y) / cc[off+1];
	coord_t wgs84 = {
		RAD_TO_DEG * ( 2 * atan(exp(g)) - M_PI_2),
		(pixel.x - cc[off+2]) / cc[off]
	};
	wgs84.lat += repo->lat_fix;
	wgs84.lon += repo->lon_fix;
	return wgs84;
}

inline point_t wgs84_to_tile(coord_t wgs84, int zoom, map_repo_t *repo)
{
	point_t point = wgs84_to_tilepixel(wgs84, zoom, repo);
	point.x = (int)floor(1.0 * point.x / TILE_SIZE);
	point.y = (int)floor(1.0 * point.y / TILE_SIZE);
	return point;
}

/**
 * LLA to ECEF XYZ
 * @ref: GPS_Compendium(GPS-X-02007).pdf,  5.3.6.2
 */
void wgs84_lla_to_ecef(llh_ecef_t *data)
{
	if (data->lon < 0)
		data->lon += 360;

	data->lat *= DEG_TO_RAD;
	data->lon *= DEG_TO_RAD;

	double a = WGS84_SEMI_MAJOR_AXIS;
	double b = a * (1.0 - WGS84_FLATENING_FACTOR);
	double a2 = a * a;
	double b2 = b * b;
	double e1 = (a2 - b2) / a2;

	double e2 = sin(data->lat);
	double e3 = a / sqrt (1.0 - e1 * e2 * e2);
	double e4 = (e3 + data->h) * cos(data->lat);

	data->x = e4 * cos(data->lon);
	data->y = e4 * sin(data->lon);
	data->z = (e3 * (1-e1) + data->h) * e2;
}

/**
 * CEF XYZ to LLA
 * @ref: GPS_Compendium(GPS-X-02007).pdf,  5.3.6.1
 */
void wgs84_ecef_to_lla(llh_ecef_t *data)
{
	double a = WGS84_SEMI_MAJOR_AXIS;
	double b = a * (1.0 - WGS84_FLATENING_FACTOR);
	double a2 = a * a;
	double b2 = b * b;

	double e0 = (a2 - b2) / a2;
	double e1 = (a2 - b2) / b2;
	double e2 = sqrt(data->x * data->x + data->y * data->y);
	double e3 = atan((data->z * a) / (e2 * b));
	double e4 = sin(e3);
	double e5 = cos(e3);
	double e6 = e4 * e4 * e4;
	double e7 = e5 * e5 * e5;

	double lat0 = atan((data->z + (e1 * b * e6)) / (e2 - e0 * a * e7));
	double lon0 = atan(data->y / data->x) * RAD_TO_DEG;
	double e8 = sin(lat0);

	if (data->x < 0)
		lon0 += 180;
	if (lon0 > 180)
		lon0 -= 360;

	data->lat = lat0 * RAD_TO_DEG;
	data->lon = lon0;
	data->h = (e2 / cos(lat0) - a / sqrt (1.0 - e0 * e8 * e8));
}
