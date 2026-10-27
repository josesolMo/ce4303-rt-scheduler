#include "../include/scheduler.h"
#include "../include/planner_bridge.h"
#include "../../common/include/wire_format.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint32_t task_cost_ticks(const Task *task, uint32_t capacidad) {
    if (!task || capacidad == 0U || task->T == 0U) {
        return UINT32_MAX;
    }
    uint64_t work = (uint64_t)task->W;
    uint64_t cap = (uint64_t)capacidad;
    return (uint32_t)((work + cap - 1U) / cap);
}

static double task_utilization(const Task *task, uint32_t capacidad) {
    if (!task || task->T == 0U || capacidad == 0U) {
        return 0.0;
    }
    uint32_t costo = task_cost_ticks(task, capacidad);
    return (double)costo / (double)task->T;
}

static double rms_bound(int n) {
    if (n <= 0) {
        return 0.0;
    }
    return (double)n * (pow(2.0, 1.0 / (double)n) - 1.0);
}

static double node_utilization_for_node(const Plan *plan, int node, uint32_t capacidad) {
    double total = 0.0;
    if (!plan) {
        return 0.0;
    }
    for (int i = 0; i < plan->cantidad; i++) {
        if (plan->nodo[i] == node) {
            total += task_utilization(&plan->tareas[i], capacidad);
        }
    }
    return total;
}

static int node_admits_task_exact(const Plan *plan, int node, const Task *task,
                                 uint32_t capacidad, int algoritmo) {
    if (!plan || !task || capacidad == 0U) {
        return 0;
    }

    double total = node_utilization_for_node(plan, node, capacidad);
    double nuevo = total + task_utilization(task, capacidad);

    if (algoritmo == ALG_EDF) {
        return nuevo <= 1.0 + DBL_EPSILON;
    }

    if (algoritmo == ALG_RMS) {
        int count = 0;
        for (int i = 0; i < plan->cantidad; i++) {
            if (plan->nodo[i] == node) {
                count++;
            }
        }
        count++;
        return nuevo <= rms_bound(count) + DBL_EPSILON;
    }

    return 0;
}

static int choose_node(const Plan *plan, const uint32_t capacidades[2],
                       const Task *task, int algoritmo, int heuristica) {
    int elegido = -1;
    double mejor_criterio = -1.0;

    for (int nodo = 0; nodo < 2; nodo++) {
        if (!node_admits_task_exact(plan, nodo + 1, task, capacidades[nodo], algoritmo)) {
            continue;
        }

        uint32_t costo = task_cost_ticks(task, capacidades[nodo]);
        double margen = (double)capacidades[nodo] - (double)costo;
        if (heuristica == 1) {
            if (elegido < 0 || margen > mejor_criterio) {
                elegido = nodo + 1;
                mejor_criterio = margen;
            }
        } else {
            if (elegido < 0) {
                elegido = nodo + 1;
                mejor_criterio = margen;
            }
        }
    }

    return elegido;
}

static int build_default_plan(Plan *plan, const Config *cfg,
                             const uint32_t capacidades[2]) {
    static const Task base[] = {
        {1, 'P', 10, 40, 40, 5},
        {2, 'P', 12, 60, 60, 4},
        {3, 'P', 14, 80, 80, 3},
        {4, 'S', 8, 50, 50, 2},
    };

    int count = (int)(sizeof(base) / sizeof(base[0]));
    int usados = 0;

    for (int i = 0; i < count; i++) {
        int nodo = choose_node(plan, capacidades, &base[i],
                              cfg ? cfg->algoritmo : ALG_EDF,
                              cfg ? cfg->heuristica : 0);
        if (nodo < 0) {
            return -1;
        }
        plan->tareas[plan->cantidad] = base[i];
        plan->nodo[plan->cantidad] = nodo;
        plan->cantidad++;
        usados++;
    }

    return usados > 0 ? 0 : -1;
}

static int node_set_is_schedulable(const Plan *plan, int node, uint32_t capacidad,
                                  int algoritmo) {
    if (!plan || node < 1 || node > 2 || capacidad == 0U) {
        return 0;
    }

    double total = 0.0;
    int count = 0;
    for (int i = 0; i < plan->cantidad; i++) {
        if (plan->nodo[i] == node) {
            total += task_utilization(&plan->tareas[i], capacidad);
            count++;
        }
    }

    if (algoritmo == ALG_EDF) {
        return total <= 1.0 + DBL_EPSILON;
    }
    if (algoritmo == ALG_RMS) {
        return total <= rms_bound(count) + DBL_EPSILON;
    }
    return 0;
}

int planner_preparar(const Config *c, const uint32_t capacidades[2], Plan *p) {
    if (!c || !p) {
        return -1;
    }

    memset(p, 0, sizeof(*p));

    if (plan_leer(c, p) == 0) {
        for (int i = 0; i < p->cantidad; i++) {
            int nodo = p->nodo[i];
            if (nodo < 1 || nodo > 2) {
                return -1;
            }
            if (!node_admits_task_exact(p, nodo, &p->tareas[i], capacidades[nodo - 1], c->algoritmo)) {
                return -1;
            }
        }
        return 0;
    }

    return build_default_plan(p, c, capacidades);
}

int planner_reubicar(const Plan *p, const uint32_t capacidades[2],
                     int nodo_caido, int nodo_vivo,
                     Task *resultado, int maximo) {
    if (!p || !resultado || maximo <= 0 || nodo_caido < 1 || nodo_caido > 2 ||
        nodo_vivo < 1 || nodo_vivo > 2 || nodo_caido == nodo_vivo) {
        return 0;
    }

    int escritos = 0;
    Plan prueba = {0};
    memcpy(&prueba, p, sizeof(Plan));

    for (int i = 0; i < p->cantidad; i++) {
        if (p->nodo[i] != nodo_caido) {
            continue;
        }
        if (p->tareas[i].type == 'S') {
            continue;
        }

        Task t = p->tareas[i];
        int ok = 0;
        for (int j = 0; j < prueba.cantidad; j++) {
            if (prueba.nodo[j] == nodo_caido) {
                prueba.nodo[j] = nodo_vivo;
            }
        }
        if (node_admits_task_exact(&prueba, nodo_vivo, &t, capacidades[nodo_vivo - 1], ALG_EDF) ||
            node_admits_task_exact(&prueba, nodo_vivo, &t, capacidades[nodo_vivo - 1], ALG_RMS)) {
            ok = 1;
        }

        if (ok) {
            resultado[escritos++] = t;
            if (escritos >= maximo) {
                break;
            }
        }
    }

    return escritos;
}

int planner_admitir_manual(Plan *p, const uint32_t capacidades[2], Task tarea,
                          int *nodo_elegido) {
    if (!p || !nodo_elegido) {
        return -1;
    }

    *nodo_elegido = -1;
    double mejor_slack = -1.0;
    int mejor_nodo = -1;

    for (int nodo = 0; nodo < 2; nodo++) {
        int node = nodo + 1;
        if (!node_admits_task_exact(p, node, &tarea, capacidades[nodo], ALG_EDF) &&
            !node_admits_task_exact(p, node, &tarea, capacidades[nodo], ALG_RMS)) {
            continue;
        }

        double total = node_utilization_for_node(p, node, capacidades[nodo]) +
                       task_utilization(&tarea, capacidades[nodo]);
        double slack = (double)capacidades[nodo] - total;
        if (mejor_nodo < 0 || slack > mejor_slack) {
            mejor_nodo = node;
            mejor_slack = slack;
        }
    }

    if (mejor_nodo < 0) {
        return -1;
    }

    p->tareas[p->cantidad] = tarea;
    p->nodo[p->cantidad] = mejor_nodo;
    p->cantidad++;
    *nodo_elegido = mejor_nodo;
    return 0;
}