#ifndef GPS_H_
#define GPS_H_

#include <glib.h>

#include "wgs84.h"

typedef unsigned char		U1;
typedef unsigned short		U2;
typedef unsigned int		U4;
typedef unsigned long long	U8;

/* ANTARIS 4 / u-blox 5 specs says 1..16,  */
#define SV_MAX_CHANNELS				16

/* Our initial configuration: port 0x01 */
#define SEND_RATE					1000

#define BAUD_RATE					9600

/* the ephemeris data usually is out-dated after 2-4 hours */
#define EPH_VALID_HOURS				3

#define HUI_VALID_HOURS				4

/* up to months */
#define ALM_VALID_DAYS				14

#define DUMP_AID_INTERVAL_SECONDS	1800

/* default PACC for CFG-AID-INI, kilometers. Hack! */
#define AID_INI_PACC_KM				300

#define AGPS_SERVER					"agps.u-blox.com"
#define AGPS_PORT					"46434"

 /* hack.
  * @see: http://users.erols.com/dlwilson/gpsacc.htm
  * @see: http://users.erols.com/dlwilson/gpshdop.htm */
#define DOP_TO_HACC					3

#define MPS_TO_KMPH					3.6
#define MPS_TO_MPH					2.237

/**
 * non-suspending does not mean running, maybe in starting state.
 */
typedef enum __poll_state_t
{
	POLL_STATE_SUSPENDING,
	POLL_STATE_STARTING,
	POLL_STATE_RUNNING,
} poll_state_t;

typedef enum __poll_engine_t
{
	POLL_ENGINE_UBX,
	POLL_ENGINE_OGPSD
} poll_engine_t;

typedef enum __speed_unit_t
{
	SPEED_UNIT_KMPH,
	SPEED_UNIT_MPS,
	SPEED_UNIT_MPH
} speed_unit_t;

/**
 * A satellite channel
 */
typedef struct __svinfo_channel_t
{
	/* SVID, i.e., PRN
	 * GPS: 1-32
	 * SBAS: 120, 122, 124, 126, 127, 129, 131, 134, 135, 137, 138 */
	U4 sv_id;

	/**
	 * 1 = SV is used for navigation
	 * 2 = Differential correction data is available for this SV
	 * 4 = Orbit information is available for this SV (Ephemeris or Almanach)
	 * 8 = Orbit information is Ephemeris
	 * 16 = SV is unhealthy / shall not be used
	 * 32 = Orbit information is Almanac Plus
	 */
	U4 flags;

	/**
	 * Quality indicator.
	 * 0: This channel is idle
	 * 1,2: Channel is searching
	 * 3: Signal detected but unusable
	 * 4: Code Lock on Signal
	 * 5,6: Code and Carrier locked
	 * 7: Code and Carrier locked, receiving 50bps data
	 */
	//char qi;

	/* Carrier to Noise Ratio (Signal Strength), 0..99, dbHZ*/
	U4 cno;

	/* actual value: [0..90], < 0 means invalid.
	 * char is enough, align to 4-bytes boundary */
	int elevation;

	/* actual value: [0..359], < 0 means invalid.
	 * short is enough, align to 4-bytes boundary */
	int azimuth;

} svinfo_channel_t;


typedef struct __gps_data_t
{
	gboolean time_valid;

	struct tm time;

	/* NMEA GPGGL or NAV-POSLLH (0x01 0x02): Geodetic Position Solution */

	gboolean latlon_valid;

	U4 llh_itow;

	/* Latitude, degrees, absolute value */
	double lat;

	/* Longitude, degrees, absolute value */
	double lon;

	/* Height above Ellipsoid, m */
	float height;

	/* Horizontal Accuracy Estimate, m,
	 * also HDOP when use ogpsd  -- need be converted to meters*/
	float hacc;

	/* Vertical Accuracy Estimate, m,
	 * also VDOP when use ogpsd -- need be converted to meters */
	float vacc;

	/** NMEA GPVTG or NAV-VELNED (0x01 0x12): Velocity Solution in NED **/

	gboolean height_valid;

	gboolean vel_valid;

	/* 3D speed, UBX only */
	//float speed_3d;

	/* ground speed (2-D) m/s */
	float speed_2d;

	/* Heading 2-D, degree */
	float heading_2d;

	//U4 vel_itow;

	/* north velocity, UBX ONLY */
	//float vel_north;

	/* east velocity, UBX ONLY */
	//float vel_east;

	/* down velocity, cm/s ->m/s */
	float vel_down;

	/* speed accuracy estimate, m/s, UBX only */
	//float speed_acc;

	/* course / heading accuracy estimate, degree. UBX only */
	//float heading_acc;

	/** NMEA GPGSV or NAV-SVINFO (0x01 0x30): Velocity Solution in NED **/

	gboolean svinfo_valid;

	U1 sv_channel_count;

	U1 sv_in_use;

	U1 sv_get_signal;

	svinfo_channel_t sv_channels[SV_MAX_CHANNELS];

	/** NAV STATUS **/

	U4 nav_status_itow;

	/* GPSfix Type, range 0..3
	 * 0x00 = no fix
	 * 0x01 = dead reckoning only
	 * 0x02 = 2D-fix
	 * 0x03 = 3D-fix
	 * 0x04 = GPS + dead reckoning combined
	 * 0x05 = Time only fix0x
	 * 06..0xff = reserved */
	U1 nav_status_fixtype;

	/* Navigation Status Flags
	 * 0x01 = GPSfixOK (i.e. within DOP and ACC Masks)
	 * 0x02 = DiffSoln (is DGPS used)
	 * 0x04 = WKNSET (is Week Number valid)
	 * 0x08 = TOWSET (is Time of Week valid)
	 * 0x?0 = reserved */
	//U1 nav_status_flags;

	/* Bits [1:0] - DGPS Input Status
	 * 00: none
	 * 01: PR+PRR Correction
	 * 10: PR+PRR+CP Correction
	 * 11: High accuracy PR+PRR+CP Correction */
	//U1 nav_status_diffs;

	/* Time to first fix (millisecond time tag) */
	//U4 nav_status_ttff;

	/* Milliseconds since Startup / Reset */
	//U4 nav_status_msss;

} gps_data_t;

typedef gboolean (*ctrl_cmd_func_t)(void* args);

#define POLL_STATE_TEST(s) (g_context.poll_state == POLL_STATE_##s)
#define POLL_STATE_SET(s) g_context.poll_state = POLL_STATE_##s
#define POLL_ENGINE_TEST(e) (g_context.poll_engine == POLL_ENGINE_##e)

extern gboolean agps_dump_aid_data(gboolean agps_online);
extern void set_initial_aid_data();

extern void map_redraw_view_gps_running();
extern void init_gpsdata();
extern gboolean start_poll_thread();
extern void stop_poll_thread();
extern void notify_poll_thread_suspend_resume();
extern gboolean issue_ctrl_cmd(ctrl_cmd_func_t f, void *args);
extern void sync_gpstime_to_system();

extern void poll_update_ui();
extern void poll_ui_on_speed_unit_changed();

extern void ctx_gpsfix_on_poll_engine_changed();
extern void ctx_gpsfix_on_poll_state_changed();
extern void ctx_gpsfix_on_track_state_changed();

extern void update_tab_on_poll_state_changed();
extern void poll_ui_on_view_range_changed();

#endif /* GPS_H_ */
