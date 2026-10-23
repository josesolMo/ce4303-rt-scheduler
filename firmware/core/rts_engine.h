#ifndef RTS_ENGINE_H
#define RTS_ENGINE_H

/*
Calendarizador local (RMS / EDF) para el ESP32 y Raspberry Pi Pico

Uso por tick:
    1. rts_tick(): revisa plazos, libera tareas y elige la siguiente tarea a ejecutar
    2. (el nodo actualiza los LEDs con la tarea elegida)
    3. rts_run(): ejecuta el trabajo de un tick de esa tarea

Criterio de desempate:
    1. Mayos criticidad
    2. Menor id
*/


#include "../../common/include/task_types.h"
#include <stdint.h>
#include <stdbool.h>

#define RTS_MAX_TASKS 16 // Maximo numero de tareas que puede manejar el scheduler (Nota, minimo son 8)
#define RTS_IDLE 0xFF // Id que se reporta cuando no hay tareas listas para ejecutar

// Tipos de datos para el scheduler
typedef enum {
    RTS_RMS = 0, // Scheduler de tasa fija (Rate Monotonic Scheduling)
    RTS_EDF = 1, // Scheduler de fecha de entrega (Earliest Deadline First)
} rts_algo_t;

// Tipos de eventos que genera el calendarizador
typedef enum {

    RTS_EV_RELEASE = 0, // Se libera una tarea
    RTS_EV_SWITCH, // Se cambia de tarea
    RTS_EV_COMPLETE, // Se completa una tarea
    RTS_EV_DEADLINE_MISS, // Se pierde la fecha de entrega de una tarea
    RTS_EV_SPORADIC_IGNORED // Se presiono el boton antes del intervalo minimo

}rst_event_type_t;


// Estructura de datos para el calendarizador
typedef struct {

    uint8_t type; // rts_event_type_t
    uint8_t task_id; // Id de la tarea que genero el evento
    uint8_t prev_id; // Solo para RTS_EV_SWITCH, id de la tarea que estaba corriendo antes del cambio
    uint32_t tick; // Ticks desde el inicio
    uint32_t value; // Dato extra segun el tipo de evento

} rts_event_t;

// Funcion que recibe los eventos. En el nodo los mete al buffer circular.
// En la prueba de PC los imprime
typedef void (*rts_event_cb_t)(const rts_event_t *ev, void *ctx);

// Estado de una tarea dentro del nodo
typedef struct {

    Task info; // Informacion de la tarea

    bool active; // Indica si la tarea esta activa (liberada y no completada)
    uint32_t remaining_ut; // Unidades de trabajo restantes para completar la tarea
    uint32_t release_tick; // Tick en el que se libero la tarea
    uint32_t abs_deadline; // Tick en el que se debe completar la tarea (release_tick + D)
    uint32_t next_release; // Tick en el que se debe liberar la siguiente instancia de la tarea (release_tick + T)(solo periodicas)

    // Para las esporadicas
    volatile bool sporadic_pending; // Lo pone el boton
    bool has_released; // Ya hubo al menos una liberacion
    uint32_t last_release; // Tick de la ultima liberacion aceptada

    // Estadisticas de la tarea
    uint32_t releases;
    uint32_t completions;
    uint32_t misses;
    uint32_t ignored;

} rts_task_t;

// Estructura de datos del calendarizador. Contiene todas las tareas y el estado del scheduler
typedef struct {

    rts_algo_t algo; // Algoritmo de planificacion
    uint32_t ut_per_tick; // Unidades de trabajo por tick
    rts_task_t tasks[RTS_MAX_TASKS]; // Arreglo de tareas
    int count; 

    bool running; // Indica si el scheduler esta corriendo
    uint32_t now; // Tick actual (0 = msj de inicio)
    int current;
    uint8_t last_id; // Id de la ultima tarea que se ejecuto (para detectar cambios de contexto)
    uint32_t busy_ticks; // Ticks que si corrio alguna tarea

    rts_event_cb_t on_event; // Callback para reportar eventos
    void *ctx; // Contexto para el callback

} rts_engine_t;

// Inicializa el scheduler
void rts_init(rts_engine_t *e, rts_algo_t algo, uint32_t ut_per_tick, rts_event_cb_t on_event, void *ctx);

// Agrega una tarea. SE puede llamar antes o despues de rts_start().
// Retorna 0 si se agrego correctamente, -1 si es invalida, esta repetida o no hay espacio
int rts_add_task(rts_engine_t *e, const Task *t);

// Pone el tick en cero y empieza a liberar trabajos
void rts_start(rts_engine_t *e);

// Detiene el calendarizador (las tareas se conservan)
void rts_stop(rts_engine_t *e);

// Registra que se presiono el boton. Solo
void rts_sporadic_trigger(rts_engine_t *e);

// Paso 1 del tick: plazos, liberaciones y seleccion
// Retorna el id de la tarea elegida o RTS_IDLE
uint8_t rts_tick(rts_engine_t *e);

// Paso 2 del tick: ejecuta el trabajo de la tarea elegida y avanza el tick
void rts_run(rts_engine_t *e);

#endif // RTS_ENGINE_H