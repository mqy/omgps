#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "ubx.h"
#include "network.h"
#include "customized.h"
#include "util.h"
#include "uart.h"
#include "gps.h"
#include "omgps.h"

/**
 * AGPS online. References:
 * 1. ANTARIS_Protocol_Specification(GPS.G3-X-03002).chm
 * 2. ImplementationAssistNowServerAndClient(GPS.G4-SW-05017-C).pdf
 * 3. Openmoko wiki documents
 */

/* without end '\0' */
typedef struct __str_t
{
	U1 *addr;
	U4 len;
} str_t;

typedef struct __agps_args_t
{
	char *user;
	char *pwd;
	double lat;
	double lon;
	double height;
	int pacc;
} agps_args_t;

typedef struct __aid_args_t {
	char *file;
	char *name;
	U1 msg_id;
	int payload_len;
	int msg_count;
} aid_args_t;

#define AGPS_ONLINE_BUF_LEN	4096
#define PREFIX				"AGPS: "

#define MIN_DUMP_INTERVAL_S	600

static GtkWidget *lockview_button, *submit_button;

static point_t agps_point;

static mouse_handler_t mouse_agpsonline_handler;

static char *aid_alm_file = NULL;
static char *aid_eph_file = NULL;
static char *aid_hui_file = NULL;

#define AID_ALM_FILE	"aid_alm.dat"
#define AID_EPH_FILE	"aid_eph.dat"
#define AID_HUI_FILE	"aid_hui.dat"

static void * agps_online_routine(void *args);
static void agps_redraw_view();

/**
 * These memory will be automatically freed at exit.
 */
static void get_aid_file_names()
{
	if (aid_alm_file != NULL)
		return;

	char buf[256];

	snprintf(buf, sizeof(buf), "%s/%s", g_context.top_dir, AID_ALM_FILE);
	aid_alm_file = strdup(buf);

	snprintf(buf, sizeof(buf), "%s/%s", g_context.top_dir, AID_EPH_FILE);
	aid_eph_file = strdup(buf);

	snprintf(buf, sizeof(buf), "%s/%s", g_context.top_dir, AID_HUI_FILE);
	aid_hui_file = strdup(buf);
}

/**
 * The problem is: if last location is out of range or PACC, we take fairly long
 * time to get TTFF.
 */
static gboolean set_aid_ini()
{
	llh_ecef_t data;

	data.lat = g_cfg->last_lat;
	data.lon = g_cfg->last_lon;
	data.h = g_cfg->last_alt;

	wgs84_lla_to_ecef(&data);

	int X = (int)(data.x * 100);
	int Y = (int)(data.y * 100);
	int Z = (int)(data.z * 100);
	U4 pacc = (U4)(AID_INI_PACC_KM * 1000);

	ubx_msg_type_t type = {UBX_CLASS_AID, UBX_ID_AID_INI};
	U4 flags = 0x01 | 0x08;

	U1 packet[8+48] = {
		0xB5, 0x62,
		type.class, type.id,
		0x30, 0x00, /* length 48 bytes */
		X & 0xFF, (X >> 8) & 0xFF, (X >> 16) & 0xFF, (X >> 24) & 0xFF, /* X */
		Y & 0xFF, (Y >> 8) & 0xFF, (Y >> 16) & 0xFF, (Y >> 24) & 0xFF, /* Y */
		Z & 0xFF, (Z >> 8) & 0xFF, (Z >> 16) & 0xFF, (Z >> 24) & 0xFF, /* Z */
		pacc & 0xFF, (pacc >> 8) & 0xFF, (pacc >> 16) & 0xFF, (pacc >> 24) & 0xFF, /* POSACC */
		/* Don't set time related args, due to normally incorrect system time */
		0x00, 0x00, /* TM_CFG */
		0x00, 0x00, /* WN */
		0x00, 0x00, 0x00, 0x00, /* TOW */
		0x00, 0x00, 0x00, 0x00, /* TOW_NS */
		0x00, 0x00, 0x00, 0x00, /* TACC_MS */
		0x00, 0x00, 0x00, 0x00, /* TACC_NS */
		0x00, 0x00, 0x00, 0x00, /* CLKD */
		0x00, 0x00, 0x00, 0x00, /* CLKD_ACC */
		flags, 0x00, 0x00, 0x00, /* FLAGS */
		0x00, 0x00 /* check sum */
	};

	return ubx_issue_cmd(packet, sizeof(packet));
}

static gboolean set_aid_data(char *file, int size)
{
	U1 *buf = NULL;
	int fd = 0;
	gboolean ret = TRUE;

	buf = (U1*) malloc(size);
	if (!buf) {
		log_warn("Can not allocate memory");
		return FALSE;
	}

	fd = open(file, O_RDONLY, 0644);
	if (fd < 0) {
		ret = FALSE;
		goto END;
	}

	if (read(fd, buf, size) != size) {
		ret = FALSE;
		goto END;
	}

	if (!ubx_send_request(buf, size)) {
		ret = FALSE;
		goto END;
	}
	log_info("%s was loaded as AID data.", file);

END:

	if (fd > 0)
		close(fd);

	if (buf)
		free(buf);

	return ret;
}

/**
 * Reference: GPS.G1-X-00006.pdf
 *
 * U-BLOX 5 document recommend sequence: INI, EPH, OPTIONAL HUI, ALM ...
 * U-BLOX AsisstNow online data provides: INI, EPH, ALM.
 * NOTE: invalid INI data cause longer TTFF.
 */
void set_initial_aid_data()
{
	/* try loading initial AID data from local cache into GPS receiver */
	gboolean ret = FALSE;

	get_aid_file_names();

	char *msg = "Set local cached AID data...";
	log_info(msg);
	LOCK_UI();
	status_label_set_text(msg, FALSE);
	UNLOCK_UI();

	if (! set_aid_ini())
		goto END;

	struct timeval time;
	struct stat st;
	gettimeofday(&time, NULL);

	char *files[] = {aid_eph_file, aid_hui_file, aid_alm_file};
	int intervals[] = {EPH_VALID_HOURS * 3600, HUI_VALID_HOURS * 3600, ALM_VALID_DAYS * 24 * 3600};
	int i;
	for (i=0; i<sizeof(files)/sizeof(char *); i++) {
		if (stat(files[i], &st) == 0 && st.st_mtim.tv_sec + intervals[i] >= time.tv_sec)
			ret = set_aid_data(files[i], st.st_size) || ret;
	}

END:

	msg = ret ? "Cached AID data was submitted." : "Initial AID data is invalid, skip.";
	log_info(msg);

	LOCK_UI();
	status_label_set_text(msg, FALSE);
	UNLOCK_UI();
}

static gboolean dump_aid_data(aid_args_t *args)
{
	int msg_len, write_len, n, i;
	int counter = 0;
	int total_len = 0;
	int fd;

	ubx_msg_type_t type = { UBX_CLASS_AID, args->msg_id};

	int buf_len = (args->payload_len + 8) * args->msg_count;
	U1 *buf = (U1*) malloc(buf_len);
	if (buf == NULL) {
		log_warn("dump aid: allocate buffer failed");
		return FALSE;
	}

	U1 packet[] = {	0xB5, 0x62,	type.class, type.id, 0x0, 0x00, 0x00, 0x00 };

	log_info("AID-%s: dumping...", args->name);
	if (! ubx_issue_cmd(packet, sizeof(packet)))
		goto END;

	for (i = 0; i < args->msg_count; i++) {
		msg_len = ubx_read_next_aid_message(&buf[total_len], buf_len - total_len, &type, args->payload_len);
		sleep_ms(100);
		if (msg_len == 0) {
			continue;
		} else if (msg_len == -1) {
			counter = -1;
			goto END;
		} else {
			total_len += msg_len;
			++counter;
		}
	}

	char tmp[256];
	snprintf(tmp, sizeof(tmp), "%s.tmp", args->file);

	if (counter > 0) {
		fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd > 0) {
			write_len = 0;
			while ((n = write(fd, &buf[write_len], total_len - write_len)) > 0)
				write_len += n;
			close(fd);
			if (n < 0)
				counter = -1;
		}
	}

END:

	if (buf)
		free(buf);

	if (counter <= 0) {
		unlink(tmp);
		return FALSE;
	} else {
		log_info("AID-%s: %d message(s)", args->name, counter);
		return (rename(tmp, args->file) == 0);
	}
}

/**
 * NOTE: don't attempt to quicken by change send rate to 500 ms or less --
 * it's too fast for default 9600 baud rate.
 * For AGPS online, force dumping EPH and ALM.
 */
gboolean agps_dump_aid_data(gboolean agps_online)
{
	log_info("Dump AID data...");

	get_aid_file_names();

	/* avoid garbage */
	uart_flush_output();

	gboolean ret = FALSE;

	struct stat st;
	struct timeval time;
	gettimeofday(&time, NULL);

	aid_args_t config[] = {
		{aid_eph_file, "EPH", UBX_ID_AID_EPH, 104, 32},
		{aid_alm_file, "ALM", UBX_ID_AID_ALM, 40, 32},
		{aid_hui_file, "HUI", UBX_ID_AID_HUI, 72, 1} /* must be last */
	};

	int i;
	int count = sizeof(config)/sizeof(aid_args_t);
	int skip_counter = 0;

	/* AGPS online data does not contain HUI */
	if (agps_online)
		--count;

	for (i=0; i<count; i++) {
		if (agps_online || (stat(config[i].file, &st) != 0) ||
			(st.st_mtim.tv_sec + MIN_DUMP_INTERVAL_S < time.tv_sec)) {
			if (! dump_aid_data(&config[i])) {
				ret = FALSE;
				break;
			}
		} else {
			++skip_counter;
		}
	}

	ret = (i == count);

	if (ret) {
		log_info("Dump AID data result: types=%d", count - skip_counter);
	} else {
		log_info("Dump AID data: failed");
	}

	if (! g_context.uart_conflict)
		uart_flush_output();

	return ret;
}

/**************************************************************************/

static void agps_online_close()
{
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lockview_button)))
		drawingarea_reset_default_mouse_handler();

	switch_to_main_view(CTX_ID_GPS_FIX);
}

static void lockview_button_toggled(GtkWidget *widget, gpointer data)
{
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lockview_button))) {
		status_label_set_text("Click your location on map, then press\"submit\" button", FALSE);
		map_config_main_view(&mouse_agpsonline_handler, 0x2|0x4|0x8, TRUE, TRUE);
		map_set_redraw_func(&agps_redraw_view);
	} else {
		agps_point.x = agps_point.y = -1;
		map_set_redraw_func(NULL);
		map_config_main_view(NULL, 0x2|0x4|0x8, FALSE, FALSE);
	}
}

/**
 * NOTE: disable agps online until:
 * (1) either finally success or
 * (2) failed during fetching or submitting to GPS chip.
 */
static void submit_button_clicked(GtkWidget *widget, gpointer data)
{
	if (g_gpsdata.latlon_valid) {
		status_label_set_text("GPS data is valid, skip.", FALSE);
	} else {
		pthread_t tid;
		if (pthread_create(&tid, NULL, agps_online_routine, NULL) != 0) {
			warn_dialog("AGPS online: unable to create thread");
			agps_online_close();
		}
	}

	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lockview_button), FALSE);
	gtk_widget_set_sensitive(lockview_button, FALSE);
	gtk_widget_set_sensitive(submit_button, FALSE);
}

static void close_button_clicked(GtkWidget *widget, gpointer data)
{
	agps_online_close();
}

static void draw_cross_sign(point_t center)
{
	if (center.x < 0 || center.y < 0)
		return;

	int r = 15;

	gdk_draw_line(g_view.da->window, g_context.crosssign_gc, center.x - r, center.y, center.x + r, center.y);
	gdk_draw_line(g_view.da->window, g_context.crosssign_gc, center.x, center.y - r, center.x, center.y + r);
}

static void agps_redraw_view()
{
	map_draw_back_layers(g_view.da->window);
	draw_cross_sign(agps_point);
}

static inline void show_lat_lon(point_t point)
{
	point.x += g_view.fglayer.tl_pixel.x;
	point.y += g_view.fglayer.tl_pixel.y;
	coord_t wgs84 = tilepixel_to_wgs84(point, g_view.fglayer.repo->zoom, g_view.fglayer.repo);
	char buf[32];
	snprintf(buf, sizeof(buf), "lat=%f, lon=%f", wgs84.lat, wgs84.lon);
	status_label_set_text(buf, FALSE);
}

static void mouse_released(point_t point, guint time)
{
	draw_cross_sign(agps_point);
	draw_cross_sign(point);

	if (! GTK_WIDGET_SENSITIVE(submit_button))
		gtk_widget_set_sensitive(submit_button, TRUE);

	show_lat_lon(point);
	agps_point = point;
}

static void apgs_get_location_args(agps_args_t *args)
{
	point_t tl = { g_view.fglayer.visible.x, g_view.fglayer.visible.y };
	point_t br = { tl.x + g_view.fglayer.visible.width,	tl.y + g_view.fglayer.visible.height };

	if (! POINT_IN_RANGE(agps_point, tl, br))
		warn_dialog("invalid point: \nout of map range!");

	point_t pt = { agps_point.x + g_view.fglayer.tl_pixel.x,
		agps_point.y + g_view.fglayer.tl_pixel.y};

	/* wgs84 coordinate of clicked point */
	coord_t wgs84 = tilepixel_to_wgs84(pt, g_view.fglayer.repo->zoom, g_view.fglayer.repo);

	args->user = g_cfg->agps_user;
	args->pwd = g_cfg->agps_pwd;
	args->lat = wgs84.lat;
	args->lon = wgs84.lon;
	if (!isnan(g_gpsdata.height))
		args->height = (int) g_gpsdata.height;
	else
		args->height = (int) g_cfg->last_alt;

	/* enlarge it with factor of 3 */
	args->pacc = (int)(g_pixel_meters[g_view.fglayer.repo->zoom]);

	if (args->pacc < 100)
		args->pacc = 100;
}


/**
 * Return: the end index into <buf> of the line.
 */
static int agps_next_line(char *buf, int start, int len)
{
	int i;
	char c;
	for (i = start, c = buf[i];; c = buf[++i]) {
		if (c == '\n') {
			break;
		} else {
			if (c == '\0' || i == len)
				return -1;
		}
	}
	return i;
}
/**
 * return index of content start in <buf>
 * len: data length of <buf>
 * out: data_len
 */
static int agps_parse_response(char *buf, int len, int *data_len)
{
	/* read first line */
	int start, end;

	/* u-blox a-gps demo server (c) 1997-2008 u-blox AG\n */
	start = 0;
	end = agps_next_line(buf, start, len);
	if (end <= 0)
		return -1;

	/* Content-Length: 2488\n */
	start = end + 1;
	end = agps_next_line(buf, start, len);
	if (end <= 0)
		return -2;

	if (sscanf((buf + start), "Content-Length: %d\n", data_len) <= 0)
		return -3;

	if (*data_len <= 0)
		return -4;

	/* Content-Type: application/ubx\n or Content-Type: text/plain\n */
	start = end + 1;
	end = agps_next_line(buf, start, len);
	if (end <= 0)
		return -5;

	/* should be <= , with tailing non-data '\0' */
	if (end + *data_len > len)
		return -6;

	++end; /* advance to next line */
	/* skip new lines (at least one) which contains '\r', '\n' */
	char c;
	for (c = buf[end]; c == '\r' || c == '\n'; c = buf[++end])
		;

	char *line_1 = "Content-Type: application/ubx";
	char *line_2 = "Content-Type: text/plain";
	if (strncmp((buf + start), line_1, strlen(line_1)) != 0) {
		if (strncmp((buf + start), line_2, strlen(line_2)) != 0)
			log_warn(PREFIX"data error=%s", (buf + end));
		return -7;
	}

	return end;
}

/**
 * return: >= 0 the index of data in <buf>,
 * out rtt: round-track (ms); < 0 on error.
 */
static int agps_get_aid_data(char *buf, int buf_len, int *rtt, int *data_len)
{
	int sock_fd = -1;
	int start_index = -1;

	agps_args_t args;
	apgs_get_location_args(&args);

	memset(buf, 0, buf_len);
	if (snprintf(buf, buf_len, "cmd=full;user=%s;pwd=%s;lat=%.4lf;lon=%.4lf;alt=%d;pacc=%d\n",
			args.user, args.pwd, args.lat, args.lon, (int) args.height, args.pacc) < 0)
		goto END;

	LOCK_UI();
	status_label_set_text("AGPS online: connecting...", FALSE);
	UNLOCK_UI();

	sock_fd = connect_remote_with_timeouts(AGPS_SERVER, AGPS_PORT, AF_UNSPEC, SOCK_STREAM, 0,
		10, 15, 15, 15);

	if (sock_fd < 0) {
		log_warn("AGPS online: connect to remote failed: ret=%d", sock_fd);
		return -1;
	}

	LOCK_UI();
	status_label_set_text("AGPS online: sending request data...", FALSE);
	UNLOCK_UI();

	struct timeval start_time, end_time;
	gettimeofday(&start_time, NULL);
	int n = 0, len = 0;

	while ((n = write(sock_fd, buf + len, strlen(buf) - len)) > 0)
		len += n;

	if (n == -1) {
		log_warn(PREFIX"write data to socket failed.");
		goto END;
	}

	memset(buf, 0, buf_len);

	LOCK_UI();
	status_label_set_text("AGPS online: reading response data...", FALSE);
	UNLOCK_UI();

	for (len = 0; (n = read(sock_fd, (buf + len), buf_len - len)) != 0; len += n) {
		if (len >= buf_len || n == -1) {
			/* buffer overflow or I/O error */
			log_warn(PREFIX"buffer overflow!");
			goto END;
		}
	}

	gettimeofday(&end_time, NULL);
	int _rtt = ((end_time.tv_sec * 1000 + end_time.tv_usec / 1000) - (start_time.tv_sec * 1000
			+ start_time.tv_usec / 1000)) / 2;

	/* Parse response */
	start_index = agps_parse_response(buf, len, data_len);

	if (start_index <= 0) {
		log_warn(PREFIX"parse response failed. ret=%d", start_index);
		goto END;
	}

	if (_rtt < 0)
		_rtt = 0; /* in case the system time was changed back */
	*rtt = _rtt;

END:

	close(sock_fd);
	return start_index;
}

static gboolean agps_online_cmd(void *_args)
{
	LOCK_UI();
	status_label_set_text("writing GPS receiver...", FALSE);
	UNLOCK_UI();

	str_t *args = (str_t*) _args;
	U1 *buf = args->addr;
	int data_len = (int) args->len;
	gboolean ret = TRUE;

	gboolean is_ubx = POLL_ENGINE_TEST(UBX);

	int gps_dev_fd = 0;
	if (! is_ubx) {
		if ((gps_dev_fd = uart_open((U4)BAUD_RATE, FALSE)) <= 0) {
			LOCK_UI();
			status_label_set_text("AGPS online: Open UART failed", FALSE);
			UNLOCK_UI();
			ret = FALSE;
			goto END;
		}
		ubx_init(gps_dev_fd);
	}

	/* NOTE: don't read ACKs */
	if (ubx_send_request(buf, data_len)) {
		struct timeval tv;
		gettimeofday(&tv, NULL);

		log_info(PREFIX"data synchronized");

		if (is_ubx) {
			/* better to dump :) */
			LOCK_UI();
			status_label_set_text("Dump AID data as local cache...", FALSE);
			UNLOCK_UI();
			sleep(1);
			agps_dump_aid_data(TRUE);
		}
	} else {
		LOCK_UI();
		status_label_set_text("AGPS online: write data to device failed.", FALSE);
		UNLOCK_UI();
		ret = FALSE;
	}

END:

	if (gps_dev_fd > 0)
		close(gps_dev_fd);

	free(buf);
	free(args);

	LOCK_UI();
	if (ret) {
		agps_online_close();
		status_label_set_text("AGPS online: synchronized.", FALSE);
	} else {
		ctx_tab_agps_online_on_show();
	}
	UNLOCK_UI();

	return ret;
}

static void * agps_online_routine(void *args)
{
	pthread_context_t *ctx = register_thread("AGPS online thread", NULL, NULL);

	int rtt = 0, start_index, data_len;

	if (! guess_network_is_connecting()) {
		LOCK_UI();
		status_label_set_text("AGPS online: no network connection?", FALSE);
		UNLOCK_UI();
	}

	gboolean ret = FALSE;

	char *buf = (char *) malloc(AGPS_ONLINE_BUF_LEN);
	if (! buf) {
		log_error(PREFIX"allocate buffer failed.");
		goto END;
	}

	log_debug(PREFIX"fetching remote data...");
	start_index = agps_get_aid_data(buf, AGPS_ONLINE_BUF_LEN, &rtt, &data_len);

	if (start_index <= 0) {
		log_warn(PREFIX"fetch remote data failed, ret=%d", start_index);
		free(buf);
		goto END;
	}

	char msg[64];
	sprintf(msg, PREFIX"received %d bytes.", data_len);
	LOCK_UI();
	status_label_set_text(msg, FALSE);
	UNLOCK_UI();
	log_info(msg);

	buf = memmove(buf, buf + start_index, data_len);
	str_t *cmd_args = (str_t *) malloc(sizeof(str_t));
	cmd_args->addr = (U1*) (buf);
	cmd_args->len = (U4) data_len;

	if (POLL_ENGINE_TEST(UBX)) {
		if (! issue_ctrl_cmd(agps_online_cmd, cmd_args)) {
			LOCK_UI();
			warn_dialog("Another command is running, please wait a while");
			UNLOCK_UI();
			goto END;
		}
	} else {
		agps_online_cmd(cmd_args);
	}
	ret = TRUE;

END:

	free(ctx);

	if (! ret) {
		LOCK_UI();
		agps_online_close();
		UNLOCK_UI();
	}

	return NULL;
}


void ctx_tab_agps_online_on_show()
{
	agps_point.x = agps_point.y = -1;

	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lockview_button)))
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lockview_button), FALSE);

	gtk_widget_set_sensitive(lockview_button, TRUE);
	gtk_widget_set_sensitive(submit_button, FALSE);
}

GtkWidget * ctx_tab_agps_online_create()
{
	/* initialize mouse handlers */
	mouse_agpsonline_handler.press_handler = NULL;
	mouse_agpsonline_handler.release_handler = mouse_released;
	mouse_agpsonline_handler.motion_handler = NULL;

	GtkWidget *vbox = gtk_vbox_new(FALSE, 0);

	GtkWidget *title_label = gtk_label_new("");
	gtk_label_set_markup(GTK_LABEL(title_label), "<span weight='bold'>U-BLOX AGPS online</span>");
	GtkWidget *sep = gtk_hseparator_new();

	gtk_container_add(GTK_CONTAINER (vbox), title_label);
	gtk_container_add(GTK_CONTAINER (vbox), sep);

	lockview_button = gtk_toggle_button_new_with_label("lock view");
	g_signal_connect (G_OBJECT (lockview_button), "toggled",
			G_CALLBACK (lockview_button_toggled), NULL);

	submit_button = gtk_button_new_with_label("submit");
	g_signal_connect (G_OBJECT (submit_button), "clicked",
			G_CALLBACK (submit_button_clicked), NULL);

	GtkWidget *close_button = gtk_button_new_with_label("close");
	g_signal_connect (G_OBJECT (close_button), "clicked",
			G_CALLBACK (close_button_clicked), NULL);

	GtkWidget *hbox = gtk_hbox_new(TRUE, 5);
	gtk_container_add(GTK_CONTAINER (hbox), lockview_button);
	gtk_container_add(GTK_CONTAINER (hbox), submit_button);
	gtk_container_add(GTK_CONTAINER (hbox), close_button);
	gtk_container_add(GTK_CONTAINER (vbox), hbox);

	return vbox;
}
