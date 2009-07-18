#ifndef DBUS_INTF_H_
#define DBUS_INTF_H_

#include <pthread.h>
#include <glib.h>
#include <dbus/dbus-glib.h>

#include "gps.h"

#define DBUS        "org.freedesktop.DBus"
#define DBUS_PATH	"/org/freedesktop/DBus"

#define DEVICE      "org.freesmartphone.Device"
#define DEVICE_PATH "/org/freesmartphone/Device"
#define ODEVICED    "org.freesmartphone.odeviced"

#define USAGE		"org.freesmartphone.Usage"
#define USAGE_PATH	"/org/freesmartphone/Usage"
#define OUSAGED		"org.freesmartphone.ousaged"

/* ms */
#define PROXY_CALL_TIMEOUT 5000

extern gboolean dbus_init();
extern void dbus_cleanup();

extern gboolean dbus_power_is_low();

extern gboolean dbus_request_resource(const char *name);
extern gboolean dbus_release_resource(const char *name);

extern gboolean fso_gypsy_init(gps_data_t *_gpsdata, pthread_mutex_t *_lock);
extern void fso_gypsy_cleanup();
extern gboolean fso_gypsy_poll(gboolean pollsv);
extern gboolean fso_gypsy_is_running();

#endif /* DBUS_INTF_H_ */
