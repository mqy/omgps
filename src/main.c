#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <dirent.h>
#include <locale.h>

#include "omgps.h"
#include "network.h"
#include "dbus_intf.h"
#include "sound.h"
#include "gps.h"
#include "util.h"
#include "uart.h"
#include "py_ext.h"
#include "customized.h"
#include "track.h"

static pthread_mutex_t cleanup_lock = PTHREAD_MUTEX_INITIALIZER;
static char pid_file[128];
static int pid_fd = -1;

#if (PLATFORM_FSO)
#define DEFAULT_FONT "Sans Bold 3.5"
#else
#define DEFAULT_FONT "Sans Bold 12"
#endif

/* SHR (20090509) set expander-size as 40, too large! */

static const char *style_string =
	"gtk-font-name = \""DEFAULT_FONT"\"\n"
	"style \"scroll\" { GtkScrollbar::slider-width = 25 }\n"
	"class \"*\" style \"scroll\"\n"
	"style \"treeview\" { GtkTreeView::expander-size = 18 }\n"
	"class \"*\" style \"treeview\"\n";

/**
 * Called by: atexit, signal handler, window delete event handler
 */
static void cleanup()
{
	static gboolean done = FALSE;
	if (TRYLOCK_MUTEX(&cleanup_lock) != 0)
		return;

	if (done) {
		UNLOCK_MUTEX(&cleanup_lock);
		return;
	}

	done = TRUE;

	if (g_init_status < SETTINGS_LOADED)
		goto END;

#if (PLATFORM_FSO)
	track_cleanup();
	stop_poll_thread();
#endif

	if (g_init_status >= MAP_INITED) {

		tile_downloader_module_cleanup();

		map_cleanup();

		drawing_cleanup();

		g_cfg->last_center_lat = g_view.center_wgs84.lat;
		g_cfg->last_center_lon = g_view.center_wgs84.lon;

		g_cfg->last_lat = g_view.pos_wgs84.lat;
		g_cfg->last_lon = g_view.pos_wgs84.lon;

		if (!isnan(g_gpsdata.height) && !isnan(g_gpsdata.hacc)) {
			g_cfg->last_alt = g_gpsdata.height;
			g_cfg->last_pacc = g_gpsdata.hacc;
		}

		gboolean ret = settings_save();
		if (! ret)
			log_warn("save settings: %s\n", (ret? "OK" : "failed"));

		mapcfg_cleanup();
	}

END:

#if (PLATFORM_FSO)
	if (g_context.suspend_disabled) {
		if (! dbus_release_resource("CPU"))
			log_warn("Release resource (CPU) failed.");
	}

	sound_cleanup();
	dbus_cleanup();
#endif

	py_ext_cleanup();

	close_log();

	if (pid_fd > 0) {
		flock(pid_fd, LOCK_UN);
		close(pid_fd);
		unlink(pid_file);
		pid_fd = 0;
	}

	UNLOCK_MUTEX(&cleanup_lock);
}

void main_exit()
{
	cleanup();
	exit(0);
}

static gboolean delete_event(GtkWidget *widget, gpointer data)
{
	cleanup();
	exit(0);
	return FALSE;
}

static void sig_handler(int signo)
{
	char *signame;

	switch (signo) {
	case SIGINT:
		signame = "SIGINT";
		break;
	case SIGABRT:
		signame = "SIGABRT";
		break;
	case SIGFPE:
		signame = "SIGFPE";
		break;
	case SIGKILL:
		signame = "SIGKILL";
		break;
	case SIGSEGV:
		signame = "SIGSEGV";
		break;
	case SIGUSR1:
		signame = "SIGUSR1";
		break;
	default:
		signame = "?";
		break;
	}

	pthread_context_t *ctx = get_thread_contex();
	char *name = ctx? ctx->name : "unknown";

	pthread_t tid = pthread_self();

	if (signo != SIGINT && signo != SIGUSR1) {
		log_warn("thread (name=%s, tid=%p): get signal: %s", name, tid, signame);
		if (ctx) {
			thread_cleanup_func_t cleanup_func = ctx->cleanup_func;
			if (cleanup_func) {
				(*cleanup_func)(ctx);
			}
		}
		warn_dialog("program crash, exit");
		exit(0);
	}

	if (ctx && ctx->is_main_thread) {
		cleanup();
		exit(0);
	} else {
		//log_info("pthread exit: (name=%s, tid=%p)", name, tid);
		pthread_exit(NULL);
	}
}

/**
 * Takes effects on by all threads
 */
void install_sig_handler(void sig_handler(int signo))
{
	struct sigaction act;
	act.sa_handler = sig_handler;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_RESTART;

	sigaction(SIGINT, &act, NULL);
	sigaction(SIGABRT, &act, NULL);
	sigaction(SIGFPE, &act, NULL);
	sigaction(SIGKILL, &act, NULL);
	sigaction(SIGSEGV, &act, NULL);
	sigaction(SIGUSR1, &act, NULL);
}

static void atexit_handler()
{
	cleanup();
	exit(0);
}

static void create_dir(const char *parent_dir, const char *dir, char **target)
{
	char buf[512];
	struct stat st;

	int n = snprintf(buf, sizeof(buf), "%s/%s", parent_dir, dir);
	if (n < 0) {
		sprintf(buf, "unable to create dir string \"%s\"", dir);
		warn_dialog(buf);
	}
	if (stat(buf, &st) < 0 && mkdir(buf, 0700) < 0) {
		sprintf(buf, "failed to create dir \"%s\"", dir);
		warn_dialog(buf);
		exit(0);
	}
	*target = strdup(buf);
}

/**
 * NOTE: these dirs are auto freed when program exit.
 */
static void create_dirs()
{
	char *home = getenv("HOME");

	if (home == NULL) {
		warn_dialog("env 'HOME' is not set!");
		exit(0);
	}

	create_dir(home, TOP_DIR, &g_context.top_dir);

	create_dir(g_context.top_dir, "config_"VERSION, &g_context.config_dir);

	create_dir(g_context.top_dir, "maps", &g_context.maps_dir);

	create_dir(g_context.top_dir, "track", &g_context.track_dir);

	create_dir(g_context.top_dir, "screenshot", &g_context.screenshot_dir);

	printf("Settings dir=%s\n", g_context.top_dir);
}

/**
 * ETCCONF_DIR is defined in Makefile.am
 */
static void link_config_files()
{
	#ifndef CONF_DIR
	#define CONF_DIR "/etc/omgps/"VERSION
	#endif

	char buf[256], buf1[256];
	struct stat st;
	struct dirent *ep;
	char *fname;
	char *dir = CONF_DIR;

	DIR *dp = opendir (dir);

	if (dp == NULL) {
		warn_dialog("unable to list files in config dir");
		exit(0);
	}

	while ((ep = readdir (dp))) {
		fname = ep->d_name;
		if (ep->d_type == DT_DIR) /* . and .. */
			continue;

		snprintf(buf, sizeof(buf), "%s/%s", g_context.config_dir, fname);

		/* FIXME: check file is a regular file or a soft link to a regular file */
		if (stat(buf, &st) == 0 && ep->d_type != DT_DIR)
			continue;

		snprintf(buf1, sizeof(buf1), "%s/%s", dir, fname);
		symlink(buf1, buf);
	}
}

static void create_pid_file()
{
	struct stat st;
	char *err = NULL;

	snprintf(pid_file, sizeof(pid_file), "%s/omgps.pid", g_context.top_dir);

	int fd = open(pid_file, O_RDWR | O_CREAT, 0644);
	if (fd < 0) {
		err = "Unable to create pid file. exit.";
		goto END;
	}

	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		err = (errno == EWOULDBLOCK)
				? "Another OMGPS instances is executing. exit."
				: "Lock pid file failed.";
		goto END;
	}

	if (fstat(fd, &st) < 0 && errno != ENOTDIR) {
		err = "Unable to check pid file, exit.";
		goto END;
	}

	if (st.st_size > 0 && ftruncate(fd, 0) < 0) {
		err = "Unable to truncate pid file. exit";
		goto END;
	}

	char buf[20];
	sprintf(buf, "%u", (U4) getpid());
	if (write(fd, buf, strlen(buf)) < 0) {
		err = "Unable to check pid file, exit.";
		goto END;
	}

END:

	if (err == NULL) {
		pid_fd = fd;
		return; // leave the pid file locked
	} else {
		warn_dialog(err);
		if (fd > 0)
			flock(fd, LOCK_UN);
		exit(0);
	}
}

static void init_ui()
{
	drawing_init(g_window);

	register_ui_panes();

#if (! PLATFORM_FSO)
	gtk_window_unmaximize (GTK_WINDOW(g_window));
	gtk_window_move(GTK_WINDOW(g_window), 300, 200);
	gtk_window_resize(GTK_WINDOW(g_window), 480, 590);
#endif
	g_init_status = WINDOW_SHOWN;
}

static void load_map_and_settings()
{
	thread_context_clear_errbuf();

	gboolean map_ok = mapcfg_load();
	if (! map_ok) {
		warn_dialog(thread_context_get_errbuf());
		exit(0);
	}

	thread_context_clear_errbuf();
	g_cfg = settings_load();
	char *err = thread_context_get_errbuf();
	if (strlen(err) > 0) {
		warn_dialog(err);
		exit(0);
	}

	g_view.fglayer.repo = mapcfg_get_default_repo(g_cfg->last_map_name);

	if (g_view.fglayer.repo == NULL) {
		warn_dialog("no map found. exit");
		exit(0);
	}
}

static void init_g_context_vars()
{
	g_view.invalidate = FALSE;
	g_context.dl_if_absent = TRUE;
	g_context.track_enabled = FALSE;
	g_context.show_rulers = FALSE;
	g_context.show_latlon_grid = FALSE;
	g_context.uart_conflict = FALSE;
	g_context.time_synced = FALSE;
	g_context.map_view_frozen = FALSE;
	g_context.fullscreen = FALSE;
	g_context.sound_enabled = FALSE;
	g_context.suspend_disabled = FALSE;
	g_context.run_gps_on_start = TRUE;
	/* enable when center button is clicked, disable when after being dragged */
	g_context.cursor_in_view = TRUE;

	g_context.poll_state = POLL_STATE_SUSPENDING;
	g_context.speed_unit = SPEED_UNIT_KMPH;
}

static void init(gboolean log2console)
{
	init_pthread_key();

	pthread_context_t *ctx = register_thread("main thread", NULL, NULL);
	ctx->is_main_thread = TRUE;

	g_init_status = 0;

	atexit(atexit_handler);

	/* NOTE: related to bug# 6 */
	setlocale(LC_ALL, "C");

	/* create dirs if not exists, save as strings */
	create_dirs();

	/* also lock the pid file */
	create_pid_file();

	link_config_files();

	gtk_window_maximize(GTK_WINDOW(g_window));
	gtk_widget_show(g_window);

	/* open log before other modules */

	char file[256];
	snprintf(file, sizeof(file), "%s/%s", g_context.top_dir, "omgps.log");

	if (! open_log(log2console? NULL : file)) {
		warn_dialog("Can not open log file!, exit");
		exit(0);
	}

	py_ext_init();

	/* load map and settings */
	load_map_and_settings();

	log_info("settings loaded.");

	g_init_status = SETTINGS_LOADED;

	//g_ubx_receiver_versions[0] = '\0';

	g_view.fglayer.tile_cache = NULL;
	g_view.fglayer.tile_cache = NULL;

	init_g_context_vars();

	/* Initialize tile downloader */
	tile_downloader_module_init();

	g_init_status = DOWNLOADER_INITED;

	/* init UI */
	init_ui();

#if (PLATFORM_FSO)
	if (! check_device_files())
		exit(0);

	status_label_set_text("Connecting to dbus...", FALSE);

	if (! dbus_init())
		status_label_set_text("<span color='red'>Connect to dbus failed</span>", TRUE);

	/* "OS auto suspend" is disabled while GPS is running */
	g_context.suspend_disabled = dbus_request_resource("CPU");

	if (! start_poll_thread()) {
		char *msg = "Can't start polling thread";
		log_error(msg);
		warn_dialog(msg);
		exit(0);
	}

	sound_init();
#endif
}

int main(int argc, char **argv)
{
	gboolean log2console = TRUE;
	if (argc == 2) {
		if (strcmp(argv[1], "-log2file") == 0)
			log2console = FALSE;
	}

	g_type_init();

	if (! g_thread_supported ())
		g_thread_init(NULL);

	gdk_threads_init();

	gdk_threads_enter();

	/* styles string */
	if (style_string)
		gtk_rc_parse_string (style_string);

	gtk_init(&argc, &argv);

	/* SHR 20090615: can't set font in rc */
	gtk_settings_set_string_property (gtk_settings_get_default(),
		"gtk-font-name", DEFAULT_FONT, "foobar");

	gtk_widget_set_default_colormap(gdk_rgb_get_colormap());

	g_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_position(GTK_WINDOW(g_window), GTK_WIN_POS_CENTER);
	g_signal_connect (g_window, "delete_event", G_CALLBACK (delete_event), NULL);

	install_sig_handler(sig_handler);

	init (log2console);

	gtk_main();
	gdk_threads_leave();
	return 0;
}
