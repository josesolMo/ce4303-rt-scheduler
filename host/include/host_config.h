#ifndef HOST_CONFIG_H
#define HOST_CONFIG_H
#include <stdint.h>
#include "../../common/include/task_types.h"
#define MAX_TAREAS 64

typedef struct {
    int algoritmo;
    int heuristica; // 0 First-Fit, 1 Worst-Fit
    int tick_ms;
    int reporte_cada;
    int reportes_perdidos;
    char puerto[2][160];
    char archivo_tareas[256];
    char carpeta_reporte[256];
} Config;

typedef struct {
    Task tareas[MAX_TAREAS];
    int nodo[MAX_TAREAS];
    int cantidad;
} Plan;

int config_leer(const char *ruta, Config *c);
int plan_leer(const Config *c, Plan *p);
int plan_buscar(const Plan *p, int id);
#endif
