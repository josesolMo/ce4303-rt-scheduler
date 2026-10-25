#ifndef REPORTER_H
#define REPORTER_H
#include "ipc_shm.h"
#include "host_config.h"
void reporter_ejecutar(SharedMemory *m, const Config *c, const Plan *p);
#endif
