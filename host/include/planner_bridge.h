#ifndef PLANNER_BRIDGE_H
#define PLANNER_BRIDGE_H
#include "host_config.h"

// PUENTE PROVISIONAL hacia la parte verde.
// Se carga una asignacion previa sin hacer prueba de admision.
int planner_preparar(const Config *c, const uint32_t capacidades[2], Plan *p);

// El companero verde implementara esta funcion usando admision y criticidad.
// Retorna la cantidad de tareas nuevas para el nodo superviviente.
int planner_reubicar(const Plan *p, const uint32_t capacidades[2],
                     int nodo_caido, int nodo_vivo,
                     Task *resultado, int maximo);

// El modo manual tambien necesita admision del planificador.
int planner_admitir_manual(Plan *p, const uint32_t capacidades[2],
                          Task tarea, int *nodo_elegido);
#endif
