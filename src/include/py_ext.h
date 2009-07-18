#ifndef PY_EXT_H_
#define PY_EXT_H_

void py_ext_init();
void py_ext_cleanup();

void inline py_ext_trylock();
void inline py_ext_lock();
void inline py_ext_unlock();

#endif /* PY_EXT_H_ */
