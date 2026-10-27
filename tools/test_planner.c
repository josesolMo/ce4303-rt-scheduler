/*
 * Pruebas del planificador del host (host/src/scheduler.c), sin nodos.
 * Cada caso tiene el resultado esperado calculado a mano.
 *
 * Compilar y correr desde la raiz del repo:
 *   make test_planner && ./test_planner
 */
#include <stdio.h>
#include <string.h>
#include "../host/include/planner_bridge.h"
#include "../common/include/wire_format.h"

static int fallos = 0;

static void revisar(int ok, const char *nombre)
{
    printf("  [%s] %s\n", ok ? "OK" : "FALLA", nombre);
    if (!ok) {
        fallos++;
    }
}

// Escribe un archivo de tareas temporal y arma el plan con el planificador
static int preparar(const char *tareas, int algoritmo, int heuristica,
                    uint32_t s1, uint32_t s2, Plan *p)
{
    const char *ruta = "/tmp/ce4303_test_tareas.txt";
    FILE *f = fopen(ruta, "w");
    if (!f) {
        return -1;
    }
    fputs(tareas, f);
    fclose(f);

    Config c;
    memset(&c, 0, sizeof(c));
    c.algoritmo = algoritmo;
    c.heuristica = heuristica;
    snprintf(c.archivo_tareas, sizeof(c.archivo_tareas), "%s", ruta);

    uint32_t sn[2] = {s1, s2};
    return planner_preparar(&c, sn, p);
}

static int nodo_de(const Plan *p, int id)
{
    int i = plan_buscar(p, id);
    return (i < 0) ? -1 : p->nodo[i];
}

int main(void)
{
    Plan p;

    // Con s_n = 100, W = 100*C. Asi U = C/T es facil de calcular.
    printf("First-Fit contra Worst-Fit (EDF, s1=100, s2=100)\n");
    const char *carga = "1,P,300,10,10,1\n"   // U = 0.3
                        "2,P,200,10,10,1\n"   // U = 0.2
                        "3,P,100,10,10,1\n";  // U = 0.1
    preparar(carga, ALG_EDF, 0, 100, 100, &p);
    revisar(nodo_de(&p, 1) == 1 && nodo_de(&p, 2) == 1 && nodo_de(&p, 3) == 1,
            "FF: las tres al nodo 1 (el primero donde caben)");
    preparar(carga, ALG_EDF, 1, 100, 100, &p);
    revisar(nodo_de(&p, 1) == 1 && nodo_de(&p, 2) == 2 && nodo_de(&p, 3) == 2,
            "WF: T1->1, T2->2 (queda mas libre), T3->2 (0.7 libre contra 0.6)");

    printf("First-Fit recorre los nodos por s_n decreciente\n");
    preparar("1,P,100,10,10,1\n", ALG_EDF, 0, 50, 100, &p);
    revisar(nodo_de(&p, 1) == 2, "FF con s2 > s1: la tarea va al nodo 2");

    printf("Orden por utilizacion decreciente (en el nodo de mayor s_n)\n");
    preparar("1,P,100,10,10,1\n2,P,500,10,10,1\n3,P,300,10,10,1\n", ALG_EDF, 0, 100, 100, &p);
    revisar(p.tareas[0].id == 2 && p.tareas[1].id == 3 && p.tareas[2].id == 1,
            "orden: T2 (0.5), T3 (0.3), T1 (0.1)");

    printf("Rechazo\n");
    preparar("1,P,1200,10,10,1\n2,P,100,10,10,1\n", ALG_EDF, 0, 100, 100, &p);
    revisar(nodo_de(&p, 1) == 0, "T1 con U = 1.2 en ambos nodos queda rechazada (nodo 0)");
    revisar(nodo_de(&p, 2) == 1, "T2 se asigna igual");

    printf("Limite de RMS\n");
    preparar("1,P,300,10,10,1\n2,P,300,10,10,1\n3,P,300,10,10,1\n", ALG_RMS, 0, 100, 100, &p);
    revisar(nodo_de(&p, 1) == 1 && nodo_de(&p, 2) == 1,
            "T1 y T2 al nodo 1 (0.6 <= 0.828)");
    revisar(nodo_de(&p, 3) == 2, "T3 al nodo 2 (0.9 > 0.780 en el nodo 1)");
    preparar("1,P,300,10,10,1\n2,P,300,10,10,1\n3,P,300,10,10,1\n", ALG_EDF, 0, 100, 100, &p);
    revisar(nodo_de(&p, 3) == 1, "con EDF la misma T3 si cabe en el nodo 1 (0.9 <= 1)");

    printf("Una esporadica por nodo\n");
    preparar("1,S,100,10,10,1\n2,S,100,10,10,1\n3,S,100,10,10,1\n", ALG_EDF, 0, 100, 100, &p);
    revisar(nodo_de(&p, 1) == 1 && nodo_de(&p, 2) == 2 && nodo_de(&p, 3) == 0,
            "S1->1, S2->2, S3 rechazada");

    printf("Reubicacion por criticidad\n");
    // Nodo 1 (vivo) con 0.1; el nodo 2 se cae con 0.5 (crit 9), 0.3 (crit 5), 0.3 (crit 1)
    memset(&p, 0, sizeof(p));
    Task t[4] = {
        {1, 'P', 100, 10, 10, 5},
        {2, 'P', 300, 10, 10, 1},
        {3, 'P', 300, 10, 10, 5},
        {4, 'P', 500, 10, 10, 9},
    };
    int nodos[4] = {1, 2, 2, 2};
    for (int i = 0; i < 4; i++) {
        p.tareas[i] = t[i];
        p.nodo[i] = nodos[i];
    }
    p.cantidad = 4;
    uint32_t sn[2] = {100, 100};
    Task r[MAX_TAREAS];
    int n = planner_reubicar(&p, sn, 2, 1, ALG_EDF, r, MAX_TAREAS);
    revisar(n == 2 && r[0].id == 4 && r[1].id == 3,
            "EDF: pasan T4 y T3, se rechaza T2 (la de menor criticidad)");
    n = planner_reubicar(&p, sn, 2, 1, ALG_RMS, r, MAX_TAREAS);
    revisar(n == 1 && r[0].id == 4, "RMS: solo pasa T4 (0.6 <= 0.828; con T3 seria 0.9 > 0.780)");

    printf("Modo manual\n");
    preparar("1,P,500,10,10,1\n", ALG_EDF, 0, 100, 100, &p);
    Task m = {9, 'P', 300, 10, 10, 1};
    int elegido = 0;
    uint32_t solo_nodo2[2] = {0, 100};   // nodo 1 caido
    int ok = planner_admitir_manual(&p, solo_nodo2, m, ALG_EDF, 0, &elegido);
    revisar(ok == 0 && elegido == 2, "con el nodo 1 caido, la tarea manual va al nodo 2");

    printf("\n%s (%d fallas)\n", fallos ? "HAY FALLAS" : "Todo bien", fallos);
    return fallos ? 1 : 0;
}
