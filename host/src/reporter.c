#include "../include/reporter.h"
#include "../include/gantt_svg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

static const char *nombre_evento(int tipo) {
    switch (tipo) {
        case EVENTO_TICK: return "TICK";
        case EVENTO_EJECUCION: return "EJECUCION";
        case EVENTO_LIBERACION: return "LIBERACION";
        case EVENTO_FINALIZACION: return "FINALIZACION";
        case EVENTO_DEADLINE: return "DEADLINE_PERDIDO";
        case EVENTO_ESPORADICO: return "ESPORADICA";
        case EVENTO_ESPORADICO_IGNORADO: return "ESPORADICA_IGNORADA";
        case EVENTO_NODO_CAIDO: return "NODO_CAIDO";
        case EVENTO_TAREA_DESPLEGADA: return "ACEPTADA_PARA_DESPLIEGUE";
        case EVENTO_TAREA_RECHAZADA: return "RECHAZADA";
        case EVENTO_TRAMA_MALA: return "TRAMA_INVALIDA";
        case EVENTO_INICIO: return "INICIO";
        default: return "DESCONOCIDO";
    }
}

static const char *motivo_rechazo(int codigo) {
    switch (codigo) {
        case 1: return "pendiente replanificacion verde";
        case 2: return "fallo despliegue de reubicacion";
        case 3: return "esporadica no reubicable";
        case 4: return "admision manual rechazada/no implementada";
        case 5: return "fallo de ACK en despliegue";
        default: return "";
    }
}

typedef struct {
    int ejecucion, liberaciones, finalizadas, deadlines, esporadicas, ignoradas;
    int espera_esp;
    long suma_latencia;
    int muestras_latencia;
} Estadistica;

void reporter_ejecutar(SharedMemory *m, const Config *c, const Plan *p) {
    
    if (mkdir(c->carpeta_reporte, 0755) == -1 && errno != EEXIST)
        perror("mkdir reportes");
    
    char ruta[512];
    
    snprintf(ruta, sizeof(ruta), "%s/eventos.csv", c->carpeta_reporte);
    FILE *eventos = fopen(ruta, "w");
    snprintf(ruta, sizeof(ruta), "%s/gantt.csv", c->carpeta_reporte);
    FILE *gantt = fopen(ruta, "w");
    snprintf(ruta, sizeof(ruta), "%s/resumen.txt", c->carpeta_reporte);
    FILE *resumen = fopen(ruta, "w");
    
    if (!eventos || !gantt || !resumen) {
        perror("No se pudieron crear reportes");
        if (eventos) {
            fclose(eventos);
        }
        if (gantt) {
            fclose(gantt);
        }
        if (resumen) {
            fclose(resumen);
        }
        ipc_disconnect(m);
        _exit(1);
    }

    fprintf(eventos, "tipo,nodo,tarea,tick,valor,motivo\n");
    fprintf(gantt, "nodo,tick,tarea\n");
    Estadistica est[2][256] = {{{0}}};
    for (int n = 0; n < 2; n++)
        for (int id = 0; id < 256; id++) {
            est[n][id].espera_esp = -1;
        }
    int ejecutados[2] = {0, 0}, cambios[2] = {0, 0};
    int anterior[2] = {-1, -1}, ultimo_tick[2] = {-1, -1};
    int caidas[2] = {0, 0}, rechazo = 0, aceptadas = 0;
    int ocupados[2] = {0, 0}, continuos[2] = {1, 1}, cantidad_ticks[2] = {0, 0};
    int primer_tick[2] = {-1, -1};
    for (;;) {
        Event e;
        if (ipc_get_event(m, &e) == -1) {
            break;
        }
        if (e.type == EVENTO_FIN) {
            break;
        }
        fprintf(eventos, "%s,%d,%d,%u,%d,%s\n", nombre_evento(e.type), e.node,
                e.task, (unsigned)e.tick, e.value,
                e.type == EVENTO_TAREA_RECHAZADA ? motivo_rechazo(e.value) : "");
        if (e.type == EVENTO_TAREA_RECHAZADA) {
            rechazo++;
        }
        if (e.type == EVENTO_TAREA_DESPLEGADA) {
            aceptadas++;
        }
        if (e.node < 1 || e.node > 2 || e.task < 0 || e.task > 255) {
            continue;
        }
        int n = e.node - 1;
        Estadistica *s = &est[n][e.task];
        if (e.type == EVENTO_EJECUCION) {
            if (primer_tick[n] == -1) {
                primer_tick[n] = (int)e.tick;
            }
            if (ultimo_tick[n] >= 0 && (uint32_t)(ultimo_tick[n] + 1) != e.tick)
                continuos[n] = 0;
            cantidad_ticks[n]++;
            if (e.task != 0) {
                ocupados[n]++;
            }
            fprintf(gantt, "%d,%u,%d\n", e.node, (unsigned)e.tick, e.task);
            if (ultimo_tick[n] >= 0 && anterior[n] != e.task) {
                cambios[n]++;
            }
            anterior[n] = e.task;
            ultimo_tick[n] = (int)e.tick;
            ejecutados[n]++;
            s->ejecucion++;
            if (s->espera_esp >= 0) {
                s->suma_latencia += (int)e.tick - s->espera_esp;
                s->muestras_latencia++;
                s->espera_esp = -1;
            }
        } else if (e.type == EVENTO_LIBERACION) s->liberaciones++;
        else if (e.type == EVENTO_FINALIZACION) {
            s->finalizadas++;
        }
        else if (e.type == EVENTO_DEADLINE) {
            s->deadlines++;
        }
        else if (e.type == EVENTO_ESPORADICO) {
            s->esporadicas++;
            s->espera_esp = (int)e.tick;
        } else if (e.type == EVENTO_ESPORADICO_IGNORADO) s->ignoradas++;
        else if (e.type == EVENTO_NODO_CAIDO) {
            caidas[n]++;
        }
    }
    fprintf(resumen, "CE4303 - REPORTE DEL HOST\n\n");
    fprintf(resumen, "Algoritmo: %s\nTick: %d ms\n", c->algoritmo ? "EDF" : "RMS", c->tick_ms);
    for (int n = 0; n < 2; n++) {
        uint32_t sn = m->node_capacity[n];
        double u = 0;
        int total_periodos = 0;
        for (int i = 0; i < p->cantidad; i++) {
            if (p->nodo[i] != n + 1 || !sn) {
                continue;
            }
            const Task *t = &p->tareas[i];
            uint64_t ci = ((uint64_t)t->W + sn - 1) / sn;
            u += (double)ci / t->T;
            total_periodos++;
        }
        fprintf(resumen, "\nNodo %d: capacidad %u UT/tick\n", n + 1, (unsigned)sn);
        fprintf(resumen, "Utilizacion teorica inicial: %s", sn ? "" : "sin calibracion\n");
        if (sn) {
            fprintf(resumen, "%.4f (%d tareas)\n", u, total_periodos);
        }
        fprintf(resumen, "Ticks recibidos para Gantt: %d\n", ejecutados[n]);
        fprintf(resumen, "Cambios de tarea observados: %d\n", cambios[n]);
        fprintf(resumen, "Caidas reportadas: %d\n", caidas[n]);
        if (primer_tick[n] == 0 && continuos[n] && cantidad_ticks[n] > 0)
            fprintf(resumen, "Utilizacion medida: %.4f (ticks completos desde cero)\n",
                    (double)ocupados[n] / cantidad_ticks[n]);
        else
            fprintf(resumen, "Utilizacion medida: no verificable (faltan ticks continuos desde cero)\n");
        for (int id = 1; id < 256; id++) {
            Estadistica *s = &est[n][id];
            if (!s->ejecucion && !s->liberaciones && !s->finalizadas &&
                !s->deadlines && !s->esporadicas && !s->ignoradas) continue;
            fprintf(resumen, "Tarea %d: ticks=%d, liberaciones=%d, finalizadas=%d, deadlines=%d, sporadicas=%d, ignoradas=%d",
                    id, s->ejecucion, s->liberaciones, s->finalizadas, s->deadlines,
                    s->esporadicas, s->ignoradas);
            if (s->muestras_latencia > 0)
                fprintf(resumen, ", latencia media=%.2f ticks",
                        (double)s->suma_latencia / s->muestras_latencia);
            fprintf(resumen, "\n");
        }
    }
    fprintf(resumen, "\nConfirmaciones de despliegue: %d\n", aceptadas);
    fprintf(resumen, "Tareas rechazadas registradas: %d\n", rechazo);
    fprintf(resumen, "Events perdidos por cola llena: %u\n", m->lost_events);
    fprintf(resumen, "Detalle de fallas y rechazos: eventos.csv\n");
    fprintf(resumen, "IMPORTANTE: sin el registro completo de ejecucion enviado por firmware, el Gantt sera parcial.\n");
    fclose(eventos); fclose(gantt); fclose(resumen);
    char csv[512], svg[512];
    snprintf(csv, sizeof(csv), "%s/gantt.csv", c->carpeta_reporte);
    snprintf(svg, sizeof(svg), "%s/gantt.svg", c->carpeta_reporte);
    if (generar_gantt_svg(csv, svg) != 0) {
        perror("gantt.svg");
    }
    ipc_disconnect(m);
    _exit(0);
}