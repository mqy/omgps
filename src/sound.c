#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <signal.h>

#include "dbus_intf.h"
#include "sound.h"
#include "util.h"
#include "omgps.h"
#include "poll.h"
#include "py_ext.h"
#include "customized.h"

#define FUNC_INIT		"init"
#define FUNC_PLAY_SPEED	"play_speed"
#define FUNC_PLAY_EVENT	"play_event"

static gboolean running = FALSE;
static pthread_t tid = 0;

#define NOTIFY_INTERVAL_SPEED	15
#define NOTIFY_INTERVAL_EVENT	180

static time_t last_time_speed = 0;
static time_t last_time_signal_weak = 0;
static time_t last_time_low_power = 0;

static snd_provider_t snd_provider;

#define WAIT_A_WHILE(t_ms) do {			\
	sleep_ms(t_ms);						\
	if (! running)						\
		break;							\
} while (0)

/**
 * ';' separated, at most 20 files
 */
static gboolean play_sound_files(char *files)
{
	/* NOTE: empty input blocks aplay */
	files = trim(files);
	if (! files || strcmp(files, "") == 0)
		return FALSE;

	char *bak = strdup(files);
	char *saveptr;
	char *p = strtok_r(bak, ";", &saveptr);

	#define MAX_SOUND_FILES 14
	#define APLAY_START_IDX 4
	char *aplay_args[APLAY_START_IDX + MAX_SOUND_FILES + 1];
	aplay_args[0] = "aplay";
	aplay_args[1] = "-t";
	aplay_args[2] = "wav";
	aplay_args[3] = "-q";

	int i = 0;
	while (p) {
		//log_debug("sound file=%s", p);
		aplay_args[APLAY_START_IDX + i] = p;
		if (++i == MAX_SOUND_FILES)
			break;
		p = strtok_r(NULL, ";", &saveptr);
	}

	aplay_args[APLAY_START_IDX + i] = NULL;

	char *aplay_cmds[] = {"/usr/bin/aplay", "/bin/aplay"};

	gboolean ok = exec_linux_cmd(aplay_cmds, sizeof(aplay_cmds)/sizeof(char*), aplay_args);

	free(bak);

	return ok;
}

static void plugin_play_speed (char *value)
{
	if (! snd_provider.play_speed_func)
		return;

	py_ext_lock();

	PyObject *pArgs = PyTuple_New(1);
	PyObject *pValue;

	pValue = PyString_FromString(value);
	PyTuple_SetItem(pArgs, 0, pValue);

	pValue = PyObject_CallObject(snd_provider.play_speed_func, pArgs);
	char *files = PyString_AsString(pValue);

	play_sound_files(files);

	Py_DECREF(pArgs);
	Py_DECREF(pValue);

	py_ext_unlock();
}

static void plugin_play_event(snd_evt_type_t evt_type)
{
	if (! snd_provider.play_event_func)
		return;

	py_ext_lock();

	PyObject *pArgs = PyTuple_New(1);
	PyObject *pValue = PyInt_FromLong(evt_type);
	PyTuple_SetItem(pArgs, 0, pValue);

	pValue = PyObject_CallObject(snd_provider.play_event_func, pArgs);
	char *files = PyString_AsString(pValue);

	play_sound_files(files);

	Py_DECREF(pArgs);
	Py_DECREF(pValue);

	py_ext_unlock();
}


static void* play_sound_routine(void *arg)
{
	time_t tm;
	char speed_buf[20];
	float speed_2d;
	int sv_in_use;

	running = TRUE;

	sigset_t sig_set;
	sigemptyset(&sig_set);
	sigaddset(&sig_set, SIGINT);

	pthread_context_t *ctx = register_thread("play sound thread", NULL, NULL);

	last_time_signal_weak = time(NULL);

	/* FIXME: may need lock */
	while (running) {
		if (snd_provider.initialized) {
			tm = time(NULL);
			if (dbus_power_is_low() && tm - last_time_low_power > NOTIFY_INTERVAL_EVENT) {
				last_time_low_power = tm;
				plugin_play_event(SND_EVT_LOW_POWER);
				WAIT_A_WHILE(1000);
			}

			if (POLL_STATE_TEST(RUNNING)) {
				/* GPS data is protected by gdk global lock */
				pthread_sigmask(SIG_BLOCK, &sig_set, NULL);
				LOCK_UI();
				speed_2d = g_gpsdata.speed_2d;
				if (g_context.speed_unit == SPEED_UNIT_KMPH) {
					speed_2d *= MPS_TO_KMPH;
				} else if (g_context.speed_unit == SPEED_UNIT_MPH) {
					speed_2d *= MPS_TO_MPH;
				}
				sv_in_use = g_gpsdata.sv_in_use;
				UNLOCK_UI();
				pthread_sigmask(SIG_UNBLOCK, &sig_set, NULL);

				if (g_gpsdata.vel_valid) {
					tm = time(NULL);
					if (tm - last_time_speed > NOTIFY_INTERVAL_SPEED) {
						last_time_speed = tm;
						sprintf(speed_buf, "%.1f", speed_2d);
						plugin_play_speed(speed_buf);
						WAIT_A_WHILE(1000);
					}
				}

				if (sv_in_use < 3) {
					tm = time(NULL);
					if (tm - last_time_signal_weak > NOTIFY_INTERVAL_EVENT) {
						last_time_signal_weak = tm;
						plugin_play_event(SND_EVT_GPS_SIGNAL_WEAK);
						WAIT_A_WHILE(1000);
					}
				}
			}
		}

		WAIT_A_WHILE(5000);
	}

	if (ctx)
		free(ctx);

	running = FALSE;

	return NULL;
}

void sound_init()
{
	running = FALSE;

	if (! g_cfg->last_sound_file)
		return;

	char *file = strdup(g_cfg->last_sound_file);

	if (! set_plugin(file))
		g_cfg->last_sound_file = NULL;

	free(file);

	return;
}

gboolean toggle_sound(gboolean enable)
{
	if (enable) {
		if (! running) {
			if (pthread_create(&tid, NULL, play_sound_routine, NULL) != 0) {
				warn_dialog("Enable sound failed: unable to create thread");
				return FALSE;
			}
		}
	} else {
		if (running) {
			running = FALSE;
			pthread_kill(tid, SIGUSR1);
			pthread_join(tid, NULL);
			tid = 0;
		}
	}
	return TRUE;
}

static void cleanup_objects()
{
	if (snd_provider.module) {
		Py_DECREF(snd_provider.module);
		snd_provider.module = NULL;
	}
	if (snd_provider.play_speed_func) {
		Py_DECREF(snd_provider.play_speed_func);
		snd_provider.play_speed_func = NULL;
	}
	if (snd_provider.play_event_func) {
		Py_DECREF(snd_provider.play_event_func);
		snd_provider.play_event_func = NULL;
	}
}

void sound_cleanup()
{
	toggle_sound(FALSE);

	cleanup_objects();
}

/**
 * @file: module file in config dir, short name
 */
gboolean set_plugin(char *file)
{
	PyObject *init_func = NULL, *play_speed_func = NULL, *play_event_func = NULL;

	int len = strlen(file) - strlen(".py");
	char module_name[256];
	strncpy(module_name, file, len);
	module_name[len] = '\0';

	py_ext_lock();

	PyObject *pName = PyString_FromString(module_name);

	PyObject *module = PyImport_Import(pName);
	Py_DECREF(pName);

	if (module == NULL) {
		log_warn("load sound module from file failed");
		PyErr_Print();
		goto END;
	}

	init_func = PyObject_GetAttrString(module, FUNC_INIT);

	if (! init_func || ! PyCallable_Check(init_func)) {
		if (init_func) {
			Py_DECREF(init_func);
			init_func = NULL;
		}
		log_error("Sound: can't find function: \"%s\"", FUNC_INIT);
		goto END;
	}

	play_speed_func = PyObject_GetAttrString(module, FUNC_PLAY_SPEED);

	if (! play_speed_func || ! PyCallable_Check(play_speed_func)) {
		if (play_speed_func) {
			Py_DECREF(play_speed_func);
			play_speed_func = NULL;
		}
		log_error("Sound: can't find function: \"%s\"", FUNC_PLAY_SPEED);
		goto END;
	}

	play_event_func = PyObject_GetAttrString(module, FUNC_PLAY_EVENT);

	if (! play_event_func || ! PyCallable_Check(play_event_func)) {
		if (play_event_func) {
			Py_DECREF(play_event_func);
			play_event_func = NULL;
		}
		log_error("Sound: can't find function: \"%s\"", FUNC_PLAY_EVENT);
		goto END;
	}

END:

	py_ext_unlock();

	if (module && init_func && play_speed_func && play_event_func) {
		py_ext_lock();

		PyObject *pValue = PyObject_CallObject(init_func, NULL);
		if (! pValue) {
			log_warn("Sound: call init() function failed--return NULL");
			return FALSE;
		}

		int ok = (int)PyLong_AsLong(pValue);
		Py_DECREF(pValue);

		py_ext_unlock();

		if (ok) {
			cleanup_objects();
			snd_provider.module = module;
			snd_provider.play_speed_func = play_speed_func;
			snd_provider.play_event_func = play_event_func;
		} else {
			log_warn("Sound: call init() function failed--return value == 0");
		}
		snd_provider.initialized = ok;

		//toggle_sound(TRUE);

		return ok;
	}
	return FALSE;
}
