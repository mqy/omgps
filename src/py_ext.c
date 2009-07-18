#include <Python.h>

#include "util.h"
#include "omgps.h"

/* Must take care of global locking for embed Python in multi-thread context */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static gboolean py_initialized = FALSE;

void py_ext_init()
{
	/* If initsigs is 0, it skips initialization registration of signal handlers,
	 * which might be useful when Python is embedded */
	Py_InitializeEx(0);
	char buf[256];
	sprintf(buf, "sys.path.append('%s')\n", g_context.config_dir);
	PyRun_SimpleString("import sys\n");
	PyRun_SimpleString(buf);

	py_initialized = TRUE;
}

void py_ext_cleanup()
{
	if (py_initialized)
		Py_Finalize();
}

void inline py_ext_trylock()
{
	TRYLOCK_MUTEX(&lock);
}

void inline py_ext_lock()
{
	LOCK_MUTEX(&lock);
}

void inline py_ext_unlock()
{
	UNLOCK_MUTEX(&lock);
}
