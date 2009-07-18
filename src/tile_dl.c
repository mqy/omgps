#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

#include "omgps.h"
#include "tile.h"
#include "network.h"
#include "util.h"

#define MAX_IDLE_MS		10000

static pthread_attr_t pthread_attr;
static update_ui_thread_t update_ui_thread;

static void * download_routine(void *args);
static gboolean tile_downloader_create_thread(tile_downloader_t *td);
static batch_dl_t *pending_free_list = NULL;
static batch_dl_t *pending_free_list_tail = NULL;

/**
 * Format tile fullpath to <buf>
 */
gboolean format_tile_file_path(map_repo_t *repo, int zoom, int x, int y, char *buf, int buflen)
{
	if (snprintf(buf, buflen, "%s/%d/%d/%d.%s",	repo->dir, zoom, x, y, repo->image_type) <= 0) {
		log_error("snprintf tile file path failed");
		return FALSE;
	}
	return TRUE;
}

/**
 * we limit max un-finished batch
 */
gboolean batch_download_check(map_repo_t *repo)
{
	gboolean ret = TRUE;
	tile_downloader_t *td = (tile_downloader_t *)repo->downloader;

	LOCK_MUTEX(&(td->lock));
	if (td->unfinished_batch_count == MAX_UNFINISHED_BATCH_DL)
		ret = FALSE;
	UNLOCK_MUTEX(&(td->lock));
	return ret;
}

static inline gboolean test_image_header(char *dest, char *src, int n)
{
	int i;
	for (i=0; i<n; i++) {
		if (src[i] != dest[i])
			return FALSE;
	}
	return TRUE;
}

/**
 * Donwload tile
 */
static int download_tile(map_repo_t *repo, pthread_mutex_t *lock, dl_task_t *task)
{
	char *path = strdup(task->path);
	char *url = strdup(task->url);
	int ret = -1;
	gboolean unlocked = FALSE;
	struct stat st;
	char buf[256];
	char *err = NULL;

	snprintf(buf, sizeof(buf), "%s/%d/%d/",	repo->dir, task->zoom, task->x);

	/* Mkdir if not exists */
	if (stat(buf, &st) != 0 && g_mkdir_with_parents(buf, 0700) != 0) {
		err = "failed to parent mkdir";
		goto END;
	}

	/* already exists, don't see this as error */
	if (stat(path, &st) == 0) {
		ret = 0;
		goto END;
	}

	/* NOTE: use temp file and rename, instead of override */
	snprintf(buf, sizeof(buf), "%s.tmp", path);

	int fd = open(buf, O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		err = "unable to open file";
		goto END;
	}

	/* being processed by another thread, don't see this as error */
	if (flock(fd, LOCK_EX | LOCK_NB) < 0 &&	(errno == EWOULDBLOCK)) {
		ret = 0;
		goto END;
	}

	/* unlock */
	unlocked = TRUE;
	UNLOCK_MUTEX(lock);

	http_get_result_t result;
	http_get(url, fd, 15, 15, &result);

	flock(fd, LOCK_UN);
	close(fd);

	ret = 0;

	/* post processing */

	if (result.error_no != HTTP_GET_ERROR_NONE) {
		err = result.err_buf;
		/* cancel the temp fie explicitly. If failed, OS will reclaim it */
		unlink(buf);
		ret = -2;
	} else {
		gboolean bad = FALSE;

		/* FIXME: (1) jpg/jpeg, (2) use enum instead of compare each one */
		if (strcmp(repo->image_type, "png") == 0) {
			if (strcmp(result.content_type, "image/png") != 0)
				bad = TRUE;
		} else if (strcmp(repo->image_type, "jpg") == 0) {
			if (strcmp(result.content_type, "image/jpg") != 0 &&
				strcmp(result.content_type, "image/jpeg") != 0)
				bad = TRUE;
        }

    	if (bad) {
    		unlink(buf);
    		err = "bad image or image type is not expected";
    		ret = -3;
    	} else {
			if (rename(buf, path) < 0) {
				log_debug("%s: remame failed: %s", path, strerror(errno));
				ret = -4;
			}
    	}
	}

END:

	if (! unlocked)
		UNLOCK_MUTEX(lock);

	if (ret != 0 && err) {
		LOCK_UI();
		batch_dl_report_error(strdup(url), strdup(err));
		UNLOCK_UI();
	}

	free(url);
	free(path);

	sleep_ms(DL_SLEEP_MS);

	return ret;
}

/**
 * The behavior is similiar to pthread_cond_timedwait();
 */
static void download_next_front(tile_downloader_t *td)
{
	front_task_t *ft, *next;
	ft = td->front_tasks;
	next = ft->next;
	/* copy: ft will be freed  */
	dl_task_t task = ft->task;
	free(ft);

	ft = td->front_tasks = next;
	if (! ft) {
		td->front_tasks_tail= NULL;
		td->front_task_count = 0;
	}

	/* will release lock before perform download */
	download_tile(td->repo, &(td->lock), &task);

	map_front_download_callback_func(td->repo, task.zoom, task.x, task.y);

	/* lock again */
	LOCK_MUTEX(&(td->lock));

	free(task.path);
	free(task.url);
	task.path = NULL;
	task.url = NULL;
}

static inline void set_next_cur_batch(tile_downloader_t *td)
{
	if (! td->cur_batch)
		return;

	batch_dl_t *next = td->cur_batch->next;
	if (next) {
		td->cur_batch = next;
		td->cur_batch->cur_task_id = 0;
		td->cur_batch->state = BATCH_DL_STATE_PROCESSING;
	} else {
		td->cur_batch = NULL;
	}
}

void download_cancel_batch(batch_dl_t *batch)
{
	tile_downloader_t *td = (tile_downloader_t *)batch->repo->downloader;

	LOCK_MUTEX(&(td->lock));

	batch_dl_t *prev = batch->prev;
	batch_dl_t *next = batch->next;
	if (prev)
		prev->next = next;
	if (next)
		next->prev = prev;
	if (td->batches == batch)
		td->batches = next;
	if (td->batches_tail == batch)
		td->batches_tail = prev;
	batch->prev = batch->next = NULL;

	if (batch->tasks) {
		int i;
		for (i = 0; i<batch->num_dl_total; i++) {
			if (batch->tasks[i].path) {
				free(batch->tasks[i].path);
				batch->tasks[i].path = NULL;
			}
			if (batch->tasks[i].url) {
				free(batch->tasks[i].url);
				batch->tasks[i].url = NULL;
			}
		}
		free(batch->tasks);
		batch->tasks = NULL;
	}

	/* download threads may hold reference to this batch, can't free */

	if (batch == td->cur_batch) {
		if (next) {
			td->cur_batch = next;
			td->cur_batch->cur_task_id = 0;
			td->cur_batch->state = BATCH_DL_STATE_PROCESSING;
		} else {
			td->cur_batch = NULL;
		}
	} else {
		if (pending_free_list_tail)
			pending_free_list_tail = pending_free_list_tail->next = batch;
		else
			pending_free_list = pending_free_list_tail = batch;
	}

	if (batch->state != BATCH_DL_STATE_FINISHED) {
		--(td->unfinished_batch_count);
		--update_ui_thread.num_downloading_batches;
	}

	--(td->batch_count);

	batch->state = BATCH_DL_STATE_CANCELED;

	UNLOCK_MUTEX(&(td->lock));
}

/**
 *
 * The behavior is similiar to pthread_cond_timedwait();
 * NOTE: when this function is called by a thread, td->lock is locked
 */
static void download_next_batch(tile_downloader_t *td)
{
	/* keep the reference since we will update it later */
	batch_dl_t *batch = td->cur_batch;

	if (! batch) {
		UNLOCK_MUTEX(&(td->lock));
		sleep(1);
		LOCK_MUTEX(&(td->lock));
		return;
	}

	dl_task_t *task = &(batch->tasks[(batch->cur_task_id)++]);

	//log_debug("task: zoom=%d, x=%d, y=%d", task->zoom, task->x, task->y);

	/* no tasks to be processed in current batch */
	if (batch->cur_task_id == batch->num_dl_total) {
		batch->state = BATCH_DL_STATE_FINISHING;
		batch->cur_task_id = -1; /* invalid */
		set_next_cur_batch(td);
	}

	/* will release lock before perform download */
	int ret = download_tile(td->repo, &(td->lock), task);

	LOCK_MUTEX(&(td->lock));

	if (batch->state == BATCH_DL_STATE_CANCELED)
		return;

	/* If the task was canceled, the path and url are set as NULL */
	assert(task->path);
	assert(task->url);
	if (task->path)
		free(task->path);
	if (task->url)
		free(task->url);
	task->path = NULL;
	task->url = NULL;

	/* update the batch that contains previous task */
	if (ret < 0)
		++(batch->num_dl_failed);

	if (++(batch->num_dl_done) == batch->num_dl_total) {
		batch->state = BATCH_DL_STATE_FINISHED;
		--(td->unfinished_batch_count);
		free(batch->tasks);
		batch->tasks = NULL;

		UNLOCK_MUTEX(&(td->lock));
		LOCK_UI();
		--update_ui_thread.num_downloading_batches;
		UNLOCK_UI();
		LOCK_MUTEX(&(td->lock));
	}
}

static void free_pending_frees()
{
	/* cleanup pending free for canceled batch downloads */
	batch_dl_t *b = pending_free_list;
	batch_dl_t *next;
	while(b) {
		next = b->next;
		free(b);
		b = next;
	}
	pending_free_list = pending_free_list_tail = NULL;
}

/**
 * Download thread routine.
 *
 * Some map provider suspects we're malicious crawler.
 * FIXME: adjust download frequency or stop downloading if we can detect that we are refused?
 */
static void * download_routine(void *arg)
{
	dl_thread_t *thread = (dl_thread_t *)arg;
	tile_downloader_t *td = thread->downloader;

	sigset_t sig_set;
	sigemptyset(&sig_set);
	sigaddset(&sig_set, SIGINT);
	pthread_sigmask(SIG_BLOCK, &sig_set, NULL);

	pthread_context_t *ctx = register_thread("download tile thread (batch)", NULL, NULL);

	pthread_mutex_t *lock = &(td->lock);
	pthread_cond_t *cv = &(td->cv);

	/* Other fields of <thread> are zeroed out by calloc() */

	thread->tid = pthread_self();

	while (! td->stop) {
		LOCK_MUTEX(lock);

HARD_WORKER:

		/* during download, the lock is unlocked then locked, so
		 * If the downloader needs to lock this thread's lock when on new download task,
		 * it can grasp the lock */
		while ((td->front_task_count > 0) || (td->unfinished_batch_count > 0)) {
			if (td->front_task_count > 0)
				download_next_front(td);
			else
				download_next_batch(td);
			if (td->stop)
				goto END;
		}

		/* wait also release lock */
		int ret = wait_ms(MAX_IDLE_MS, cv, lock, FALSE);
		/* and the lock is grasped again*/
		if (td->stop)
			goto END;

		if (ETIMEDOUT == ret) {
			if ((td->front_task_count > 0) || (td->unfinished_batch_count > 0))
				goto HARD_WORKER;
			else {
				--(thread->downloader->dl_threads_count);
				thread->downloader->dl_threads[thread->id].tid = 0;
				UNLOCK_MUTEX(lock);
				goto END;
			}
		} else {
			goto HARD_WORKER;
		}
	}

END:

	free_pending_frees();
	free(ctx);

	return NULL;
}

/**
 * front-end download request, when update view.
 */
void add_front_download_task(map_repo_t *repo, int zoom, int x, int y, char *path, char *url)
{
	tile_downloader_t *td = (tile_downloader_t *)repo->downloader;

	LOCK_MUTEX(&(td->lock));

	/* check duplicate */
	front_task_t *ft = td->front_tasks;
	while (ft) {
		if (ft->task.zoom == zoom && ft->task.x == x && ft->task.y == y)
			goto END;
		ft = ft->next;
	}

	ft = (front_task_t*) malloc(sizeof(front_task_t));
	ft->task.zoom = zoom;
	ft->task.x = x;
	ft->task.y = y;
	ft->task.path = path;
	ft->task.url = url;
	ft->next = NULL;

	if (td->front_task_count == 0) {
		td->front_tasks = td->front_tasks_tail = ft;
	} else {
		td->front_tasks_tail->next = ft;
		td->front_tasks_tail = ft;
	}
	++(td->front_task_count);

	if ((td->front_task_count > (td->dl_threads_count << 2)) &&
		(td->dl_threads_count < TILE_DL_THREADS_LIMIT)) {
		tile_downloader_create_thread(td);
	}
	pthread_cond_broadcast(&(td->cv));

END:

	UNLOCK_MUTEX(&(td->lock));
}

int batch_download_prepare(batch_dl_t *batch)
{
	int levels = batch->max_zoom - batch->min_zoom + 1;
	int cur_zoom = batch->min_zoom;
	map_repo_t *repo = batch->repo;

	struct stat st;
	char buf[256], *url;
	point_t tl_tile, br_tile;
	int i, j, k, x, y, idx, max_tile_no, size;

	batch->num_in_range = 0;
	batch->num_dl_total = 0;
	batch->num_dl_done = 0;
	batch->num_dl_failed = 0;

	/* sizeof(dl_task_t) == 20, 4096 / 20 == 204 */
	const int step = 200;
	int up_bound = step;

	dl_task_t *bulk = (dl_task_t*)calloc(step, sizeof(dl_task_t));
	if (! bulk)
		return -1;

	idx = 0;
	size = 0;

	for (i=0; i<levels; i++) {
		++cur_zoom;
		max_tile_no = (1 << cur_zoom) - 1;
		tl_tile = wgs84_to_tile(batch->tl_wgs84, cur_zoom, repo);
		br_tile = wgs84_to_tile(batch->br_wgs84, cur_zoom, repo);

		tl_tile.x = MIN(MAX(tl_tile.x, 0), max_tile_no);
		tl_tile.y = MIN(MAX(tl_tile.y, 0), max_tile_no);

		int rows = br_tile.y - tl_tile.y + 1;
		int cols = br_tile.x - tl_tile.x + 1;

		if (rows <= 0 || cols <= 0)
			continue;

		for (j=0; j<cols; j++) {
			for (k=0; k<rows; k++) {
				x = tl_tile.x + j;
				y = tl_tile.y + k;
				++batch->num_in_range;

				if (! format_tile_file_path(repo, cur_zoom, x, y, buf, sizeof(buf)))
					continue;

				if (stat(buf, &st) == 0) {
					size += st.st_size;
				} else {
					/* SPECIAL NOTE: also synchronize access to Python interpreter! */
					url = mapcfg_get_dl_url(repo, cur_zoom, x, y);
					if (! url) {
						log_error("download tile: can't get url for map: %s", repo->name);
						continue;
					}

					if (idx == up_bound) {
						up_bound += step;
						/* 1. needless to clear newly allocated region
						 * 2. at most 6 levels (< 10 KB), highly chances that we can get
						 * this amount fo continuous memory */
						bulk = (dl_task_t*)realloc(bulk, sizeof(dl_task_t) * up_bound);
						if (! bulk) {
							log_error("realloc for batch download failed.");
							return -1;
						}
					}
					bulk[idx].x = x;
					bulk[idx].y = y;
					bulk[idx].zoom = cur_zoom;
					bulk[idx].path = strdup(buf);
					bulk[idx].url = url;
					++idx;
					++batch->num_dl_total;
				}
			}
		}
	}

	batch->tasks = bulk;

	/* size (bytes) of existing tiles */
	return size;
}

/**
 * enqueue
 */
void batch_download(batch_dl_t *batch)
{
	++(update_ui_thread.num_downloading_batches);

	tile_downloader_t *td = (tile_downloader_t *)batch->repo->downloader;

	LOCK_MUTEX(&(td->lock));

	batch->state = BATCH_DL_STATE_PENDING;
	batch->next = NULL;
	batch->cur_task_id = 0;

	if (td->batches) {
		batch->prev = td->batches_tail;
		td->batches_tail = td->batches_tail->next = batch;
	} else {
		td->batches = td->batches_tail = batch;
		batch->prev = NULL;
	}
	if (td->cur_batch == NULL) {
		td->cur_batch = batch;
		td->cur_batch->state = BATCH_DL_STATE_PROCESSING;
	}

	++(td->batch_count);
	++(td->unfinished_batch_count);

	int new_thread_count = batch->num_dl_total / 5;
	if (new_thread_count == 0)
		new_thread_count = 1;
	new_thread_count = MIN(new_thread_count, (TILE_DL_THREADS_LIMIT - td->dl_threads_count));

	int i;
	for (i=0; i<new_thread_count; i++) {
		tile_downloader_create_thread(td);
	}
	pthread_cond_broadcast(&(td->cv));

	UNLOCK_MUTEX(&(td->lock));

	if (update_ui_thread.thread_tid == 0) {
		pthread_create(&update_ui_thread.thread_tid, &pthread_attr,
			batch_download_update_ui_routine, NULL);
	}
}

/**
 * NOTE:
 * (1) require <td>'s lock being locked;
 * (2) invariant: dl_threads_count == TILE_DL_THREADS_LIMIT
 * return slot id
 */
gboolean tile_downloader_create_thread(tile_downloader_t *td)
{
	int i;
	for (i=0; i<TILE_DL_THREADS_LIMIT; i++) {
		if (td->dl_threads[i].tid == 0) {
			td->dl_threads[i].downloader = td;
			td->dl_threads[i].id = i;
			if (pthread_create(&td->dl_threads[i].tid, &pthread_attr, download_routine, &(td->dl_threads[i])) == 0) {
				while (td->dl_threads[i].tid == 0)
					sleep_ms(100);
				++(td->dl_threads_count);
				return TRUE;
			} else {
				log_error("create download pthread failed");
				return FALSE;
			}
		}
	}

	/* should not go here */
	return FALSE;
}

void* batch_download_update_ui_routine(void *arg)
{
	sleep(1);

	sigset_t sig_set;
	sigemptyset(&sig_set);
	sigaddset(&sig_set, SIGINT);
	pthread_sigmask(SIG_BLOCK, &sig_set, NULL);

	pthread_context_t *ctx = register_thread("tile downloader update UI thread", NULL, NULL);

	/* no harm when we update UI one more time, if the unfinished_batch_count becomes 0.
	 * The major problem here is to avoid dead lock */

	while (! update_ui_thread.stop) {
		/* always lock GUI lock first! */
		LOCK_UI();
		update_batch_dl_status();
		UNLOCK_UI();

		if (update_ui_thread.num_downloading_batches == 0)
			break;
		sleep(1);
	}

	LOCK_UI();
	update_batch_dl_status();
	update_ui_thread.thread_tid = 0;
	UNLOCK_UI();

	free(ctx);

	return NULL;
}

static void init_repo_tile_downloader(map_repo_t *repo, void *arg)
{
	tile_downloader_t *td = (tile_downloader_t *)calloc(1, sizeof(tile_downloader_t));

	if (! td) {
		log_warn("allocate memory failed");
		exit(0);
	}

	td->repo = repo;
	repo->downloader = (void *)td;

	td->unfinished_batch_count = 0;
	td->batches = NULL;
	td->batches_tail = NULL;
	td->cur_batch = NULL;
	td->batch_count = 0;
	td->dl_threads_count = 0;
	td->front_task_count = 0;
	td->front_tasks = NULL;
	td->front_tasks_tail = NULL;
	td->stop = FALSE;

	pthread_mutex_init(&(td->lock), NULL);
	pthread_cond_init(&(td->cv), NULL);
}

static void cleanup_repo_tile_downloader(map_repo_t *repo, void *arg)
{
	tile_downloader_t *td = (tile_downloader_t *)repo->downloader;

	LOCK_MUTEX(&(td->lock));

	if (td->dl_threads_count > 0) {
		int i;
		dl_thread_t *thread;

		td->stop = TRUE;

		for (i = 0; i<td->dl_threads_count; i++) {
			thread = &(td->dl_threads[i]);
			pthread_kill(thread->tid, SIGUSR1);
			sleep_ms(100);
			pthread_join(thread->tid, NULL);
		}

		batch_dl_t *batch, *next;
		batch = td->batches;
		while (batch) {
			next = batch->next;
			if (batch->tasks) {
				for (i = 0; i<batch->num_dl_total; i++) {
					if (batch->tasks[i].path) {
						free(batch->tasks[i].path);
						batch->tasks[i].path = NULL;
					}
					if (batch->tasks[i].url) {
						free(batch->tasks[i].url);
						batch->tasks[i].url = NULL;
					}
				}
				free(batch->tasks);
				free(batch);
			}
			batch = next;
		}
	}

	UNLOCK_MUTEX(&(td->lock));
	free(td);
}

/**
 * init module
 */
void tile_downloader_module_init()
{
	update_ui_thread.stop = FALSE;
	update_ui_thread.thread_tid = 0;
	update_ui_thread.num_downloading_batches = 0;

	pthread_attr_init(&pthread_attr);
	pthread_attr_setdetachstate(&pthread_attr, PTHREAD_CREATE_DETACHED);

	mapcfg_iterate_maplist(init_repo_tile_downloader, NULL);
}

void tile_downloader_module_cleanup()
{
	mapcfg_iterate_maplist(cleanup_repo_tile_downloader, NULL);

	if (update_ui_thread.thread_tid > 0) {
		update_ui_thread.stop = TRUE;
		pthread_kill(update_ui_thread.thread_tid, SIGUSR1);
		sleep_ms(100);
		pthread_join(update_ui_thread.thread_tid, NULL);
	}

	free_pending_frees();

	pthread_attr_destroy(&pthread_attr);
}
