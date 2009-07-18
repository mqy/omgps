#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "util.h"
#include "omgps.h"

/* in home dir */
#define SETTINGS_FILE			"settings.txt"

#define map_cfg_prefix			"mapcfg."

#define map_cfg_zoom			"zoom"
#define map_cfg_lat_fix			"lat-fix"
#define map_cfg_lon_fix			"lon-fix"

#define key_last_map_name		"last-map-name"

#define key_last_center_lat		"last-center-lat"
#define key_last_center_lon		"last-center-lon"

#define key_last_lat			"last-lat"
#define key_last_lon			"last-lon"
#define key_last_alt			"last-alt"
#define key_last_pacc			"last-pacc"

#define key_agps_user			"agps-user"
#define key_agps_pwd			"agps-pwd"

#define key_sound_cfg_file		"sound-cfg-file"

static cfg_t cfg =
{
	.last_map_name = NULL,

	.last_center_lat = 0,
	.last_center_lon = 0,

	.last_lat = 0,
	.last_lon = 0,
	.last_alt = 0,
	.last_pacc = 0,

	.agps_user = NULL,
	.agps_pwd = NULL,
};

static char *settings_file = NULL;

/**
 * NOTE: most of the checks are just hacks.
 */
static gboolean check_settings()
{
	char *errbuf = thread_context_get_errbuf();

	if (cfg.last_center_lat <= -90 || cfg.last_center_lat >= 90)
		cfg.last_center_lat = 0;

	if (cfg.last_center_lon < -180 || cfg.last_center_lon > 180) {
		cfg.last_center_lon = 0;
	}

	if (cfg.last_lat <= -90 || cfg.last_lat >= 90)
		cfg.last_lat = 0;

	if (cfg.last_lon < -180 || cfg.last_lon > 180)
		cfg.last_lon = 0;

	if (cfg.last_alt < -10000)
		cfg.last_alt = 0;

	/* see ubx spec */
	int max_pacc = 1000000;

	if (cfg.last_pacc >= max_pacc || cfg.last_pacc <= 0)
		cfg.last_pacc = 10000;

	cfg.agps_user = trim(cfg.agps_user);
	cfg.agps_pwd = trim(cfg.agps_pwd);

	if (cfg.agps_user && strcmp(cfg.agps_user, "") != 0) {
		if (! validate_email(cfg.agps_user)) {
			snprintf(errbuf, ERRBUF_LEN, "%s=%s: not a valid email address",
				key_agps_user, cfg.agps_user);
			return FALSE;
		}
		if (! cfg.agps_pwd || strcmp(cfg.agps_pwd, "") == 0) {
			snprintf(errbuf, ERRBUF_LEN, "user is set but password is empty");
			return FALSE;
		}
	}

	return TRUE;
}

static void parse_map_config(char *key, char *value)
{
	char *map_name = key + strlen(map_cfg_prefix);
	if (*map_name == '\0')
		return;

	map_repo_t *repo = mapcfg_get_repo(map_name);
	if (! repo)
		return;

	value = strdup(value);
	char *sep = ";";
	char *saveptr;
	char *p = strtok_r(value, sep, &saveptr);
	char *val;

	while (p) {
		if ((val = strstr(p, "="))) {
			*val = '\0';
			++val;
			if (! *val)
				continue;

			key = trim(p);
			val = trim(val);

			if (val) {
				if (strcmp(key, map_cfg_lat_fix) == 0) {
					repo->lat_fix = atof(val);
					if (fabs(repo->lat_fix) > MAX_LAT_LON_FIX)
						repo->lat_fix = 0;
				} else if (strcmp(key, map_cfg_lon_fix) == 0) {
					repo->lon_fix = atof(val);
					if (fabs(repo->lon_fix) > MAX_LAT_LON_FIX)
						repo->lon_fix = 0;
				} else if (strcmp(key, map_cfg_zoom) == 0) {
					repo->zoom = atoi(val);
					if (repo->zoom < repo->min_zoom || repo->zoom > repo->max_zoom)
						repo->zoom = repo->min_zoom;
				}
			}
		}
		p = strtok_r(NULL, sep, &saveptr);
	}

	free(value);
}

static gboolean parse_line(char *line, int len)
{
	char *key = NULL, *value = NULL;

	key = line;
	value = strstr(line, "=");
	if (value == NULL)
		return FALSE;

	*value = '\0';
	++value;

	key = trim(key);
	value = trim(value);

	#define IS_KEY(s) (strcmp(key, s) == 0)

	/* NOTE: atof: not exactly accurate, but acceptable */

	if (IS_KEY(key_last_map_name))
		cfg.last_map_name = value? strdup(value) : NULL;
	else if (IS_KEY(key_sound_cfg_file)) {
		if (value)
			value = trim(value);
		if (value && strstr(value, ".py"))
			cfg.last_sound_file = strdup(value);
		else
			cfg.last_sound_file = NULL;
	}
	else if (IS_KEY(key_last_center_lat))
		cfg.last_center_lat = atof(value);
	else if (IS_KEY(key_last_center_lon))
		cfg.last_center_lon = atof(value);
	else if (IS_KEY(key_last_lat))
		cfg.last_lat = atof(value);
	else if (IS_KEY(key_last_lon))
		cfg.last_lon = atof(value);
	else if (IS_KEY(key_last_alt))
		cfg.last_alt = atof(value);
	else if (IS_KEY(key_last_pacc))
		cfg.last_pacc = atof(value);
	else if (IS_KEY(key_agps_user))
		cfg.agps_user = value? strdup(value) : NULL;
	else if (IS_KEY(key_agps_pwd))
		cfg.agps_pwd = value? strdup(value) : NULL;
	else if (strncmp(key, map_cfg_prefix, strlen(map_cfg_prefix)) == 0) {
		if (value) {
			parse_map_config(key, value);
		}
	}

	return TRUE;
}

static int read_line(int fd, char line_buf[], int buflen, char *errbuf, int errbuf_len)
{
	int n, i = 0;
	int max_idx = buflen - 1;
	char c;

	while(1) {
		n = read(fd, &c, 1);
		if (n < 0) {
			snprintf(errbuf, errbuf_len, "read settings: I/O error: %s", settings_file);
			return -1;
		} else if (n == 0 || c == '\n') {
			/* EOF or line end */
			break;
		} else {
			if (i == max_idx) {
				snprintf(errbuf, errbuf_len,
					"read settings: max line buffer reached (%d): %s", buflen, settings_file);
				return -1;
			}
			line_buf[i++] = c;
		}
	}
	return i;
}

static void set_map_default_config(map_repo_t *repo, void *arg)
{
	repo->zoom = repo->min_zoom;
	repo->lat_fix = 0;
	repo->lon_fix = 0;
}

/**
 * Load settings from <file> to global vars: maps etc..
 * Call this after map configuration being loaded.
 */
cfg_t *settings_load()
{
	mapcfg_iterate_maplist(set_map_default_config, NULL);

	if (settings_file == NULL) {
		char buf[256];
		snprintf(buf, sizeof(buf), "%s/%s", g_context.config_dir, SETTINGS_FILE);
		settings_file = strdup(buf);
	}

	int fd = 0;
	struct stat st;

	char *errbuf = thread_context_get_errbuf();

	if (stat(settings_file, &st) == 0) {
		if ((fd = open(settings_file, O_RDWR, 0644)) <= 0) {
			log_error("open settings file failed, error=%s", strerror(errno));
			snprintf(errbuf, ERRBUF_LEN, "open settings file failed: %s", settings_file);
		}
	} else {
		return &cfg;
	}

	/* begin parsing */
	char line_buf[256];
	int n = 0;
	gboolean ret = FALSE;

	while(1) {
		n = read_line(fd, line_buf, sizeof(line_buf), errbuf, ERRBUF_LEN);
		if (n <= 0)
			break;
		/* ignore comment/blank line */
		if (*line_buf == '#')
			continue;
		line_buf[n] = '\0';
		if (! parse_line(line_buf, n)) {
			snprintf(errbuf, ERRBUF_LEN, "settings file: invalid line: %s", line_buf);
			continue;
		}
	}

	ret = check_settings(errbuf, ERRBUF_LEN);

	if (fd > 0)
		close(fd);

	return &cfg;
}

static void save_map_config(map_repo_t *repo, void *arg)
{
	FILE *fp = (FILE *)arg;
	fprintf(fp, "%s%s = %s=%d; %s=%f; %s=%f\n", map_cfg_prefix, repo->name,
			map_cfg_zoom, repo->zoom, map_cfg_lat_fix, repo->lat_fix, map_cfg_lon_fix, repo->lon_fix);
}

static void save_cfg(FILE *fp)
{
	fprintf(fp, key_last_map_name" = %s\n", g_view.fglayer.repo->name);
	fprintf(fp, key_last_center_lat" = %f\n", cfg.last_center_lat);
	fprintf(fp, key_last_center_lon" = %f\n", cfg.last_center_lon);
	fprintf(fp, key_last_lat" = %f\n", cfg.last_lat);
	fprintf(fp, key_last_lon" = %f\n", cfg.last_lon);
	fprintf(fp, key_last_alt" = %f\n", cfg.last_alt);
	fprintf(fp, key_last_pacc" = %f\n", cfg.last_pacc);
	fprintf(fp, key_agps_user" = %s\n", cfg.agps_user == NULL? "" : cfg.agps_user);
	fprintf(fp, key_agps_pwd" = %s\n",	cfg.agps_pwd == NULL? "" : cfg.agps_pwd);
	fprintf(fp, key_sound_cfg_file" = %s\n", cfg.last_sound_file? cfg.last_sound_file : "");

	mapcfg_iterate_maplist(save_map_config, fp);
}

static void backup_file(char *file)
{
	int len = strlen(file);
	char *ext = ".bak";
	char *new = (char *)malloc(len + strlen(ext) + 1);
	FILE *fp1 = NULL, *fp2 = NULL;

	if (new == NULL)
		return; /* silently ignore */

	sprintf(new, "%s%s", file, ext);

	fp1 = fopen(file, "r");
	if (fp1 == NULL)
		goto END;

	fp2 = fopen(new, "w+");
	if (fp2 == NULL)
		goto END;

	char buf[1024];
	int n;
	while ((n=fread(buf, 1, sizeof(buf), fp1)) > 0)
		if (fwrite(buf, 1, n, fp2) < 0)
			goto END;

END:
	if (new)
		free(new);
	if (fp1)
		fclose(fp1);
	if (fp2)
		fclose(fp2);
}

/**
 * Copy default settings to target configuration file.
 * return 0 if ok, else failed.
 * NOTE: Ignore error checking for fprintf.
 */
gboolean settings_save()
{
	struct stat st;
	if (stat(settings_file, &st) == 0)
		backup_file(settings_file);

	FILE *fp = fopen(settings_file, "w+");
	if (fp == NULL)
		return FALSE;

	fprintf(fp, "%s", "#This is the runtime configuration file of OMGPS. Don't modify it!\n");

	save_cfg(fp);

	fclose(fp);
	return TRUE;
}
