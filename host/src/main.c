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


int main(void) {
    printf("=== CE4303 REAL-TIME SCHEDULER HOST ===\n");
    
    SharedMemory *mem = ipc_crear();

    // Revisar si se creo correctamente
    if (mem == NULL) {
        printf("No se pudo crear la memoria compartida\n");
        return 1;
    }

    printf("Memoria compartida SIUUU\n");

    /*
    // Procesos que vamos a crear
    const char *nombres[] = {
        "Driver ESP32",
        "Driver Pico",
        "Reporter"
    };
    */

    pid_t pids[3];
    int created = 0;

    printf("Proceso padre iniciado. PID: %ld\n", (long)getpid());

    // Con esto se limpia la salida de la consola
    fflush(stdout); 

    // Esto crea los procesos hijos
    for (int i = 0; i < 3; i++) {

        pid_t pid = fork();
        // Lo que ejecuta el chikito

        if (pid == 0) {

              int resultado = 0;

            if (i == 0){
                resultado = simular_driver(mem, 1);
            } else if (i == 1) {
                resultado = simular_driver(mem, 2);
            } else{
                resultado = simular_reportero(mem);
            }
            
            fflush(stdout);

            // Desconectar memoria del angelito
            ipc_disconnect(mem);

            // Abortar al chikito
            _exit(resultado);
        }

        // El padre guarda los PID de sus hijos
        pids[i] = pid;
        created++;
    }

    // Esperar que terminen todos los hijos
    for (int i = 0; i < 2; i++) {
        // Espera al chikito, bloqueando el proceso, sin usar el busy waiting
        waitpid(pids[i], NULL, 0);
    }

    // Esto avisa al reportero que ya puede terminar pe
    
    Event fin = {EVENTO_FIN, 0, 0, 0};

    ipc_send_event(mem, &fin);

    // Esperar al reportero xd
    waitpid(pids[2], NULL, 0);
    
    printf("Procesos hijos finalizados ez\n");

    // Liberar todo
    ipc_destroy(mem);
    printf("Fin del host\n");
    return 0;
}