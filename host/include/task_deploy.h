
#ifndef TASK_DEPLOY_H
#define TASK_DEPLOY_H

#include <stdint.h>
#include "../../common/include/task_types.h"

#define RMS 0
#define EDF 1

// Enviar una tarea al nodo
int deploy_task(int fd, const Task *task, uint8_t algoritm);

#endif
