#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "../../common/include/task_types.h"
#include "host_config.h"

// Admisión y particionado de tareas en el planificador host.
int planner_preparar(const Config *c, const uint32_t capacidades[2], Plan *p);
int planner_reubicar(const Plan *p, const uint32_t capacidades[2],
                    int nodo_caido, int nodo_vivo,
                    Task *resultado, int maximo);
int planner_admitir_manual(Plan *p, const uint32_t capacidades[2],
                          Task tarea, int *nodo_elegido);

#endif // SCHEDULER_H