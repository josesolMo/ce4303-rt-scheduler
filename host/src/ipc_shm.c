#include "../include/ipc_shm.h"

#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

// Crear la memoria compartida
SharedMemory *ipc_crear(void) {

    // Crear el objeto de memoria compartida
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0600);

    // Darle el tamaño de nuestra estructura
    ftruncate(shm_fd, sizeof(SharedMemory));

    // Mapear la memoria para poder utilizarla
    SharedMemory *shm = mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    close(shm_fd);

    shm->head = 0;
    shm->tail = 0;
    
    // Se inicializan los semaforos
    sem_init(&shm->mutex, 1, 1);
    sem_init(&shm->free, 1, MAX_EVENTS);
    sem_init(&shm->available, 1, 0);

    // Semaforo compartido entre procesos
    sem_init(&shm->mutex, 1, 1);

    return shm;
}

// Esta es para conectarse a la memoria que ya existe
SharedMemory *ipc_connect(void) {
     int shm_fd = shm_open(SHM_NAME, O_RDWR, 0600);

     SharedMemory *shm = mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

     close(shm_fd);

     return shm;
}

// Desconecta un proceso de la memoria compartida
void ipc_disconnect(SharedMemory *shm) {
    if (shm != NULL) {
        munmap(shm, sizeof(SharedMemory));
    }
}

// Liberar los recursos al finalizar
void ipc_destroy(SharedMemory *shm) {
    if (shm == NULL) {
        return;
    }
    sem_destroy(&shm->mutex);
    sem_destroy(&shm->free);
    sem_destroy(&shm->available);
    munmap(shm, sizeof(SharedMemory));
    shm_unlink(SHM_NAME);
}