#ifndef PLANNER_BRIDGE_H
#define PLANNER_BRIDGE_H
#include "host_config.h"

// API del planificador host; las asignaciones pasan por las funciones de admision.
// Lee las tareas y las asigna con la heuristica configurada. Las tareas que no
// caben en ningun nodo quedan con nodo = 0 (rechazadas).
int planner_preparar(const Config *c, const uint32_t capacidades[2], Plan *p);

// Intenta readmitir tareas periodicas en el nodo superviviente, de mayor a
// menor criticidad y con el algoritmo configurado.
// Retorna la cantidad de tareas que pueden desplegarse en ese nodo.
int planner_reubicar(const Plan *p, const uint32_t capacidades[2],
                     int nodo_caido, int nodo_vivo, int algoritmo,
                     Task *resultado, int maximo);

// Admite una tarea manual con el algoritmo y la heuristica configurados y
// agrega al plan la asignacion seleccionada.
int planner_admitir_manual(Plan *p, const uint32_t capacidades[2],
                          Task tarea, int algoritmo, int heuristica, int *nodo_elegido);
#endif
