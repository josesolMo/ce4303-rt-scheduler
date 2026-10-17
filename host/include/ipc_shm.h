#ifndef IPC_SHM_H
#define IPC_SHM_H

#include <semaphore.h>

#define SHM_NAME "/ce4303_host"
#define MAX_EVENTS 100


// Informacion de un evento
typedef struct {
    int type;
    int node;
    int task;
    int tick;
} Event;


typedef struct {
    Event events[MAX_EVENTS]; // Arreglo de eventos

    int head;           // Indice del primer evento
    int tail;           // Indice del ultimo evento

    sem_t mutex;        // Para controlar el acceso a la memoria
    sem_t free;    // Para notificar al host que hay un evento nuevo
    sem_t available; // Para notificar al host que hay espacio disponible
} SharedMemory;

// Crear la memoria compartida
SharedMemory *ipc_crear(void);
SharedMemory *ipc_connect(void);

// Liberar la memoria compartida
void ipc_disconnect(SharedMemory *mem);
void ipc_destroy(SharedMemory *mem);

#endif