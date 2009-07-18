#ifndef DEVICE_H_
#define DEVICE_H_

#include <glib.h>
#include <gps.h>

extern gboolean check_device_files();

extern int uart_open(U4 baud_rate, gboolean verify_output);
extern gboolean uart_init();
extern void uart_flush_output();
extern void uart_cleanup();
extern void uart_close();

extern inline int read_with_timeout(U1 *buf, int len);
extern inline int write_with_timeout(U1 *buf, int len);
extern inline gboolean read_fixed_len(U1 *buf, int expected_len);

extern int sysfs_get_gps_device_power();
extern gboolean gps_device_power_on();

#endif /* DEVICE_H_ */
