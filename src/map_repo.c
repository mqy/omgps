#include "map_repo.h"
#include "util.h"
#include "omgps.h"
#include "py_ext.h"

/**
 * @Ref: http://www.mgmaps.com/cache/MapTileCacher.perl
 * http://wiki.openaerialmap.org/Using_With_OSM
 */

#define MAP_CFG_MODULE			"map"
#define MAP_CFG_FUNC_MAP_LIST	"map_list"
#define MAP_CFG_FUNC_URL		"_url"

static PyObject *pModule = NULL;

typedef struct __list_entry_t
{
	map_repo_t *repo;
	struct __list_entry_t *next;
} list_entry_t;

static list_entry_t *map_list_head = NULL;
static list_entry_t *map_list_tail = NULL;

static map_repo_t* parse_map_config_details(char *map_name, char *map_cfg_str,
	char *errbuf, int errbuf_len)
{
	map_repo_t * repo = (map_repo_t *)calloc(1, sizeof(map_repo_t));
	if (! repo) {
		snprintf(errbuf, errbuf_len, "load map config: %s\n\nmalloc failed", map_name);
		return NULL;
	}
	repo->name = strdup(map_name);
	repo->min_zoom = 0;
	repo->max_zoom = 0;
	repo->image_type = NULL;

	char *bak = strdup(map_cfg_str);
	char *sep = ";";
	char *saveptr;
	char *p = strtok_r(bak, sep, &saveptr);
	char *key, *value;

	/* parse string */
	while (p) {
		if ((value = strstr(p, "="))) {
			*value = '\0';
			++value;
			key = trim(p);
			//log_debug("key=%s, value=%s", key, value);

			if (strcmp(key, "min-zoom") == 0) {
				repo->min_zoom = *value? atoi(value) : -1;
			} else if (strcmp(key, "max-zoom") == 0) {
				repo->max_zoom = *value? atoi(value) : -1;
			} else if (strcmp(key, "image-type") == 0) {
				repo->image_type = *value? strdup(trim(value)) : NULL;
			}
		}
		p = strtok_r(NULL, sep, &saveptr);
	}

	free(bak);

	gboolean ok = TRUE;

	/* sanity check */
	if (repo->min_zoom < 0 || repo->max_zoom <= 0 ||
			repo->min_zoom > repo->max_zoom || repo->max_zoom > MAX_ZOOM_LEVELS) {
		snprintf(errbuf, errbuf_len, "load map config: %s\n\ninvalid zoom value(s)", map_name);
		ok = FALSE;
		goto END;
	}

	if (! repo->image_type || strcmp(repo->image_type, "") == 0) {
		snprintf(errbuf, errbuf_len, "load map config: %s\n\nimage type is not set", map_name);
		ok = FALSE;
		goto END;
	}

END:

	if (! ok) {
		if (repo) {
			free(repo->name);
			free(repo);
			repo = NULL;
		}
	}

	return repo;
}

static map_repo_t * parse_map_from_python(char *map_name)
{
	PyObject *pFunc = NULL;
	PyObject *pValue = NULL;

	/* call map config */
	char *func_name = map_name;
	map_repo_t *repo = NULL;

	char *errbuf = thread_context_get_errbuf();

	pFunc = PyObject_GetAttrString(pModule, func_name);
	if (! pFunc || ! PyCallable_Check(pFunc)) {
		snprintf(errbuf, ERRBUF_LEN, "Map config: can't find function: \"%s\"\n", func_name);
		goto END;
	}

	pValue = PyObject_CallObject(pFunc, NULL);
	if (! pValue) {
		snprintf(errbuf, ERRBUF_LEN, "Map config: can't call function: %s\n", func_name);
		goto END;
	}

	char *map_cfg_str = PyString_AsString(pValue);
	if (map_cfg_str == NULL || strcmp(map_cfg_str, "") == 0) {
		snprintf(errbuf, ERRBUF_LEN, "Map config: bad config from function: %s", func_name);
		goto END;
	}

	repo = parse_map_config_details(map_name, map_cfg_str, errbuf, ERRBUF_LEN);
	if (! repo)
		goto END;

	char buf[256];
	sprintf(buf, "%s%s", func_name, MAP_CFG_FUNC_URL);

	repo->urlfunc = PyObject_GetAttrString(pModule, buf);
	if (! repo->urlfunc || ! PyCallable_Check(repo->urlfunc)) {
		snprintf(errbuf, ERRBUF_LEN, "Map config: can't find function: %s\n", buf);
		free(repo);
		repo = NULL;
		goto END;
	}

END:

	if (pValue) {
		Py_DECREF(pValue);
	}
	return repo;
}

/**
 * for each map name,
 *	(1) call it's config function
 *	(2) get url function object
 */
static gboolean parse_map_config(char *map_name)
{
	map_repo_t *repo = parse_map_from_python(map_name);
	if (! repo) {
		return FALSE;
	}

	list_entry_t *e = (list_entry_t*)malloc(sizeof(list_entry_t));
	if (! e) {
		char *errbuf = thread_context_get_errbuf();
		snprintf(errbuf, ERRBUF_LEN, "Map config: malloc failed");
		return FALSE;
	}
	repo->name = strdup(map_name);
	e->repo = repo;
	e->next = NULL;

	if (! map_list_head) {
		map_list_head = map_list_tail = e;
	} else {
		map_list_tail->next = e;
		map_list_tail = e;
	}

	return TRUE;
}

/**
 * Map name list: name_1; name_2; ... name_n
 */
static gboolean parse_python_config(char* map_list)
{
	char *bak = strdup(map_list);
	char *sep = ";";
	char *saveptr;
	char *p = strtok_r(map_list, sep, &saveptr);
	while (p) {
		p = trim(p);
		if (p) {
			if(! parse_map_config(p))
				return FALSE;
		}
		p = strtok_r(NULL, sep, &saveptr);
	}
	free(bak);
	return TRUE;
}

gboolean mapcfg_load()
{
	PyObject *pFunc = NULL;
	PyObject *pValue = NULL;
	gboolean ret = FALSE;

	char *errbuf = thread_context_get_errbuf();

	py_ext_lock();

	PyObject *pName = PyString_FromString(MAP_CFG_MODULE);
	pModule = PyImport_Import(pName);
	Py_DECREF(pName);

	if (pModule == NULL) {
		PyErr_Print();
		snprintf(errbuf, ERRBUF_LEN, "Map config: failed to load %s.py",
			MAP_CFG_MODULE);
		goto END;
	}

	char *func_name = MAP_CFG_FUNC_MAP_LIST;

	pFunc = PyObject_GetAttrString(pModule, func_name);
	if (! pFunc || ! PyCallable_Check(pFunc)) {
		log_error("Map config: Can't find function: \"%s\"", func_name);
		goto END;
	}

	pValue = PyObject_CallObject(pFunc, NULL);
	if (! pValue) {
		snprintf(errbuf, ERRBUF_LEN, "Map config: can't call function: \"%s\"", func_name);
		goto END;
	}

	char *map_list = PyString_AsString(pValue);

	if (map_list == NULL || strcmp(map_list, "") == 0) {
		snprintf(errbuf, ERRBUF_LEN, "Map config: bad map list from function: \"%s\"", func_name);
		goto END;
	}

	ret = parse_python_config(map_list);

END:

	if (pFunc) {
		Py_DECREF(pFunc);
	}
	if (pValue) {
		Py_DECREF(pValue);
	}

	py_ext_unlock();

	list_entry_t *e;
	char buf[256];
	for (e = map_list_head; e; e=e->next) {
		snprintf(buf, sizeof(buf), "%s/%s", g_context.maps_dir, e->repo->name);
		e->repo->dir = strdup(buf);
		/* Other fields are zeroed out by calloc() */
	}

	return (ret && map_list_head != NULL);
}

/**
 * Don't call this if the map is not properly configured.
 * Caller must free url!
 */
char *mapcfg_get_dl_url(map_repo_t *repo, int zoom, int x, int y)
{
	PyObject *urlfunc = repo->urlfunc;
	if (! urlfunc)
		return NULL;

	char *url = NULL;

	py_ext_lock();

	PyObject *pArgs = PyTuple_New(3);

	PyObject *pValue;
	pValue = PyInt_FromLong(zoom);
	PyTuple_SetItem(pArgs, 0, pValue);

	pValue = PyInt_FromLong(x);
	PyTuple_SetItem(pArgs, 1, pValue);

	pValue = PyInt_FromLong(y);
	PyTuple_SetItem(pArgs, 2, pValue);

	pValue = PyObject_CallObject(urlfunc, pArgs);
	if (pValue == NULL) {
		char *errbuf = thread_context_get_errbuf();
		snprintf(errbuf, ERRBUF_LEN, "Get download url failed: map name=%s", repo->name);
		goto END;
	}

	url = PyString_AsString(pValue);
	if (url)
		url = strdup(url);

END:

	if(pArgs) {
		Py_DECREF(pArgs);
	}
	if(pValue) {
		Py_DECREF(pValue);
	}

	py_ext_unlock();

	return url;
}

void mapcfg_cleanup()
{
	/* free urlfunc for each map */
	list_entry_t *e, *next;
	e = map_list_head;

	while (e) {
		if (e->repo) {
			if (e->repo->urlfunc) {
				Py_DECREF(e->repo->urlfunc);
			}
			if (e->repo->name)
				free(e->repo->name);
			free(e->repo);
		}
		next = e->next;
		free(e);
		e = next;
	}

	map_list_head = map_list_tail = NULL;

	if (pModule) {
		Py_DECREF(pModule);
	}
}

map_repo_t *mapcfg_get_repo(char *map_name)
{
	list_entry_t *e;
	for (e = map_list_head; e; e=e->next) {
		if (strcmp(map_name, e->repo->name) == 0)
			return e->repo;
	}
	return NULL;
}

map_repo_t *mapcfg_get_default_repo(char *map_name)
{
	if (! map_name)
		return map_list_head->repo;

	map_repo_t* repo = mapcfg_get_repo(map_name);
	if (repo)
		return repo;
	else
		return map_list_head->repo;
}

void mapcfg_iterate_maplist(iterate_maplist_func f, void *arg)
{
	list_entry_t *e;
	for (e = map_list_head; e; e=e->next) {
		(*f)(e->repo, arg);
	}
}
