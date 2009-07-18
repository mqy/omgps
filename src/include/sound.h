#ifndef SOUND_H_
#define SOUND_H_

#include "Python.h"

/* NOTE: the directory is hard-coded, package "omgps-sound" must respect this */
#define OMGPS_SOUND_TOP_DIR		"/usr/share/sounds/omgps"

typedef enum
{
	SND_EVT_GPS_SIGNAL_WEAK = 1,
	SND_EVT_LOW_POWER       = 2
} snd_evt_type_t;

typedef void (*snd_speed_func_t)(char *value);
typedef void (*snd_evt_func_t)(snd_evt_type_t evt_type);

typedef struct
{
	gboolean initialized;
	char *parent_dir;
	PyObject *module;
	PyObject *play_speed_func;
	PyObject *play_event_func;
} snd_provider_t;

extern void sound_init();
extern void sound_cleanup();
extern gboolean toggle_sound(gboolean enable);

extern gboolean set_plugin(char *moudle);

/* built-in providers */
extern snd_provider_t * snd_provider_en_us_init();
extern snd_provider_t * snd_provider_zh_cn_init();

#endif /* SOUND_H_ */
