#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "../include/scheduler.h"
#include "../include/ipc_shm.h"
#include "../include/serial_driver.h"

int main(void) {
    printf("=== CE4303 REAL-TIME SCHEDULER HOST ===\n");
    
    // Crear la memoria compartida
    SharedMemory *shm = ipc_crear();

    printf("Memoria compartida SIUUU\n");

    // Procesos que vamos a crear
    const char *nombres[] = {
        "Driver ESP32",
        "Driver Pico",
        "Reporter"
    };

    pid_t pids[3];
    int created = 0;

    printf("Proceso padre iniciado. PID: %ld\n", (long)getpid());

    // Con esto se limpia la salida de la consola
    fflush(stdout); 

    for (int i = 0; i < 3; i++) {

        pid_t pid = fork();

        if (pid == 0) {
            
            // Lo que ejecuta el chikito
            printf("Proceso hijo %s iniciado. PID: %ld\n", nombres[i], (long)getpid());
            fflush(stdout);

            // Desconectar memoria del angelito
            ipc_disconnect(shm);

            // Abortar al chikito
            _exit(0);
        }

        // El padre guarda los PID de sus hijos
        pids[i] = pid;
        created++;
    }

    // Esperar que terminen todos los hijos
    for (int i = 0; i < created; i++) {
        
        // Espera al chikito, bloqueando el proceso, sin usar el busy waiting
        waitpid(pids[i], NULL, 0);
        printf("Procesos hijos terminados\n");

        ipc_destroy(shm);

        printf("Memoria compartida chao\n");
        printf("Fin del host\n");

        return 0;
    }
}