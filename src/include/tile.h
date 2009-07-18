#include "wgs84.h"
#include "sys/time.h"
#include "map_repo.h"

#ifndef TILE_H_
#define TILE_H_

#define TILE_DL_THREADS_LIMIT		3
#define TILE_DL_USER_LIBCURL		0
#define TILE_CACHE_CAPACITY			6

typedef struct __tile_t
{
	int zoom;
	int x;
	int y;
	char *path;
	char *url;
	GdkPixbuf *pixbuf;
	gboolean cached;
} tile_t;

struct __tilecache_slot_t;

typedef struct __tilecache_t
{
	pthread_mutex_t lock;
	int count;
	int capacity;
	struct __tilecache_slot_t *head;
	struct __tilecache_slot_t *tail;
} tilecache_t;

#define MAX_FG_DL				10
#define MAX_UNFINISHED_BATCH_DL	5
#define DL_SLEEP_MS				500

#define BATCH_DL_MAX_FAILS		20

struct __dl_task_t;
struct __batch_dl_bulk_t;

typedef enum
{
	BATCH_DL_STATE_PENDING,
	BATCH_DL_STATE_PROCESSING,
	BATCH_DL_STATE_FINISHING,
	BATCH_DL_STATE_FINISHED,
	BATCH_DL_STATE_CANCELED
} BATCH_DL_STATE;

typedef struct __dl_task_t
{
	int zoom;
	int x;
	int y;
	char *path;
	char *url;
} dl_task_t;

typedef struct __batch_dl_t
{
	BATCH_DL_STATE state;
	map_repo_t *repo;

	coord_t tl_wgs84;
	coord_t br_wgs84;

	int min_zoom;
	int max_zoom;

	/* at most 6 levels, so the max size of this array is at most
	 * several 4096-byte memroy pages */
	dl_task_t *tasks;
	int cur_task_id;

	int num_in_range;
	int num_dl_total;
	int num_dl_done;
	int num_dl_failed;

	struct __batch_dl_t *prev;
	struct __batch_dl_t *next;
} batch_dl_t;

typedef struct __front_task_t
{
	dl_task_t task;
	struct __front_task_t *next;
} front_task_t;

typedef struct __dl_thread_t
{
	/* id in downloader's thread list */
	int id;
	/* the downloader that owns dl thread */
	struct __tile_downloader_t *downloader;
	pthread_t tid;
} dl_thread_t;

/**
 * Per map repository downloader.
 * Each downloader can create and maintain up to <TILE_DL_THREADS_LIMIT>
 * dl threads of type <dl_thread_t>. One or more batches can be queued to a repository's
 * downloader.
 */
typedef struct __tile_downloader_t
{
	map_repo_t *repo;

	pthread_mutex_t lock;
	pthread_cond_t cv;

	gboolean stop;

	/* unfinished or beding processing */
	batch_dl_t *batches;
	batch_dl_t *batches_tail;
	batch_dl_t *cur_batch;

	int batch_count;
	int unfinished_batch_count;

	int dl_threads_count;

	/* from tasks are allocated by caller */
	int front_task_count;
	front_task_t *front_tasks;
	front_task_t *front_tasks_tail;

	/* If a slot is not used, set it as -1 */
	dl_thread_t dl_threads[TILE_DL_THREADS_LIMIT];

} tile_downloader_t;

typedef struct __update_ui_thread_t
{
	pthread_t thread_tid;
	int num_downloading_batches;
	gboolean stop;
} update_ui_thread_t;

/******************* tile_cache.c *********************/

extern tilecache_t * tilecache_new(int capacity);
extern void tilecache_cleanup(tilecache_t *cache, gboolean free_cache);
extern tile_t* tilecache_get(tilecache_t *cache, int zoom, int x, int y);
extern gboolean tilecache_add(tilecache_t *cache, tile_t *tile);

/******************* tile_dl.c ************************/

extern void tile_downloader_module_init();
extern void tile_downloader_module_cleanup();

extern gboolean format_tile_file_path(map_repo_t *repo, int zoom, int x, int y, char *buf, int buflen);
extern void add_front_download_task(map_repo_t *repo, int zoom, int x, int y, char *path, char *url);

extern gboolean batch_download_check();
extern int batch_download_prepare(batch_dl_t *batch);
extern void batch_download(batch_dl_t *batch);

extern update_ui_thread_t * tile_downloader_start_update_ui_thread(map_repo_t *cur_repo);

extern void map_front_download_callback_func(map_repo_t *repo, int zoom, int x, int y);

/********************* ctx_dl_tile.c******************/

extern void update_batch_dl_status();
extern void download_cancel_batch(batch_dl_t *batch);
extern void* batch_download_update_ui_routine(void *arg);
extern void batch_dl_report_error(const char *url, const char *err);

extern GtkWidget* ctx_tab_fix_map_create();
extern void ctx_tab_fix_map_on_show();

/********************* tab_tile.c*********************/
extern void fixmap_update_maplist(map_repo_t *repo);
extern float* get_alpha_values();

extern GtkWidget * tile_tab_create();
extern void tile_tab_on_show();

#endif /* TILE_H_ */
