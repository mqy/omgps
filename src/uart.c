#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>

#include "omgps.h"
#include "gps.h"
#include "dbus_intf.h"
#include "util.h"
#include "ubx.h"
#include "uart.h"
#include "customized.h"

static struct termios ttyset, ttyset_old;
static int gps_dev_fd = -1;
static int flags;
static int fd_count;
static struct timespec timeout;
static fd_set rs, ws;

#define TIMEOUT (SEND_RATE / 1000 + 5)

/**
 * NOTE: these file paths are subject to change according to kernel and distribution.
 */
static const char *sysfs_gps_power[] = {
	"/sys/bus/platform/devices/neo1973-pm-gps.0/power_on",
	"/sys/bus/platform/devices/neo1973-pm-gps.0/pwron"
};

static const char *gps_uart_file = "/dev/ttySAC1";

/**
 * Check sysfs files (gps power, uart device file)
 */
gboolean check_device_files()
{
	int i, count;
	struct stat st;
	gboolean found = FALSE;

	/* GPS power */
	count = sizeof(sysfs_gps_power) / sizeof(char *);
	for (i=0; i<count; i++) {
		if (stat(sysfs_gps_power[i], &st) == 0) {
			found = TRUE;
			break;
		}
	}
	if (! found) {
		warn_dialog("Sysfs file not found: GPS power");
		return FALSE;
	}

	/* GPS uart */

	if (stat(gps_uart_file, &st) < 0) {
		warn_dialog("Dev file not found: GPS uart");
		return FALSE;
	}

	return TRUE;
}

/**
 * return: 1: already power on; 0: power off; -1: error;
 */
int sysfs_get_gps_device_power()
{
	char c;
	int i, fd = -1, count = sizeof(sysfs_gps_power) / sizeof(char *);

	for (i=0; i<count; i++) {
		fd = open(sysfs_gps_power[i], O_RDONLY);
		if (fd > 0)
			break;
	}

	if (fd < 0)
		return -1;

	int ret = read(fd, &c, 1);
	close(fd);
	if (ret == -1)
		return -1;
	else
		return c == '1'? 1 : 0;
}

/**
 * return: TRUE: OK, FALSE: error
 */
gboolean sysfs_set_gps_device_power(gboolean poweron)
{
	int i, fd = -1, count = sizeof(sysfs_gps_power) / sizeof(char *);

	for (i=0; i<count; i++) {
		fd = open(sysfs_gps_power[i], O_WRONLY);
		if (fd > 0)
			break;
	}

	if (fd < 0)
		return -1;

	char c1 = '1', c0 = '0';

	if (poweron) {
		if (write(fd, &c0, 1) != 1)
			return FALSE;
		sleep_ms(500);
	}

	if (write(fd, poweron? &c1 : &c0, 1) != 1)
		return FALSE;

	close(fd);

	if (poweron)
		sleep(3);

	return TRUE;
}


/* Being executed within polling thread, need lock */
gboolean gps_device_power_on()
{
	log_info("Device is powered off, power on it...");

	g_context.uart_conflict = FALSE;

	if (gps_dev_fd != -1) {
		tcsetattr(gps_dev_fd, TCSANOW, &ttyset_old);
		close(gps_dev_fd);
	}

	LOCK_UI();
	status_label_set_text("Power on GPS...", FALSE);
	UNLOCK_UI();

	sysfs_set_gps_device_power(TRUE);

	log_info("GPS chip is powered on");

	return uart_init();
}

/**
 * select() may update the timeout argument to indicate how much time was left.
 * pselect() does not change this argument.
 * suspend/resume is the major reason that we use read/write with timeout.
 */
inline int read_with_timeout(U1 *buf, int len)
{
	FD_ZERO(&rs);
	FD_SET(gps_dev_fd, &rs);
	int ret = pselect(fd_count, &rs, NULL, NULL, &timeout, NULL);
	if (ret <= 0) {
		log_error("read UART failed: %s", ret == -1? strerror(errno) : "timeout");
		return ret;
	}

	if (FD_ISSET(gps_dev_fd, &rs)) {
		return read(gps_dev_fd, buf, len);
	} else {
		return -1;
	}
}

inline int write_with_timeout(U1 *buf, int len)
{
	FD_ZERO(&ws);
	FD_SET(gps_dev_fd, &ws);
	int ret = pselect(fd_count, NULL, &ws, NULL, &timeout, NULL);
	if (ret <= 0) {
		log_error("write UART failed: %s", ret == -1? strerror(errno) : "timeout");
		return ret;
	}

	if (FD_ISSET(gps_dev_fd, &ws)) {
		return write(gps_dev_fd, buf, len);
	} else {
		return -1;
	}
}

/*
 * If the read size < expected length, retry even if EOF
 */
gboolean inline read_fixed_len(U1 *buf, int expected_len)
{
	int len = 0, count;

	while (TRUE) {
		count = read_with_timeout(&buf[len], expected_len);
		if (count == expected_len) {
			return TRUE;
		} else if (count <= 0) {
			uart_flush_output();
			return FALSE;
		} else {
			len += count;
			expected_len -= count;
			if (expected_len == 0)
				return TRUE;
		}
	}
}

/**
 * the fd is saved to local static var gps_dev_fd.
 *
 * O_NOCTTY:
 * If pathname refers to a terminal device -- see tty(4) -- it will not become
 * the process's controlling terminal even if the process does not have one.
 *
 * O_NONBLOCK or O_NDELAY:
 * When possible, the file is opened in non-blocking mode.  Neither the open()
 * nor any subsequent operations on the file descriptor which is returned  will
 * cause the calling process to wait. For the handling of FIFOs (named pipes),
 * see also fifo(7). For a discussion of the effect of O_NONBLOCK in conjunction
 * with mandatory file locks and with file leases, see fcntl(2).
 */
int uart_open(unsigned int baud_rate, gboolean verify_output)
{
	/* first open with non-blocking mode: we need
	 * 1) if GPS chip is started just now, we must wait until it gets ready.
	 * 2) else sweep possible UBX garbage */
	flags = O_RDWR | O_NOCTTY;

	if (verify_output)
		flags |= O_NONBLOCK;

	if (gps_dev_fd > 0)
		close(gps_dev_fd);

	gps_dev_fd = open(gps_uart_file, flags);

	if (gps_dev_fd < 0) {
		log_error("open device failed: %s\n", strerror(errno));
		return 0;
	}

	/* Save original terminal parameters */
	if (tcgetattr(gps_dev_fd, &ttyset_old) != 0) {
		log_error("get device attribute failed: %s", gps_uart_file);
		return 0;
	}

	speed_t speed = 0;
	switch(baud_rate) {
	case 4800:
		speed = B4800;
		break;
	case 9600:
		speed = B9600;
		break;
	case 19200:
		speed = B19200;
		break;
	case 38400:
		speed = B38400;
		break;
	}

	if (speed == 0) {
		log_warn("bad port baud rate: %d", baud_rate);
		return 0;
	}

	memcpy(&ttyset, &ttyset_old, sizeof(ttyset));

	ttyset.c_iflag = ttyset.c_oflag = 0;
	ttyset.c_cflag |= CS8 | CLOCAL | CREAD;
	ttyset.c_lflag = ~(ICANON | ISIG | ECHO);

	int i;
	for (i = 0; i < NCCS; i++)
		ttyset.c_cc[i] = -1;

	ttyset.c_cc[VMIN] = 1;

	/* unit: 1/10 second, max: 256
	 * NOTE: this also means message send rate must <= 25 seconds. */
	ttyset.c_cc[VTIME] = (U1)(TIMEOUT * 10);

	if (tcsetattr(gps_dev_fd, TCSANOW, &ttyset) != 0) {
		log_error("Unable to set UART attribute: %s", strerror(errno));
		return 0;
	}

	timeout.tv_sec = TIMEOUT; // NOTE
	timeout.tv_nsec = 0;

	fd_count = gps_dev_fd + 1;

	if (verify_output) {
		/* wait until UART has NMEA data output */
		char c;
		while (read(gps_dev_fd, &c, 1) < 0)
			sleep_ms(100);
	}

	/* turn back to blocking mode */
	flags &= ~O_NONBLOCK;
	if (fcntl(gps_dev_fd, F_SETFL, flags) != 0) {
		log_error("Unable to turn UART back to blocking mode: %s", strerror(errno));
		return 0;
	}

	return gps_dev_fd;
}

static void show_status(char *msg)
{
	log_info(msg);
	LOCK_UI();
	status_label_set_text(msg, FALSE);
	UNLOCK_UI();
}

/**
 * baud_rate: only support [1,2,4,8] * 4800
 * called by polling thread!
 */
gboolean uart_init()
{
	log_info("UART init: message rate=%d ms, baud rate=%d...", SEND_RATE, BAUD_RATE);
	show_status("Initializing GPS...");

	/* open UART */
	if (uart_open((U4)BAUD_RATE, TRUE) <= 0) {
		log_error("Open UART failed");
		return FALSE;
	}

	ubx_init(gps_dev_fd);

	if (! ubx_cfg_rate((U2)SEND_RATE, TRUE))
		return FALSE;

	if (! ubx_cfg_prt(0x01, 0x01, 0x01, BAUD_RATE, TRUE))
		return FALSE;

	/* Disable NMEA to avoid flushing UBX binary output */
	if (! ubx_cfg_msg_nmea_ubx(0x0, TRUE, TRUE))
		return FALSE;

	if (! ubx_cfg_msg_nmea_std(0x0, TRUE, TRUE))
		return FALSE;

	if (! ubx_cfg_sbas(TRUE, TRUE))
		return FALSE;

	show_status("Set initial AID data...");

	/* no matter success or failure */
	set_initial_aid_data();

	uart_flush_output();

	show_status("GPS was initialized.");

	//ubx_mon_ver_poll(g_ubx_receiver_versions, sizeof(g_ubx_receiver_versions));
	//show_status(g_ubx_receiver_versions);

	return TRUE;
}

void uart_close()
{
	close(gps_dev_fd);
	gps_dev_fd = -1;
}

void uart_cleanup()
{
	if (gps_dev_fd <= 0 || sysfs_get_gps_device_power() != 1)
		return;

	log_info("cleanup UART...");

	/* power off GPS */
	sysfs_set_gps_device_power(FALSE);

	/* reset serial port to original settings */
	tcsetattr(gps_dev_fd, TCSANOW, &ttyset_old);
	close(gps_dev_fd);
	gps_dev_fd = -1;

	log_info("cleanup UART, done");
}

void uart_flush_output()
{
	tcflush(gps_dev_fd, TCOFLUSH);

	char buf[128];

	/* turn to non-blocking mode, consume (possible) ubx binary garbage */
	if (fcntl(gps_dev_fd, F_SETFL, flags|O_NONBLOCK) != 0) {
		log_error("sweep_garbage, change to non-blocking mode failed.");
		return;
	}

	/* important: wait for pending output to be written to GPS output buffer */
	sleep_ms(SEND_RATE);

	int count = 0;
	int n;

	while ((n = read(gps_dev_fd, buf, sizeof(buf))) > 0) {
		count += n;
		/* hack: lots of NMEA to read?
		 * size of any UBX output block should <= 1K */
		if (count > 1024) {
			log_warn("Detect possible conflict on UART.");
			g_context.uart_conflict = TRUE;
			break;
		}
	}

	/* restore flags */
	if (fcntl(gps_dev_fd, F_SETFL, flags) != 0) {
		log_error("sweep_garbage, change to blocking mode failed.");
		return;
	}
}
