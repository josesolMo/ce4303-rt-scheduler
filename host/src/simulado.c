#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "../include/scheduler.h"
#include "../include/ipc_shm.h"
#include "../include/serial_driver.h"




// Simular eventos recibidos por un driver

int simular_driver(SharedMemory *mem, int nodo) {

    for (int i = 1; i <= 3; i++) {

        Event event;

        event.type = EVENTO_TICK;
        event.node = nodo;
        event.task = i;
        event.tick = i * 10;

        // Guardar evento en memoria compartida
        if (ipc_send_event(mem, &event) == -1) {
            printf("Error al agregar evento\n");
            return 1;
        }

        printf("[Driver %d] Evento enviado: tarea %d, tick %d\n",
               nodo, event.task, event.tick);
    }

    return 0;
}

// Simular el proceso reportero
int simular_reportero(SharedMemory *mem) {

    Event event;

    while (1) {

        // Esperar y recibir un evento
        if (ipc_get_event(mem, &event) == -1) {
            printf("Error al recibir evento\n");
            return 1;
        }

        // Terminar cuando el padre lo indique
        if (event.type == EVENTO_FIN) {
            break;
        }

        printf("[Reportero] Nodo %d, tarea %d, tick %d\n",
               event.node, event.task, event.tick);
    }

    printf("[Reportero] Finalizado\n");

    return 0;
}