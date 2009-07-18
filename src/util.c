#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <string.h>
#include <assert.h>
#include <regex.h>
#include <unistd.h>
#include <fcntl.h>
#include <glib.h>

#include "util.h"

/****************************************** log ***************************************/

static FILE *logfp = NULL;
static pthread_mutex_t loglock = PTHREAD_MUTEX_INITIALIZER;
static gboolean log2console = FALSE;

static char *log_priority_names[] = {
		"DEBUG",
		"INFO",
		"WARN",
		"ERROR",
		"FATAL", };

void close_log()
{
	if (!log2console) {
		if (logfp != NULL)
			fclose(logfp);
	}
}

gboolean is_log2console()
{
	return log2console;
}

gboolean open_log(char *file_path)
{
	if (file_path == NULL) {
		log2console = TRUE;
		logfp = stdout;
		return TRUE;
	}

	logfp = fopen(file_path, "w+");
	if (logfp == NULL)
		return FALSE;

	return TRUE;
}

void gps_log(log_level_t level, const char *fmt, ...)
{
	if (logfp == NULL || level < GPS_LOG_DEBUG || level >= GPS_LOG_LEVELS)
		return;

	pthread_mutex_lock(&loglock);

	char buf0[32], buf[256];

	time_t tt = time(NULL);
	struct tm *tm = localtime(&tt);
	strftime(buf0, sizeof(buf0), "%Y%m%d %H:%M:%S", tm);

	fprintf(logfp, "%s  [%s]  ", buf0, log_priority_names[level]);

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	fprintf(logfp, "%s\n", buf);
	fflush(logfp);

	pthread_mutex_unlock(&loglock);
}

void sleep_ms(long ms)
{
	struct timespec ts;
	ts.tv_sec = ms / 1000;

	/* 0 .. 999999999 */
	ts.tv_nsec = (ms - ts.tv_sec * 1000) * 1000000;

	nanosleep(&ts, NULL);
}

int wait_ms(long span_ms, pthread_cond_t *cond, pthread_mutex_t *mutex, gboolean lock)
{
	struct timeval tv;
	struct timespec ts;

	int ret = 0;

	if (span_ms > 0) {
		const int k = 1000;
		const int m = k * k;
		const long b = k * m;

		gettimeofday(&tv, NULL);
		long second = span_ms / k;
		long ms = span_ms % m;
		long ns = tv.tv_usec * k + ms * m;
		if (ns >= b) {
			second += 1;
			ns -= b;
		}
		ts.tv_sec = tv.tv_sec + second;
		ts.tv_nsec = ns;
	}

	if (lock)
		pthread_mutex_lock(mutex);

	if (span_ms > 0)
		ret = pthread_cond_timedwait(cond, mutex, &ts);
	else
		ret = pthread_cond_wait(cond, mutex);

	if (lock)
		pthread_mutex_unlock(mutex);

	return ret;
}

/**
 * return: buf size written
 */
int format_time(struct tm *t, char *buf, int buf_len)
{
	if (t == NULL)
		return 0;

	return strftime(buf, buf_len, "%a, %Y-%m-%d %H:%M:%S %Z", t);
}

#define BLANK_CHAR(c) \
	(c == ' ' || c == '\t' || c == '\r' || c == '\n')

/**
 * Trim left ' ', '\t', '\r', or '\n'
 */
char *ltrim(char *str)
{
	if (str == NULL/* || *str == '\0'*/)
		return NULL;

	char c;
	/* ltrim */
	for (c = *str; BLANK_CHAR(c); c = *(++str))
		;
	return str;
}

/**
 * Trim right ' ', '\t', '\r', or '\n'
 */
char *rtrim(char *str)
{
	if (str == NULL/* || *str == '\0'*/)
		return NULL;

	int len = strlen(str);
	if (len == 0)
		return str;

	char c;
	char *p = (char *) (str + len - 1);
	for (c = *p; BLANK_CHAR(c); c = *(--p))
		;
	*(p + 1) = '\0';

	return str;
}

char *trim(char *str)
{
	char *p = ltrim(str);
	if (p != NULL)
		p = rtrim(p);
	return p;
}

/* http://en.wikipedia.org/wiki/E-mail_address
 * http://haacked.com/archive/2007/08/21/i-knew-how-to-validate-an-email-address-until-i.aspx
 * http://en.wikipedia.org/wiki/Regular_expression
 * http://www.ex-parrot.com/~pdw/Mail-RFC822-Address.html
 *
 * Even if it passed the above validation, we still don't know if the email is llh_valid.
 * Not to say there are CJK domain names... so just do a simple check here.
 */
gboolean validate_email(char *email)
{
	assert(email);
	/* [local@(sub.)+top]
	 * Local address makes no sense in the case of AGPS online */
	#define CH "[^@]"
	const char *regex = "^" CH "+@(" CH "+\\.)+" CH "{2,}$";

	regex_t preg;
	/* should pass the compilation. error can be detected during dev phase.
	 * performance is not a consideration */
	assert(regcomp(&preg, regex, REG_EXTENDED|REG_ICASE|REG_NOSUB) == 0);

	int valid = regexec(&preg, email, 0, NULL, 0) == 0;

	regfree(&preg);

	return valid;
}

gboolean exec_linux_cmd(char *candidates[], int n, char *args[])
{
	int statloc;
	pid_t pid;
	int i;
	struct stat st;
	char *cmd = NULL;
	for (i = 0; i < n; i++) {
		if (stat(candidates[i], &st) == 0) {
			cmd = candidates[i];
			break;
		}
	}
	if (cmd == NULL) {
		log_error("Linux command does not exist: %s", args[0]);
		return FALSE;
	}

	if ((pid = vfork()) < 0) {
		return FALSE;
	} else if (pid == 0) {
		int exit_code = execv(cmd, args);
		if (exit_code != 0)
			log_warn("exec linux command \"%s\" failed, exit code=%d", args[0], exit_code);
		exit(exit_code == 0);
	} else {
		if (waitpid(pid, &statloc, 0) != pid)
			return FALSE;
		if (!(WIFEXITED(statloc) && WEXITSTATUS(statloc) == 0))
			return FALSE;
		return TRUE;
	}
}

static pthread_key_t thread_key;

void init_pthread_key()
{
	pthread_key_create(&thread_key, NULL);
}

pthread_context_t * register_thread(char *name, void *arg, thread_cleanup_func_t cleanup_func)
{
	pthread_context_t *ctx = (pthread_context_t *) malloc(sizeof(pthread_context_t));
	if (! ctx) {
		log_warn("unable to allocate memory for pthread_context_t");
		exit(0);
	}

	ctx->name = name;
	ctx->arg = arg;
	ctx->cleanup_func = cleanup_func;
	ctx->is_main_thread = FALSE;
	pthread_setspecific(thread_key, ctx);

	return ctx;
}

inline pthread_context_t* get_thread_contex()
{
	return pthread_getspecific(thread_key);
}

void thread_context_clear_errbuf()
{
	pthread_context_t *ctx = pthread_getspecific(thread_key);
	memset(ctx->errbuf, 0, ERRBUF_LEN);
}

char* thread_context_get_errbuf()
{
	pthread_context_t *ctx = pthread_getspecific(thread_key);
	return ctx->errbuf;
}
