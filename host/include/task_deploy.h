#ifndef TASK_DEPLOY_H
#define TASK_DEPLOY_H

#include <stdint.h>
#include "../../common/include/task_types.h"

#define RMS 0
#define EDF 1

// Enviar y esperar ACK (util para pruebas individuales)
int deploy_task(int fd, const Task *task, uint8_t algoritm);

// Solo enviar la tarea (el driver principal espera el ACK)
int deploy_task_send_only(int fd, const Task *task, uint8_t algoritm);

#endif