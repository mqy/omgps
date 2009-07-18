#include <termios.h>

#include "omgps.h"
#include "gps.h"
#include "ubx.h"
#include "util.h"
#include "uart.h"

static int gps_dev_fd;

static const ubx_msg_type_t type_nav_posllh = 	{UBX_CLASS_NAV, UBX_ID_NAV_POSLLH};
static const ubx_msg_type_t type_nav_status = 	{UBX_CLASS_NAV, UBX_ID_NAV_STATUS};
static const ubx_msg_type_t type_nav_velned = 	{UBX_CLASS_NAV, UBX_ID_NAV_VELNED};
static const ubx_msg_type_t type_nav_timeutc = 	{UBX_CLASS_NAV, UBX_ID_NAV_TIMEUTC};
static const ubx_msg_type_t type_nav_svinfo = 	{UBX_CLASS_NAV, UBX_ID_NAV_SVINFO};

static const ubx_msg_type_t type_cfg_prt = 		{UBX_CLASS_CFG, UBX_ID_CFG_PRT};
static const ubx_msg_type_t type_cfg_msg = 		{UBX_CLASS_CFG, UBX_ID_CFG_MSG};
static const ubx_msg_type_t type_cfg_rate = 	{UBX_CLASS_CFG, UBX_ID_CFG_RATE};
static const ubx_msg_type_t type_cfg_sbas = 	{UBX_CLASS_CFG, UBX_ID_CFG_SBAS};
static const ubx_msg_type_t type_cfg_rst = 		{UBX_CLASS_CFG, UBX_ID_CFG_RST};
static const ubx_msg_type_t type_cfg_rxm = 		{UBX_CLASS_CFG, UBX_ID_CFG_RXM};

static const ubx_msg_type_t type_mon_ver = 		{UBX_CLASS_MON, UBX_ID_MON_VER};
static const ubx_msg_type_t type_cfg_nav2 = 	{UBX_CLASS_CFG, UBX_ID_CFG_NAV2};

/**
 * <buf>: checksum must be set already.
 */
gboolean ubx_send_request(U1 *buf, int size)
{
	int written = 0;
	while (TRUE) {
		if ((written = write_with_timeout(&buf[written], size)) <= 0)
			return FALSE;
		size -= written;
		if (size == 0)
			break;
		buf += written;
	}
	tcdrain(gps_dev_fd);

	return TRUE;
}

/**
 * NOTE: the packet must end with 2 chars of checksum at count size and size+1.
 */
void ubx_checksum(U1 *packet, int size)
{
	U4 a = 0x00;
	U4 b = 0x00;
	int i = 0;
	while(i<size) {
		a += packet[i++];
		b += a;
	}
	packet[size] = a & 0xFF;
	packet[size+1] = b & 0xFF;
}

static gboolean ubx_read_header(U1 *buf, int next_len)
{
	U1 c;
	int status = 0;
	int ret;

	while (1) {
		if ((ret = read_with_timeout(&c, 1)) != 1) {
			uart_flush_output();
			return FALSE;
		}

		if (c == 0xB5) {
			status = 1;
		} else if (status == 1) {
			if (c == 0x62)
				break;
			else {
				log_warn("read UBX binary header failed, bad stream: "
					"expect 0x62 after 0xB5, but get %x. continue", c);
				continue;
			}
		}
	}

	/* class, id, length: 1 + 1 + 2, or full message -- ack */
	if (! read_fixed_len(buf, next_len)) {
		log_warn("read data after UBX binary header failed.");
		return FALSE;
	}

	return TRUE;
}

/**
 * When messages from the Class CFG are sent to the receiver,
 * the receiver will send an Acknowledge (ACK-ACK (0x05 0x01)) or a Not Acknowledge
 * (ACK-NAK (0x05 0x00)) message back to the sender,
 * depending on whether or not the message was processed correctly.
 * There is no ACK/NAK mechanism for message poll_suspending requests outside Class CFG.
 */
gboolean ubx_read_ack(const ubx_msg_type_t *expected_type)
{
	static U1 ack_buf[8];

	if (! ubx_read_header(ack_buf, 8))
		return FALSE;

	if (ack_buf[0] != UBX_CLASS_ACK || ack_buf[1] != UBX_ID_ACK_ACK ||
		ack_buf[2] != 0x02 || ack_buf[3] != 0x00 ||
		ack_buf[4] != expected_type->class || ack_buf[5] != expected_type->id) {
		log_warn("bad ack: %02X, %02X, %02X, %02X, %02X, %02X",
			ack_buf[0], ack_buf[1], ack_buf[2], ack_buf[3], ack_buf[4], ack_buf[5]);
		uart_flush_output();
		return FALSE;
	}

	return TRUE;
}

gboolean ubx_read_next_msg(ubx_msg_t *msg, const ubx_msg_type_t *expected_type)
{
	static U1 buf[1024];

	if (! ubx_read_header(buf, 4))
		return FALSE;

	msg->class = buf[0];
	msg->id = buf[1];

	/* LITTLE ENDIAN ALWAYS */
	msg->payload_len = buf[2] | (buf[3] << 8);

	/* payload length + checksum(2) */
	if (! read_fixed_len(buf, msg->payload_len + 2)) {
		log_warn("read payload failed.");
		return FALSE;
	}

	msg->payload_addr = buf;
	msg->checksum[0] = buf[msg->payload_len];
	msg->checksum[1] = buf[msg->payload_len + 1];

	/* Previous operations may leave garbage in the out buffer. */
	if (expected_type != NULL && (msg->class != expected_type->class || msg->id != expected_type->id)) {
		log_warn("bad message type: expected {0x%02x, 0x%02x}, but get {0x%02x, 0x%02x}",
			expected_type->class, expected_type->id, msg->class, msg->id);
		uart_flush_output();
		return FALSE;
	}

	return TRUE;
}

/**
 * Send a command.
 * The response content is ignored
 */
gboolean ubx_issue_cmd(U1 *packet, int len)
{
	ubx_checksum(&packet[2], len - 4);
	if (! ubx_send_request(packet, len)) {
		log_warn("write device failed");
		return FALSE;
	}
	return TRUE;
}

/**
 * Enable or disable all standard NMEA messages
 * send_reate: 0x00 means disabled.
 *
 * NOTE: on FR GTA02 v6, the I/O target numbers are 1. (0, 1, 2, 3)
 * Although u-blox firmware 5.00 doc says 6 ports, but ANTARIS 0635 has only 4.
 */
gboolean ubx_cfg_msg_nmea_std(U1 send_rate, gboolean all, gboolean readack)
{
	/* class=0xf0, id={0x00 - 0x0a} */
	U1 packet[8+66] = {
		0xB5, 0x62,
		type_cfg_msg.class, type_cfg_msg.id,
		66, 0x00
	};

	int i, off;
	U1 val;
	U1 ids[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a};
	gboolean enable[] = {TRUE, FALSE, FALSE, FALSE, FALSE, TRUE, FALSE, FALSE, FALSE, FALSE, FALSE};

	for (i=0; i<11; i++) {
		off = 6 + i * 6;
		val = all? send_rate : (enable[i]? send_rate : 0x00);
		packet[off] = 0xF0; /* NMEA msg class*/
		packet[off+1] = ids[i];
		packet[off+2] = 0;	/*target 0*/
		packet[off+3] = val;/*target 1*/
		packet[off+4] = 0;	/*target 2*/
		packet[off+5] = 0;	/*target 3*/
	}

	return ubx_issue_cmd(packet, sizeof(packet)) && (! readack || ubx_read_ack(&type_cfg_msg));
}

void ubx_mon_ver_poll(char *buf, int buf_len)
{
	U1 packet[] = {
		0xB5, 0x62,
		type_mon_ver.class, type_mon_ver.id,
		0x00, 0x00,
		0x00, 0x00
	};

	ubx_msg_t msg;

	if (! (ubx_issue_cmd(packet, sizeof(packet)) &&
		ubx_read_next_msg(&msg, &type_mon_ver))) {
		log_warn("MON-VER: poll failed.\n");
		return;
	}

	snprintf(buf, buf_len, "software=%s; hardware=%s", msg.payload_addr, &msg.payload_addr[30]);

	/* no extension at all, ignore */
}

/**
 * NOTE: U-BLOX5 chip use CFG-NAV5 instead of CFG-NAV2.
 * model:
 * 1 Stationary
 * 2 Pedestrian
 * 3 Automotive
 * 4 Sea
 * 5 Airborne with <1g Acceleration
 * 6 Airborne with <2g Acceleration
 * 7 Airborne with <4g Acceleration
 * fix_mode:
 * 1: 2D only
 * 2: Auto 2D/3D
 * 3: 3D only
 *
 * min_ELE: Minimum Elevation for a GNSS satellite to be used in NAV, degree, default 5
 * maxsv: Maximum number of GNSS satellites for Navigation, default 16
 */
gboolean ubx_cfg_nav2(U1 model, gboolean readack)
{
	U1 maxsv = 0x10;
	U1 fix_mode = 0x02; /* auto */
	U1 min_ELE = 0x05;
	U1 allow_alma_nav = 0X0;

	U1 packet[8+40] = {
		0xB5, 0x62,
		type_cfg_nav2.class, type_cfg_nav2.id,
		40, 0x00,
		model,
		0x00,
		0x00, 0x00,
		0x03,
		0x03,
		maxsv,
		fix_mode,
		0x50, 0xC3, 0x00, 0x00,
		0x0F,
		0x0A,
		min_ELE,
		0x3C,
		0x0F, /* default 0 */
		allow_alma_nav,
		0x00, 0x00,
		0xFA, 0x00,
		0xFA, 0x00,
		0x64, 0x00,
		0x2C, 0x01,
		0x00,
		0x00,
		0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00
	};

	return ubx_issue_cmd(packet, sizeof(packet)) && (! readack || ubx_read_ack(&type_cfg_nav2));
}

/**
 * @rate: the bigger the slower. 0 : disable
 */
gboolean ubx_cfg_msg_nmea_ubx(U1 send_rate, gboolean all, gboolean readack)
{
	/* class=0xf1, id={0x00, 0x01, 0x03, 0x04} */
	U1 packet[8+4*6] = {
		0xB5, 0x62,
		type_cfg_msg.class, type_cfg_msg.id,
		24, 0x00
	};

	U1 ids[] = {0x00, 0x01, 0x03, 0x04};
	gboolean enable[] = {TRUE, FALSE, FALSE, FALSE};
	int i, off;

	U1 val;
	for (i=0; i<4; i++) {
		off = 6 + i * 6;
		val = all? send_rate : (enable[i]? send_rate : 0x00);
		packet[off] = 0xF1;
		packet[off+1] = ids[i];
		packet[off+2] = 0;	/*target 0*/
		packet[off+3] = val;/*target 1*/
		packet[off+4] = 0;	/*target 2*/
		packet[off+5] = 0;	/*target 3*/
	}

	return ubx_issue_cmd(packet, sizeof(packet)) && (! readack || ubx_read_ack(&type_cfg_msg));
}

gboolean ubx_cfg_msg(const ubx_msg_type_t *msg_type, gboolean enabled, gboolean readack)
{
	U1 packet[11] = {
		0xB5, 0x62,
		type_cfg_msg.class, type_cfg_msg.id,
		0x03, 0x00 /* length */
	};

	packet[6] = msg_type->class;
	packet[7] = msg_type->id;
	packet[8] = enabled? 0x01 : 0x00;

	return ubx_issue_cmd(packet, sizeof(packet)) && (! readack || ubx_read_ack(&type_cfg_msg));
}

/**
 * meas: ms, multiple of 250 ms.
 */
gboolean ubx_cfg_rate(U2 meas, gboolean readack)
{
	U1 packet[8+6] = {
		0xB5, 0x62,
		type_cfg_rate.class, type_cfg_rate.id,
		0x06, 0x00, /* len */
		0x00, 0x00, /* meas */
		0x01, 0x00, /* nav */
		0x00, 0x00, /* time: aligned to GPS time */
		0x00, 0x00  /* checksum */
	};

	packet[6] = (U1)(meas & 0xFF);
	packet[7] = (U1)((meas >> 8) & 0xFF);

	return ubx_issue_cmd(packet, sizeof(packet)) && (!readack || ubx_read_ack(&type_cfg_rate));
}

/**
 * By default SBAS is enabled with three prioritized SBAS channels and it will use
 * any received SBAS satellites (except for those in test mode) for navigation,
 * ionosphere parameters and corrections.
 *
 * SBAS Usage (Bitmask):
 * Bit 0: Use SBAS GEOs as a ranging source (for navigation)
 * Bit 1: Use SBAS Differential Corrections
 * Bit 2: Use SBAS Integrity Information
 */
gboolean ubx_cfg_sbas(gboolean enable, gboolean readack)
{
	/* FIXME: make these configurable? */
	U1 usage = 0x07;
	U1 max_channels_searched = 0x03;

	U1 packet[8+8] = {
		0xB5, 0x62,
		type_cfg_sbas.class, type_cfg_sbas.id,
		0x08, 0x00, /* length */
		enable? 0x01:0x00, /* mode */
		usage, /* usage */
		max_channels_searched, /* maxsbas: 0-3 */
		0x00, /* reserved */
		0x00, 0x00, 0x00, 0x00, /* scanmode: all zeros->auto */
		0X00, 0X00 /* checksum */
	};

	return ubx_issue_cmd(packet, sizeof(packet)) && (! readack || ubx_read_ack(&type_cfg_sbas));
}

/**
 * GPS Sensitivity Mode:
 * 0: Normal
 * 1: Fast Acquisition
 * 2: High Sensitivity
 * 3: Auto
 *
 * Low Power Mode:
 * 0: Continuous Tracking Mode
 * 1: Fix Now
 */
gboolean ubx_cfg_rxm(U1 gps_mode, U1 lp_mode, gboolean readack)
{
	U1 packet[] = {
		0xB5, 0x62,
		type_cfg_rxm.class, type_cfg_rxm.id,
		0x02, 0x00,
		gps_mode, lp_mode,
		0x00, 0x00
	};

	return ubx_issue_cmd(packet, sizeof(packet)) && (! readack || ubx_read_ack(&type_cfg_rxm));
}

/**
 * BBR Sections to clear:
 *
 * 0x0000 Hotstart
 * 0x0001 Warmstart
 * 0xFFFF Coldstart
 *
 * Reset Type:
 * 0x00 - Hardware Reset (Watchdog)
 * 0x01 - Controlled Software reset
      terminates all running processes in an orderly manner and, once the system is
 *    idle, restarts operation, reloads its configuration and starts to acquire and track GPS satellites
 * 0x02 - Controlled Software reset (GPS only)
 *    only restarts the GPS tasks, without reinitializing the full system or
 *    reloading any stored configuration.
 * 0x08 - Controlled GPS stop
 * 0x09 - Controlled GPS start
 */
gboolean ubx_cfg_rst(U2 bbr, U1 reset_type)
{
	U1 packet[] = {
		0xB5, 0x62,
		type_cfg_rst.class, type_cfg_rst.id,
		0x04, 0x00, /* len */
		bbr & 0xFF, (bbr >> 8) & 0xFF, /* bbr */
		reset_type & 0xFF, /* reset type */
		0x00, /* reserved */
		0x00, 0x00
	};

	/* no output available, so skip reading ACK */
	return ubx_issue_cmd(packet, sizeof(packet));
}

gboolean ubx_reset_gps(char *type)
{
	U2 bbr;
	if (strcmp(type, "hot") == 0)
		bbr = 0x0000;
	else if (strcmp(type, "warm") == 0)
		bbr = 0x0001;
	else if (strcmp(type, "cold") == 0)
		bbr = 0xFFFF;
	else
		return FALSE; // should not happen

	return ubx_cfg_rst(bbr, 0x02);
}

/**
 * CFG-PRT (0x06 0x00)
 * 0x0001: UBX Protocol
 * 0x0002: NMEA Protocol
 */
gboolean ubx_cfg_prt(U1 port_id, U1 in_protocol, U1 out_protocol, U4 baud, gboolean readack)
{
	U1 packet[] = {
		0xB5, 0x62,
		type_cfg_prt.class, type_cfg_prt.id,
		20, 0, /* length */
		port_id, /* port ID */
		0x00, 0x00, 0x00, /* reserved */
		0xd0, 0x08, 0x08, 0x00, /* mode */
		0x80, 0x25, 0x00, 0x00, /* baud rate */
		0x07, 0x00, /* in protocol */
		0x03, 0x00, /* out protocol */
		0x00, 0x00, /* flags */
		0x00, 0x00, /* pad */
		0x00, 0x00 /* checksum */
	};

	/* Disable this port totally */
	if (in_protocol == 0x00 && out_protocol == 0x00 && baud == 0x00) {
		packet[10] = packet[11] = packet[12] = packet[13] = 0x00;
	}

	packet[6+8] = baud & 0xFF;
	packet[6+9] = (baud >> 8) & 0xFF;
	packet[6+10] = (baud >> 16) & 0xFF;
	packet[6+11] = (baud >> 24) & 0xFF;
	packet[6+12] = in_protocol;
	packet[6+14] = out_protocol;

	return ubx_issue_cmd(packet, sizeof(packet)) && (! readack|| ubx_read_ack(&type_cfg_prt));
}

static gboolean ubx_parse_nav_svinfo(ubx_msg_t *msg)
{
	U1 *buf = msg->payload_addr;

	g_gpsdata.sv_channel_count = (U1)READ_U1(buf+4);
	if (g_gpsdata.sv_channel_count > SV_MAX_CHANNELS)
		g_gpsdata.sv_channel_count = SV_MAX_CHANNELS;

	if (msg->payload_len != 8 + g_gpsdata.sv_channel_count * 12) {
		log_warn("svinfo: bad length\n");
		g_gpsdata.svinfo_valid = FALSE;
		return FALSE;
	}

	int i, j = 0;
	U1 flags, sv_id, *p;
	svinfo_channel_t *sv;

	g_gpsdata.sv_in_use = 0;
	g_gpsdata.sv_get_signal = 0;

	/* default elevation is -9, default azimuth is 0 */

	for (i=0; i < g_gpsdata.sv_channel_count; i++) {
		p = (U1 *)(buf + 8 + i * 12);

		/* CHN: 255 -- SVs not assigned to a channel */
		if (READ_U1(p) == 0xFF)
			continue;

		/* Valid SV ID: 1..32, or DGPS IDS (>100) */
		sv_id = READ_U1(p+1);
		if (sv_id == 0x0)
			continue;

		flags = READ_U1(p+2);
		/* unhealthy */
		if (flags & 0x10)
			continue;

		sv = &g_gpsdata.sv_channels[j];
		sv->sv_id = sv_id;
		sv->flags = flags;
		sv->cno = READ_U1(p+4);
		sv->elevation = READ_S1(p+5);
		sv->azimuth = READ_S2(p+6);
		if (sv->elevation < 0 || sv->azimuth < 0)
			sv->elevation = sv->azimuth = -9;

		if ((flags & 0x01) == 0x01)
			++g_gpsdata.sv_in_use;

		if (sv->cno > (U1)0)
			++g_gpsdata.sv_get_signal;

		j++;
	}

	g_gpsdata.sv_channel_count = j;
	g_gpsdata.svinfo_valid = TRUE;
	return TRUE;
}

/**
 * For dump AID-EPH, AID-HUI, AID-ALM.
 * Make sure the <raw> char array can holds max length of data ot <type>
 *
 * From http://www.u-blox.cn/customersupport/gps.g5/ublox5_Fw5.00_Release_Notes(GPS.G5-SW-08019).pdf:
 *
 * UBX-AID-EPH and UBX-AID-ALM Messages for Satellite without valid Orbits
 * When polling UBX-AID-EPH or UBX-AID-ALM messages, satellites without valid ephemeris or almanac data will
 * return a complete UBX-AID-EPH or UBX-AID-ALM message with all data words set to zero. This doesn’t comply
 * with the protocol specification. Furthermore, u-blox 5 receivers with firmware V5.00 and
 * earlier can run into a floating-point exception when fed with such “empty” ephemeris.
 */
int ubx_read_next_aid_message(U1 raw[], int len, const ubx_msg_type_t *type, int valid_payload_len)
{
	ubx_msg_t msg;
	if (! ubx_read_next_msg(&msg, type)) {
		log_warn("read response failed: class=%02x, id=%02x", type->class, type->id);
		return -1;
	}

	if (len < msg.payload_len + 8)
		return -2;

	if (msg.payload_len != valid_payload_len)
		return 0;

	gboolean hit_bug = TRUE;
	int i;

	for (i=4; i<msg.payload_len; i++) { /* skip SV ID */
		if (msg.payload_addr[i] != 0x00) {
			hit_bug = FALSE;
			break;
		}
	}
	if (hit_bug)
		return 0;

	raw[0] = 0xB5;
	raw[1] = 0x62;
	raw[2] = type->class;
	raw[3] = type->id;
	raw[4] = msg.payload_len & 0xFF;
	raw[5] = (msg.payload_len >> 8) & 0xFF;

	memcpy(&raw[6], msg.payload_addr, msg.payload_len);

	raw[6 + msg.payload_len] = msg.checksum[0];
	raw[7 + msg.payload_len] = msg.checksum[1];

	return 8 + msg.payload_len;
}

static int enabled_type_count = 8;
static U1 poll_packet[8 * 8];

/**
 * SVINFO must be last!
 */
void ubx_set_poll_msg_types(ubx_msg_type_t types[], int count)
{
	enabled_type_count = count;

	int i, offset;

	for (i = 0; i<count; i++) {
		offset = i * 8;
		poll_packet[offset+0] = 0xB5;
		poll_packet[offset+1] = 0x62;
		poll_packet[offset+2] = types[i].class;
		poll_packet[offset+3] = types[i].id;
		poll_packet[offset+4] = 0x00;
		poll_packet[offset+5] = 0x00;

		ubx_checksum(&poll_packet[offset+2], 4);
	}
}

/*
 * _gpsdata is set as non-NULL on first call ubx_init();
 */
void ubx_init(int dev_fd)
{
	gps_dev_fd = dev_fd;

	init_gpsdata(&g_gpsdata);
}

static inline gboolean ubx_parse_nav_status(ubx_msg_t *msg)
{
	U1 *buf = msg->payload_addr;

	if (msg->payload_len != 16) {
		log_warn("nav status: bad length");
		return FALSE;
	}

	g_gpsdata.nav_status_itow = (U4)READ_U4(buf);
	g_gpsdata.nav_status_fixtype = (U1)READ_U1(buf+4);
	//g_gpsdata.nav_status_flags = ((U1)READ_U1(buf+5)) & 0xF;
	//g_gpsdata.nav_status_diffs = ((U1)READ_U1(buf+6)) & 0x4;
	//g_gpsdata.nav_status_ttff = (U4)READ_U4(buf+8);
	//g_gpsdata.nav_status_msss = (U4)READ_U4(buf+12);

	return TRUE;
}

static inline gboolean ubx_parse_nav_posllh(ubx_msg_t *msg)
{
	/* 1e7 */
	#define POS_SCALE	10000000
	/* mm -> m */
	#define ACC_SCALE	1000
	/* mm: hack! */
	#define VALID_HACC	1000000

	U1 *buf = msg->payload_addr;

	if (msg->payload_len != 28) {
		log_warn("nav posllh: bad length");
		return FALSE;
	}

	U4 hacc = (U4)READ_U4(buf+20);
	if (hacc > VALID_HACC) {
		g_gpsdata.latlon_valid = FALSE;
		g_gpsdata.height_valid = FALSE;
		return TRUE;
	}

	g_gpsdata.llh_itow = (U4)READ_U4(buf);
	g_gpsdata.hacc = (float)hacc / ACC_SCALE;
	g_gpsdata.vacc = (float)(U4)READ_U4(buf+24) / ACC_SCALE;
	g_gpsdata.lon = (double)READ_S4(buf+4) / POS_SCALE;
	g_gpsdata.lat = (double)READ_S4(buf+8) / POS_SCALE;
	g_gpsdata.height = (float)READ_S4(buf+12) / ACC_SCALE;

	g_gpsdata.latlon_valid = TRUE;
	g_gpsdata.height_valid = TRUE;

	return TRUE;
}

static inline gboolean ubx_parse_nav_velned(ubx_msg_t *msg)
{
	/* 1e5 */
	#define HEADING_SCALE	100000
	/* cm -> m */
	#define VEL_SCALE		100
	/* cm/s, hack! */
	#define VALID_SPEED_ACC	500

	U1 *buf = msg->payload_addr;

	if (msg->payload_len != 36) {
		log_warn("nav velned: bad length");
		return FALSE;
	}

	U4 acc = (U4)READ_U4(buf+28);
	if (acc > VALID_SPEED_ACC) {
		g_gpsdata.vel_valid = FALSE;
		return TRUE;
	}

	g_gpsdata.speed_2d = (float)READ_U4(buf+20) / VEL_SCALE;
	g_gpsdata.vel_down = (float)READ_S4(buf+12) / VEL_SCALE;
	g_gpsdata.heading_2d = (float)READ_S4(buf+24) / HEADING_SCALE;

	g_gpsdata.vel_valid = TRUE;

	return TRUE;
}

static gboolean ubx_parse_nav_timeutc(ubx_msg_t *msg)
{
	U1 *buf = msg->payload_addr;
	/**
	 * 0x01 = Valid Time of Week
	 * 0x02 = Valid Week Number
	 * 0x04 = Valid UTC (Leap Seconds already known?)
	 */
	U1 valid = (U1)READ_U1(buf+19);

	if (valid != 0x07) {
		g_gpsdata.time_valid = FALSE;
		return TRUE;
	}

	g_gpsdata.time.tm_year = (U2)READ_U2(buf+12) - 1900;
	g_gpsdata.time.tm_mon = (U1)READ_U1(buf+14) - 1;
	g_gpsdata.time.tm_mday = (U1)READ_U1(buf+15);
	g_gpsdata.time.tm_hour = (U1)READ_U1(buf+16);
	g_gpsdata.time.tm_min = (U1)READ_U1(buf+17);
	g_gpsdata.time.tm_sec = (U1)READ_U1(buf+18);
	g_gpsdata.time.tm_zone = "UTC";

	g_gpsdata.time_valid = TRUE;

	if (! g_context.time_synced)
		sync_gpstime_to_system(&g_gpsdata.time);

	return TRUE;
}

static gboolean ubx_read_and_parse_response(ubx_msg_t *msg, ubx_msg_type_t *type)
{
	if (! ubx_read_next_msg(msg, type)) {
		if (type != NULL)
			log_warn("read response failed: class=%02x, id=%02x", type->class, type->id);
		else
			log_warn("read response failed");
		return FALSE;
	}

	if (msg->class == UBX_CLASS_NAV) {
		if (msg->id == UBX_ID_NAV_STATUS)
			return ubx_parse_nav_status(msg);
		else if (msg->id == UBX_ID_NAV_POSLLH)
			return ubx_parse_nav_posllh(msg);
		else if (msg->id == UBX_ID_NAV_VELNED)
			return ubx_parse_nav_velned(msg);
		else if (msg->id == UBX_ID_NAV_TIMEUTC)
			return ubx_parse_nav_timeutc(msg);
		else if (msg->id == UBX_ID_NAV_SVINFO)
			return ubx_parse_nav_svinfo(msg);
	}

	return FALSE;
}

/**
 * Poll a group of messages.
 * Save result to msg. The class and id of a msg must be initialized.
 * Return:
 * 		TRUE: ALL ok, may be invalid
 * 		FALSE: part or all failed: I/O error
 */
gboolean ubx_poll_group(gboolean pollsv)
{
	ubx_msg_t msg;
	int i;
	int count = (pollsv)? enabled_type_count : enabled_type_count - 1;
	int size = count << 3;
	int written = 0;

	for (; size > 0; size-= written) {
		if ((written = write_with_timeout(&poll_packet[written], size)) <= 0) {
			log_warn("UBX poll: write device failed");
			return FALSE;
		}
	}
	tcdrain(gps_dev_fd);

	//sleep_ms(SEND_RATE);

	gboolean has_failure = FALSE;
	for (i=0; i<count; i++) {
		if (! ubx_read_and_parse_response(&msg, NULL)) {
			has_failure = TRUE;
			break;
		}
	}

	return ! has_failure;
}
