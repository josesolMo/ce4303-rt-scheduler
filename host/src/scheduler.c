#include "../include/scheduler.h"
#include "../include/planner_bridge.h"
#include "../../common/include/wire_format.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// Maximo de tareas por nodo. Es el mismo valor que RTS_MAX_TASKS del
// calendarizador local (firmware/core/rts_engine.h).
#define NODO_MAX_TAREAS 16

/*
 * El costo de una tarea en un nodo se calcula como ceil(W / s_n), segun el PDF.
 * El costo es la cantidad de ticks que necesita la tarea en ese nodo.
 */
static uint32_t task_cost_ticks(const Task *task, uint32_t capacidad) {
    if (!task || capacidad == 0U || task->T == 0U) {
        return UINT32_MAX;
    }
    uint64_t work = (uint64_t)task->W;
    uint64_t cap = (uint64_t)capacidad;
    return (uint32_t)((work + cap - 1U) / cap);
}

/*
 * Utilizacion de la tarea en un nodo:
 *     U = C / T
 * donde C = ceil(W / s_n).
 */
static double task_utilization(const Task *task, uint32_t capacidad) {
    if (!task || task->T == 0U || capacidad == 0U) {
        return 0.0;
    }
    uint32_t costo = task_cost_ticks(task, capacidad);
    return (double)costo / (double)task->T;
}

/*
 * Límite de admision RMS oficial del proyecto:
 *     sum(C_i/T_i) <= n * (2^(1/n) - 1)
 */
static double rms_bound(int n) {
    if (n <= 0) {
        return 0.0;
    }
    return (double)n * (pow(2.0, 1.0 / (double)n) - 1.0);
}

/*
 * Prueba de admision de una tarea en un nodo, con el algoritmo configurado.
 * Solo cuenta las tareas que ya estan asignadas a ese nodo (nodo[i] == node);
 * las que tienen nodo 0 todavia no se asignan o fueron rechazadas.
 * Ademas revisa los limites del nodo: maximo NODO_MAX_TAREAS tareas y
 * maximo una esporadica (seccion 3.7).
 */
static int node_admits_task_exact(const Plan *plan, int node, const Task *task,
                                 uint32_t capacidad, int algoritmo) {
    if (!plan || !task || capacidad == 0U) {
        return 0;
    }

    int count = 0;
    int esporadicas = 0;
    double total = 0.0;
    for (int i = 0; i < plan->cantidad; i++) {
        if (plan->nodo[i] == node) {
            total += task_utilization(&plan->tareas[i], capacidad);
            count++;
            if (plan->tareas[i].type == 'S') {
                esporadicas++;
            }
        }
    }

    if (count >= NODO_MAX_TAREAS) {
        return 0;
    }
    if (task->type == 'S' && esporadicas > 0) {
        return 0;
    }

    double nuevo = total + task_utilization(task, capacidad);

    if (algoritmo == ALG_EDF) {
        return nuevo <= 1.0 + DBL_EPSILON;
    }

    if (algoritmo == ALG_RMS) {
        int total_tasks = count + 1;
        return nuevo <= rms_bound(total_tasks) + DBL_EPSILON;
    }

    return 0;
}

/*
 * Elige el nodo para una tarea segun la heuristica (seccion 3.4):
 *  - First-Fit: se recorren los nodos en orden decreciente de s_n y se
 *    elige el primero donde la tarea pasa la admision.
 *  - Worst-Fit: entre los nodos donde pasa la admision, se elige el que
 *    queda con mas capacidad libre despues de asignarla. Si empatan, se
 *    queda el de mayor s_n.
 * Un nodo con capacidad 0 (caido o sin calibrar) nunca se elige.
 * Retorna 1 o 2, o -1 si no cabe en ningun nodo.
 */
static int choose_node(const Plan *plan, const uint32_t capacidades[2],
                       const Task *task, int algoritmo, int heuristica) {
    int elegido = -1;
    double mejor_criterio = -1.0;

    // Orden de recorrido: primero el nodo con mayor s_n
    int orden[2] = {0, 1};
    if (capacidades[1] > capacidades[0]) {
        orden[0] = 1;
        orden[1] = 0;
    }

    for (int k = 0; k < 2; k++) {
        int nodo = orden[k];
        if (!node_admits_task_exact(plan, nodo + 1, task, capacidades[nodo], algoritmo)) {
            continue;
        }

        int count = 0;
        double total = 0.0;
        for (int i = 0; i < plan->cantidad; i++) {
            if (plan->nodo[i] == nodo + 1) {
                total += task_utilization(&plan->tareas[i], capacidades[nodo]);
                count++;
            }
        }
        double nuevo = total + task_utilization(task, capacidades[nodo]);
        double slack = (algoritmo == ALG_EDF)
            ? (1.0 - nuevo)
            : (rms_bound(count + 1) - nuevo);

        if (heuristica == 1) {
            if (elegido < 0 || slack > mejor_criterio) {
                elegido = nodo + 1;
                mejor_criterio = slack;
            }
        } else {
            // First-Fit: el primero que sirve, en orden de s_n
            elegido = nodo + 1;
            break;
        }
    }

    return elegido;
}

/*
 * Asignacion inicial (particionado). Lee las tareas del archivo, las ordena
 * de mayor a menor utilizacion medida en el nodo de mayor s_n y las asigna
 * una por una con la heuristica configurada.
 * Desempate del orden: mayor criticidad y luego menor id.
 * Si una tarea no cabe en ningun nodo queda con nodo = 0 (rechazada); el
 * main la reporta y sigue con las demas.
 * Retorna -1 solo si no se pudo leer el archivo de tareas.
 */
int planner_preparar(const Config *c, const uint32_t capacidades[2], Plan *p) {
    if (!c || !p) {
        return -1;
    }

    if (plan_leer(c, p) != 0) {
        return -1;
    }

    // Capacidad del nodo mas rapido, para ordenar
    uint32_t mayor = capacidades[0] > capacidades[1] ? capacidades[0] : capacidades[1];

    // Ordenar por insercion (son pocas tareas). Se ordenan las tareas y su
    // nodo juntos; en este punto todos los nodos son 0.
    for (int i = 1; i < p->cantidad; i++) {
        Task t = p->tareas[i];
        double ut = task_utilization(&t, mayor);
        int j = i - 1;
        while (j >= 0) {
            double uj = task_utilization(&p->tareas[j], mayor);
            int va_antes = (ut > uj + DBL_EPSILON) ||
                (fabs(ut - uj) <= DBL_EPSILON &&
                 (t.priority > p->tareas[j].priority ||
                  (t.priority == p->tareas[j].priority && t.id < p->tareas[j].id)));
            if (!va_antes) {
                break;
            }
            p->tareas[j + 1] = p->tareas[j];
            j--;
        }
        p->tareas[j + 1] = t;
    }

    // Asignar en ese orden
    for (int i = 0; i < p->cantidad; i++) {
        int nodo = choose_node(p, capacidades, &p->tareas[i], c->algoritmo, c->heuristica);
        p->nodo[i] = (nodo < 0) ? 0 : nodo;
    }

    return 0;
}

/*
 * Reubicacion cuando se cae un nodo (seccion 3.8). Las tareas que ya tenia
 * el nodo vivo se mantienen. Las periodicas del nodo caido se prueban de
 * mayor a menor criticidad (desempate: menor id), con el algoritmo
 * configurado; las que pasan la admision se suman al nodo vivo para probar
 * la siguiente. Asi, si no caben todas, se rechazan primero las de menor
 * criticidad. Las esporadicas no se reubican.
 * Retorna cuantas tareas quedaron en 'resultado'.
 */
int planner_reubicar(const Plan *p, const uint32_t capacidades[2],
                     int nodo_caido, int nodo_vivo, int algoritmo,
                     Task *resultado, int maximo) {
    if (!p || !resultado || maximo <= 0 || nodo_caido < 1 || nodo_caido > 2 ||
        nodo_vivo < 1 || nodo_vivo > 2 || nodo_caido == nodo_vivo) {
        return 0;
    }

    // 'prueba' empieza solo con las tareas que ya tiene el nodo vivo
    Plan prueba;
    memset(&prueba, 0, sizeof(prueba));
    for (int i = 0; i < p->cantidad; i++) {
        if (p->nodo[i] == nodo_vivo) {
            prueba.tareas[prueba.cantidad] = p->tareas[i];
            prueba.nodo[prueba.cantidad] = nodo_vivo;
            prueba.cantidad++;
        }
    }

    // Candidatas: periodicas del nodo caido
    Task cand[MAX_TAREAS];
    int n = 0;
    for (int i = 0; i < p->cantidad; i++) {
        if (p->nodo[i] == nodo_caido && p->tareas[i].type == 'P') {
            cand[n++] = p->tareas[i];
        }
    }

    // Ordenar de mayor a menor criticidad (desempate: menor id)
    for (int i = 1; i < n; i++) {
        Task t = cand[i];
        int j = i - 1;
        while (j >= 0 && (cand[j].priority < t.priority ||
               (cand[j].priority == t.priority && cand[j].id > t.id))) {
            cand[j + 1] = cand[j];
            j--;
        }
        cand[j + 1] = t;
    }

    // Probar una por una; las que entran se suman al nodo vivo
    int escritos = 0;
    for (int i = 0; i < n && escritos < maximo; i++) {
        if (prueba.cantidad >= MAX_TAREAS) {
            break;
        }
        if (node_admits_task_exact(&prueba, nodo_vivo, &cand[i],
                                   capacidades[nodo_vivo - 1], algoritmo)) {
            prueba.tareas[prueba.cantidad] = cand[i];
            prueba.nodo[prueba.cantidad] = nodo_vivo;
            prueba.cantidad++;
            resultado[escritos++] = cand[i];
        }
    }

    return escritos;
}

/*
 * Modo manual (seccion 3.5): se hace la misma admision y se usa la misma
 * heuristica que en la asignacion inicial. El main pasa capacidad 0 para
 * los nodos caidos, asi nunca se eligen.
 * Si se acepta, la tarea se agrega al plan con su nodo.
 */
int planner_admitir_manual(Plan *p, const uint32_t capacidades[2], Task tarea,
                          int algoritmo, int heuristica, int *nodo_elegido) {
    if (!p || !nodo_elegido) {
        return -1;
    }

    *nodo_elegido = -1;
    if (p->cantidad >= MAX_TAREAS) {
        return -1;
    }

    int nodo = choose_node(p, capacidades, &tarea, algoritmo, heuristica);
    if (nodo < 0) {
        return -1;
    }

    p->tareas[p->cantidad] = tarea;
    p->nodo[p->cantidad] = nodo;
    p->cantidad++;
    *nodo_elegido = nodo;
    return 0;
}
