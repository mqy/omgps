#ifndef UTIL_H_
#define UTIL_H_

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <glib.h>
#include <gdk/gdk.h>

typedef enum
{
	GPS_LOG_DEBUG	= 0,
	GPS_LOG_INFO,
	GPS_LOG_WARN,
	GPS_LOG_ERROR,
	GSP_LOG_FATAL,
	GPS_LOG_LEVELS
} log_level_t;

struct __pthread_context_t;

extern pthread_t g_gdk_lock_owner;

#define ERRBUF_LEN	256
typedef void (*thread_cleanup_func_t)(struct __pthread_context_t *ctx);

typedef struct __pthread_context_t
{
	char *name;
	pthread_t tid;
	char *arg;
	thread_cleanup_func_t cleanup_func;
	gboolean is_main_thread;
	char errbuf[ERRBUF_LEN];
} pthread_context_t;

#define DEBUG_UI_LOCK FALSE;

#define LOCK_UI()	gdk_threads_enter()
#define UNLOCK_UI()	{ gdk_flush(); gdk_threads_leave(); }

extern gboolean open_log(char *file_path);
extern void close_log();
extern void gps_log(log_level_t level, const char *fmt,...);

//#define log_debug(fmt,...)	gps_log(GPS_LOG_DEBUG, fmt"\t%s:%d", ## __VA_ARGS__, __FILE__, __LINE__)
#define log_debug(fmt,...)	gps_log(GPS_LOG_DEBUG, fmt, ## __VA_ARGS__)
#define log_info(fmt,...) 	gps_log(GPS_LOG_INFO,  fmt, ## __VA_ARGS__)
#define log_warn(fmt,...) 	gps_log(GPS_LOG_WARN,  fmt, ## __VA_ARGS__)
#define log_error(fmt,...)	gps_log(GPS_LOG_ERROR, fmt, ## __VA_ARGS__)
#define log_fatal(fmt,...)	gps_log(GPS_LOG_FATAL, fmt, ## __VA_ARGS__)

#define LOCK_MUTEX(lk) do {							\
	if (pthread_mutex_trylock(lk) != 0)				\
		pthread_mutex_lock(lk);						\
} while (0)

#define TRYLOCK_MUTEX(lk)	pthread_mutex_trylock(lk)

#define UNLOCK_MUTEX(lk) do {						\
	pthread_mutex_unlock(lk);						\
} while (0)

extern int format_time(struct tm * t, char *buf, int buf_len);
extern void sleep_ms(long ms);
extern int wait_ms(long span_ms, pthread_cond_t *cond, pthread_mutex_t *mutex, gboolean lock);
extern gboolean exec_linux_cmd(char *candidates[], int n, char *args[]);

extern char *ltrim(char *str);
extern char *rtrim(char *str);
extern char *trim(char *str);

extern gboolean validate_email(char *email);

extern void init_pthread_key();
extern pthread_context_t * register_thread(char *name, void *arg, thread_cleanup_func_t cleanup_func);
extern pthread_context_t * get_thread_contex();
extern void thread_context_clear_errbuf();
extern char* thread_context_get_errbuf();

#endif /* UTIL_H_ */
