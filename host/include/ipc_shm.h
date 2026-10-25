#ifndef IPC_SHM_H
#define IPC_SHM_H

#include <stdint.h>
#include <semaphore.h>
#include "../../common/include/task_types.h"

#define mem_NAME "/ce4303_host"
#define MAX_EVENTS 4096

// Tipos de eventos internos para la cola del host
#define EVENTO_FIN -1
#define EVENTO_TICK 1
#define EVENTO_ESPORADICO 2
#define EVENTO_DEADLINE 3
#define EVENTO_EJECUCION 4
#define EVENTO_LIBERACION 5
#define EVENTO_FINALIZACION 6
#define EVENTO_ESPORADICO_IGNORADO 7
#define EVENTO_NODO_CAIDO 8
#define EVENTO_TAREA_DESPLEGADA 9
#define EVENTO_TAREA_RECHAZADA 10
#define EVENTO_TRAMA_MALA 11
#define EVENTO_INICIO 12

// Comandos que el padre envia a cada driver
#define CMD_CALIBRAR 1
#define CMD_DESPLEGAR 2
#define CMD_INICIAR 3
#define CMD_DETENER 4
#define CMD_SALIR 5
#define CMD_DISPARAR 6
#define CMD_CONFIGURAR 7

// Informacion de un evento recibido
// value es un dato adicional, por ejemplo el motivo de un rechazo
// tick representa el tiempo medido desde el inicio del nodo
typedef struct {
    int type;
    int node;
    int task;
    uint32_t tick;
    int value;
} Event;

// Comando que el padre entrega a uno de los drivers
typedef struct {
    int type;
    uint32_t tick_us;
    uint16_t report_every;
    uint8_t algorithm;
    Task task;
    int result;
    uint32_t capacity;
} Command;

// Esta memoria la utilizan el padre, los drivers y el reportero
typedef struct {
    Event events[MAX_EVENTS];
    int head;
    int tail;
    unsigned int lost_events;

    // Semaforos para la cola de eventos
    sem_t mutex;
    sem_t free;
    sem_t available;

    // Un comando a la vez para cada driver
    Command commands[2];
    sem_t command_ready[2];
    sem_t command_done[2];
    sem_t driver_ready[2];

    // Estado de los dos microcontroladores
    int connected[2];
    int started[2];
    uint32_t node_capacity[2];
    uint64_t last_report_ms[2];
    uint64_t start_node_ms[2];
} SharedMemory;

// Crear, conectar y eliminar memoria compartida
SharedMemory *ipc_create(void);
SharedMemory *ipc_connect(void);
void ipc_disconnect(SharedMemory *mem);
void ipc_destroy(SharedMemory *mem);

// Cola de eventos entre drivers y reportero
int ipc_send_event(SharedMemory *mem, const Event *event);
int ipc_get_event(SharedMemory *mem, Event *event);

// Capacidad de un nodo: 1 ESP32, 2 Pico
int ipc_set_capacity(SharedMemory *mem, int node, uint32_t capacity);
int ipc_get_capacity(SharedMemory *mem, int node, uint32_t *capacity);

// Comunicacion del padre con los drivers
int ipc_send_command(SharedMemory *mem, int node, Command *command);
int ipc_set_status(SharedMemory *mem, int node, int connected, int started, uint32_t capacity);
int ipc_mark_report(SharedMemory *mem, int node, uint64_t when_ms);
uint64_t ipc_now_ms(void);
int ipc_wait(sem_t *sem);

#endif