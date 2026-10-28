#include "../include/host_config.h"
#include "../../common/include/wire_format.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static char *limpiar(char *s) {
    while (isspace((unsigned char)*s)) {
        s++;
    }
    char *fin = s + strlen(s);
    while (fin > s && isspace((unsigned char)fin[-1])) {
        *--fin = 0;
    }
    return s;
}

static void texto(char *destino, size_t tam, const char *dato) {
    snprintf(destino, tam, "%s", dato);
}

int config_leer(const char *ruta, Config *c) {
    if (!ruta || !c) {
        return -1;
    }
    memset(c, 0, sizeof(*c));
    c->algoritmo = ALG_EDF;
    c->heuristica = 0;
    c->tick_ms = 10;
    c->reporte_cada = 5;
    c->reportes_perdidos = 4;
    texto(c->carpeta_reporte, sizeof(c->carpeta_reporte), "output");
    FILE *f = fopen(ruta, "r");
    if (!f) {
        perror(ruta);
        return -1;
    }
    char linea[512];
    while (fgets(linea, sizeof(linea), f)) {
        char *s = limpiar(linea);
        if (!*s || *s == '#') {
            continue;
        }
        char *igual = strchr(s, '=');
        if (!igual) {
            fclose(f);
            return -1;
        }
        *igual = 0;
        char *clave = limpiar(s), *valor = limpiar(igual + 1);
        if (!strcmp(clave, "algoritmo")) {
            if (!strcmp(valor, "RMS")) {
                c->algoritmo = ALG_RMS;
            }
            else if (!strcmp(valor, "EDF")) {
                c->algoritmo = ALG_EDF;
            }
            else { fclose(f); return -1; }
        } else if (!strcmp(clave, "heuristica")) {
            if (!strcmp(valor, "FF")) {
                c->heuristica = 0;
            }
            else if (!strcmp(valor, "WF")) {
                c->heuristica = 1;
            }
            else { fclose(f); return -1; }
        } else if (!strcmp(clave, "tick_ms")) c->tick_ms = atoi(valor);
        else if (!strcmp(clave, "reporte_cada")) {
            c->reporte_cada = atoi(valor);
        }
        else if (!strcmp(clave, "reportes_perdidos")) {
            c->reportes_perdidos = atoi(valor);
        }
        else if (!strcmp(clave, "puerto_esp32")) {
            texto(c->puerto[0], sizeof(c->puerto[0]), valor);
        }
        else if (!strcmp(clave, "puerto_pico")) {
            texto(c->puerto[1], sizeof(c->puerto[1]), valor);
        }
        else if (!strcmp(clave, "archivo_tareas")) {
            texto(c->archivo_tareas, sizeof(c->archivo_tareas), valor);
        }
        else if (!strcmp(clave, "archivo_asignaciones")) {
            // Ya no se usa: la asignacion la hace el planificador con FF/WF.
            // Se acepta la clave para que no fallen configuraciones viejas.
        }
        else if (!strcmp(clave, "carpeta_reporte")) {
            texto(c->carpeta_reporte, sizeof(c->carpeta_reporte), valor);
        }
        else { fprintf(stderr, "Config desconocida: %s\n", clave); fclose(f); return -1; }
    }
    fclose(f);
    if (c->tick_ms < 1 || c->tick_ms > 1000 || c->reporte_cada < 1 ||
        c->reportes_perdidos < 1 || c->reporte_cada > 65535 || !c->puerto[0][0] || !c->puerto[1][0] ||
        !c->archivo_tareas[0]) return -1;
    return 0;
}

int plan_buscar(const Plan *p, int id) {
    for (int i = 0; i < p->cantidad; i++) {
        if (p->tareas[i].id == id) return i;
    }
    return -1;
}

// Lee las tareas del archivo (formato fijo: id,tipo,W,T,D,criticidad).
// La asignacion a nodos la hace el planificador (planner_preparar), aqui
// todas quedan con nodo 0. Una linea invalida se informa y se salta, para
// que una entrada mala no termine el host.
int plan_leer(const Config *c, Plan *p) {
    if (!c || !p) {
        return -1;
    }
    memset(p, 0, sizeof(*p));
    FILE *f = fopen(c->archivo_tareas, "r");
    if (!f) {
        perror(c->archivo_tareas);
        return -1;
    }
    char linea[256];
    while (fgets(linea, sizeof(linea), f)) {
        char *s = limpiar(linea);
        if (!*s || *s == '#') {
            continue;
        }
        unsigned int id, w, t, d, pr;
        char tipo, extra;
        if (sscanf(s, " %u , %c , %u , %u , %u , %u %c", &id, &tipo, &w, &t, &d, &pr, &extra) != 6 ||
            id < 1 || id > 255 || (tipo != 'P' && tipo != 'S') || w == 0 || t == 0 || d == 0 ||
            pr > 255 || plan_buscar(p, (int)id) >= 0) {
            fprintf(stderr, "Tarea invalida (se ignora): %s\n", s);
            continue;
        }
        if (p->cantidad == MAX_TAREAS) {
            fprintf(stderr, "Demasiadas tareas, se ignora: %s\n", s);
            continue;
        }
        p->tareas[p->cantidad] = (Task){(uint8_t)id, tipo, w, t, d, (uint8_t)pr};
        p->nodo[p->cantidad] = 0;
        p->cantidad++;
    }
    fclose(f);
    return 0;
}
