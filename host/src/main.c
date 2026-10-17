#include <stdio.h>
#include "../include/scheduler.h"
#include "../include/ipc_shm.h"
#include "../include/serial_driver.h"

int main(void) {
    printf("=== CE4303 REAL-TIME SCHEDULER HOST ===\n");
    
    // Crear la memoria compartida
    SharedMemory *shm = ipc_crear();

    printf("Memoria compartida SIUUU\n");

    // Prueba nomás
    printf("Total eventos: %d\n", shm->tail);


    ipc_destroy(shm);
    printf("RIP Memoria compartida\n");

    return 0;
}