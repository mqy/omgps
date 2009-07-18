#ifndef UBX_H_
#define UBX_H_

#include <glib.h>

#include "gps.h"

/* class and id of UBX binary messages */

#define UBX_CLASS_NAV 		0x01
#define UBX_CLASS_RXM 		0x02
#define UBX_CLASS_ACK		0x05
#define UBX_CLASS_CFG 		0x06
#define UBX_CLASS_MON 		0x0A
#define UBX_CLASS_AID 		0x0B

#define UBX_ID_NAV_POSLLH	0x02
#define UBX_ID_NAV_STATUS	0x03
#define UBX_ID_NAV_VELNED	0X12
#define UBX_ID_NAV_TIMEUTC	0x21
#define UBX_ID_NAV_SVINFO	0x30

#define UBX_ID_ACK_NAK		0x00
#define UBX_ID_ACK_ACK		0x01

#define UBX_ID_CFG_PRT		0x00
#define UBX_ID_CFG_MSG		0x01
#define UBX_ID_CFG_RST		0x04
#define UBX_ID_CFG_RATE		0x08
#define UBX_ID_CFG_SBAS		0x16
#define UBX_ID_CFG_RXM		0x11
#define UBX_ID_CFG_NAV2		0x1A

#define UBX_ID_AID_INI		0x01
#define UBX_ID_AID_HUI		0x02
#define UBX_ID_AID_ALM		0x30
#define UBX_ID_AID_EPH		0x31

#define UBX_ID_MON_VER		0x04

typedef struct __ubx_msg_type_t
{
	U1 class;
	U1 id;
} ubx_msg_type_t;

typedef struct __ubx_msg_t
{
	U1 *payload_addr;
	U2 payload_len;
	U1 class;
	U1 id;
	U1 checksum[2];

} ubx_msg_t;

extern void 	ubx_mon_ver_poll();

extern void		ubx_checksum(U1 *packet, int size);
extern gboolean ubx_issue_cmd(U1 *packet, int len);
extern gboolean ubx_read_next_msg(ubx_msg_t *msg, const ubx_msg_type_t *expected_type);
extern gboolean ubx_send_request(U1 *buf, int size);

extern gboolean ubx_cfg_prt(U1 port_id, U1 in_protocol, U1 out_protocol, U4 baud_rate, gboolean readack);
extern gboolean ubx_cfg_rate(U2 update_rate, gboolean readack);
extern gboolean ubx_cfg_msg_nmea_ubx(U1 send_rate, gboolean all, gboolean readack);
extern gboolean ubx_cfg_msg_nmea_std(U1 send_rate, gboolean all, gboolean readack);
extern gboolean ubx_cfg_msg(const ubx_msg_type_t *type, gboolean enabled, gboolean readack);
extern gboolean ubx_cfg_rxm(U1 gps_mode, U1 lp_mode, gboolean readack);
extern gboolean ubx_cfg_sbas(gboolean enable, gboolean readack);
extern gboolean ubx_cfg_rst(U2 bbr, U1 reset_type);
extern gboolean ubx_cfg_nav2(U1 model, gboolean readack);

extern gboolean ubx_reset_gps(char *type);
extern gboolean ubx_read_ack(const ubx_msg_type_t *expected_type);
extern int		ubx_read_next_aid_message(U1 raw[], int len, const ubx_msg_type_t *type, int valid_payload_len);

/* read fields from UBX message, the built-in endian is little endian */

#define READ_U1(p)    (*(p))
#define READ_U2(p)    ((*(p+1)<<8)|(*(p)))
#define READ_U4(p)    ((*(p+3)<<24)|(*(p+2)<<16)|(*(p+1)<<8)|(*(p)))
#define READ_S1(p)    (signed char)READ_U1(p)
#define READ_S2(p)    (signed short)READ_U2(p)
#define READ_S4(p)    (signed int)READ_U4(p)

#define READ_FLOAT(p) \
	(float)(((u4)(*(p+3))<<24)|((u4)(*(p+2))<<16)|((u4)(*(p+1))<<8)|((u4)(*(p+0)))

#define READ_DOUBLE(p) \
	(double)(((u8)(*(p+7))<<56)|((u8)(*(p+6))<<48)|((u8)(*(p+5))<<40)|((u8)(*(p+4))<<32)| \
	((u8)(*(p+3))<<24)|((u8)(*(p+2))<<16)|((u8)(*(p+1))<<8)|((u8)(*(p+0)))

extern void ubx_init(int dev_fd);
extern void ubx_set_poll_msg_types(ubx_msg_type_t types[], int count);
extern gboolean ubx_poll_group(gboolean pollsv);

/* Global data */
extern gps_data_t g_gpsdata;

#endif /*UBX_H_*/
