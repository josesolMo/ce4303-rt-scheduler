#ifndef PLANNER_BRIDGE_H
#define PLANNER_BRIDGE_H
#include "host_config.h"

// API del planificador host; las asignaciones pasan por las funciones de admision.
int planner_preparar(const Config *c, const uint32_t capacidades[2], Plan *p);

// Intenta readmitir tareas periodicas en el nodo superviviente.
// Retorna la cantidad de tareas que pueden desplegarse en ese nodo.
int planner_reubicar(const Plan *p, const uint32_t capacidades[2],
                     int nodo_caido, int nodo_vivo,
                     Task *resultado, int maximo);

// Admite una tarea manual y agrega al plan la asignacion seleccionada.
int planner_admitir_manual(Plan *p, const uint32_t capacidades[2],
                          Task tarea, int *nodo_elegido);
#endif
