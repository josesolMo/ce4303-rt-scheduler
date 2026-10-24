#include "../include/ipc_shm.h"

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>


// Crear la memoria compartida
SharedMemory *ipc_create(void) {

    // Crear el objeto de memoria compartida
    int shm_fd = shm_open(mem_NAME, O_CREAT | O_EXCL | O_RDWR, 0600);

    if (shm_fd == -1) {
        perror("Error en shm_open");
        return NULL;
    }

    // Darle el tamaño de nuestra estructura
    if (ftruncate(shm_fd, sizeof(SharedMemory)) == -1) {
        perror("Error en ftruncate");
        close(shm_fd);
        shm_unlink(mem_NAME);
        return NULL;
    }

    // Mapear la memoria para poder utilizarla
    SharedMemory *mem = mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    close(shm_fd);

    if (mem == MAP_FAILED) {
        perror("Error en mmap");
        shm_unlink(mem_NAME);
        return NULL;
    }

    mem->head = 0;
    mem->tail = 0;

    // Inicialmente ningun nodo esta calibrado
    mem->node_capacity[0] = 0;
    mem->node_capacity[1] = 0;
    
    // Se inicializan los semaforos
    sem_init(&mem->mutex, 1, 1);
    sem_init(&mem->free, 1, MAX_EVENTS);
    sem_init(&mem->available, 1, 0);

    return mem;
}

// Esta es para conectarse a la memoria que ya existe
SharedMemory *ipc_connect(void) {
     int shm_fd = shm_open(mem_NAME, O_RDWR, 0600);

     SharedMemory *mem = mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

     close(shm_fd);

     return mem;
}

// Desconecta un proceso de la memoria compartida
void ipc_disconnect(SharedMemory *mem) {
    if (mem != NULL) {
        munmap(mem, sizeof(SharedMemory));
    }
}

// Liberar los recursos al finalizar
void ipc_destroy(SharedMemory *mem) {
    if (mem == NULL) {
        return;
    }
    sem_destroy(&mem->mutex);
    sem_destroy(&mem->free);
    sem_destroy(&mem->available);
    munmap(mem, sizeof(SharedMemory));
    shm_unlink(mem_NAME);
}

// Esperar un semáforo
static int wait_semaforo(sem_t *sem){

    while (sem_wait(sem) == -1){
        // Si una señal interrumpe, volvemos a esperar
        if (errno == EINTR){continue;};

        perror("Error en sem_wait");
        return -1;
        
    }
    return 0;
}

// Esta guarda un evento en la cola
int ipc_send_event(SharedMemory *mem, const Event *event){
    // Revisar que haya espacio
    if (wait_semaforo(&mem->free) == -1){
        return -1;
    }

    // Se bloquea la cola mientras se escribe
    if (wait_semaforo(&mem->mutex) == -1){
        sem_post(&mem->free);
        return -1;
    }

    // Se guarda el evento
    mem->events[mem->tail] = *event;

    // Avanza la siguiente posición 
    mem->tail = (mem->tail +1) % MAX_EVENTS;

    // Desbloquear la cola con el mutex
    sem_post(&mem->mutex);

    // Avisar que hay un evento disponible
    sem_post(&mem->available);

    return 0;
}

// Sacar el evento de la cola
int ipc_get_event(SharedMemory *mem, Event *event){

    if (mem == NULL || event == NULL) {
        return -1;
    }
    
    // Esperar hasta que haya un evento disponible
    if (wait_semaforo(&mem->available) == -1) {
        return -1;
    }

    // Bloquear la cola mientras se lee
    if (wait_semaforo(&mem->mutex) == -1) {
        sem_post(&mem->available);
        return -1;
    }
    
    // Leer el evento
    *event = mem->events[mem->head];

    // Avanzar a la siguiente posicion
    mem->head = (mem->head + 1) % MAX_EVENTS;

    // Desbloquear la cola
    sem_post(&mem->mutex);

    // Avisar que quedo un espacio libre
    sem_post(&mem->free);

    return 0;
}


// Guardar capacidad de un nodo
int ipc_set_capacity(SharedMemory *mem, int node, uint32_t capacity) {

    if (mem == NULL || node < 1 || node > 2 || capacity == 0) {
        return -1;
    }

    // Bloquear acceso a memoria
    if (wait_semaforo(&mem->mutex) == -1) {
        return -1;
    }

    mem->node_capacity[node - 1] = capacity;

    // Liberar acceso
    sem_post(&mem->mutex);

    return 0;
}

// Obtener capacidad de un nodo
int ipc_get_capacity(SharedMemory *mem, int node, uint32_t *capacity) {

    if (mem == NULL || node < 1 || node > 2 || capacity == NULL) {
        return -1;
    }

    if (wait_semaforo(&mem->mutex) == -1) {
        return -1;
    }

    *capacity = mem->node_capacity[node - 1];

    sem_post(&mem->mutex);

    // Si es cero, todavia no se ha calibrado
    if (*capacity == 0) {
        return -1;
    }

    return 0;
}
