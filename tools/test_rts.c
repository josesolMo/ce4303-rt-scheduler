/*
 * Prueba del calendarizador en la PC (sin placa).
 *
 * Compilar desde la raíz del repo:
 *   gcc -Wall -Wextra -I common/include -I firmware/core \
 *       tools/test_rts.c firmware/core/rts_engine.c firmware/core/ut.c -o test_rts
 *
 * Ejecutar:
 *   ./test_rts
 */

#include <stdio.h>
#include "../firmware/core/rts_engine.h"

// Imprime cada evento que genera el calendarizador
static void print_event(const rts_event_t *ev, void *ctx)
{
    (void)ctx;

    switch (ev->type) {
    case RTS_EV_RELEASE:
        printf("  [%3u] libera T%u (plazo abs %u)\n",
               ev->tick, ev->task_id, ev->value);
        break;
    case RTS_EV_COMPLETE:
        printf("  [%3u] termina T%u (respuesta %u ticks)\n",
               ev->tick, ev->task_id, ev->value);
        break;
    case RTS_EV_DEADLINE_MISS:
        printf("  [%3u] PLAZO PERDIDO T%u (le faltaban %u UT)\n",
               ev->tick, ev->task_id, ev->value);
        break;
    case RTS_EV_SPORADIC_IGNORED:
        printf("  [%3u] boton ignorado T%u (solo pasaron %u ticks)\n",
               ev->tick, ev->task_id, ev->value);
        break;
    case RTS_EV_SWITCH:
        // Los cambios se ven en el Gantt, no se imprimen aquí
        break;
    }
}

// Corre 'ticks' ticks e imprime el diagrama de Gantt.
// 'triggers' son los ticks en que se "presiona" el botón (termina en -1).
static void simulate(rts_engine_t *e, int ticks, const int *triggers)
{
    char gantt[256];
    int  t_idx = 0;

    rts_start(e);

    for (int i = 0; i < ticks && i < (int)sizeof(gantt) - 1; i++) {
        if (triggers != NULL && triggers[t_idx] == i) {
            rts_sporadic_trigger(e);
            t_idx++;
        }

        uint8_t id = rts_tick(e);
        rts_run(e);

        gantt[i] = (id == RTS_IDLE) ? '.' : (char)('0' + id);
        gantt[i + 1] = '\0';
    }

    printf("\n  Gantt (un caracter por tick, '.' = libre):\n  ");
    for (int i = 0; gantt[i] != '\0'; i++) {
        printf("%c", gantt[i]);
        if (i % 5 == 4) {
            printf(" ");
        }
    }
    printf("\n\n");

    for (int i = 0; i < e->count; i++) {
        const rts_task_t *t = &e->tasks[i];
        printf("  T%u: liberados=%u terminados=%u perdidos=%u ignorados=%u\n",
               t->info.id, t->releases, t->completions, t->misses, t->ignored);
    }
    printf("  Ocupacion: %u de %d ticks\n\n", e->busy_ticks, ticks);
}

int main(void)
{
    rts_engine_t e;

    // Caso 1: carga que cumple con EDF pero no con RMS.
    // Con s_n = 1, W es igual al costo en ticks.
    // U = 2/5 + 4/7 = 0.97 (cabe en EDF, pasa el límite de RMS)
    Task a = { .id = 1, .type = 'P', .W = 2, .T = 5, .D = 5, .priority = 1 };
    Task b = { .id = 2, .type = 'P', .W = 4, .T = 7, .D = 7, .priority = 1 };

    printf("=== Caso 1: RMS ===\n");
    rts_init(&e, RTS_RMS, 1, print_event, NULL);
    rts_add_task(&e, &a);
    rts_add_task(&e, &b);
    simulate(&e, 35, NULL);

    printf("=== Caso 1: EDF ===\n");
    rts_init(&e, RTS_EDF, 1, print_event, NULL);
    rts_add_task(&e, &a);
    rts_add_task(&e, &b);
    simulate(&e, 35, NULL);

    // Caso 2: tarea esporádica.
    // El botón se presiona en los ticks 2, 6 y 14. El de 6 se ignora
    // porque el intervalo mínimo es 10.
    Task p = { .id = 1, .type = 'P', .W = 1, .T = 4,  .D = 4,  .priority = 1 };
    Task s = { .id = 3, .type = 'S', .W = 2, .T = 10, .D = 10, .priority = 2 };
    int presses[] = { 2, 6, 14, -1 };

    printf("=== Caso 2: esporadica con EDF ===\n");
    rts_init(&e, RTS_EDF, 1, print_event, NULL);
    rts_add_task(&e, &p);
    rts_add_task(&e, &s);
    simulate(&e, 20, presses);

    return 0;
}