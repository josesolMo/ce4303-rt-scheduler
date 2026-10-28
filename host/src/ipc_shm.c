#define _POSIX_C_SOURCE 200809L

#include "../include/ipc_shm.h"

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

// Tiempo actual en milisegundos, usado para monitorear los reportes
uint64_t ipc_now_ms(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000;
}

// Esperar un semaforo sin busy waiting
int ipc_wait(sem_t *sem) {
    while (sem_wait(sem) == -1) {
        if (errno == EINTR) {
            continue;
        }
        perror("Error en sem_wait");
        return -1;
    }
    return 0;
}

// Crear memoria compartida y todos los semaforos
SharedMemory *ipc_create(void) {
    int shm_fd = shm_open(mem_NAME, O_CREAT | O_EXCL | O_RDWR, 0600);

    if (shm_fd == -1) {
        perror("Error en shm_open");
        return NULL;
    }

    if (ftruncate(shm_fd, sizeof(SharedMemory)) == -1) {
        perror("Error en ftruncate");
        close(shm_fd);
        shm_unlink(mem_NAME);
        return NULL;
    }

    SharedMemory *mem = mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);

    if (mem == MAP_FAILED) {
        perror("Error en mmap");
        shm_unlink(mem_NAME);
        return NULL;
    }

    // Los campos de memoria nueva comienzan en cero
    mem->head = 0;
    mem->tail = 0;
    mem->node_capacity[0] = 0;
    mem->node_capacity[1] = 0;

    // Primero los semaforos de la cola
    if (sem_init(&mem->mutex, 1, 1) == -1) {
        goto error;
    }
    if (sem_init(&mem->free, 1, MAX_EVENTS) == -1) {
        sem_destroy(&mem->mutex);
        goto error;
    }
    if (sem_init(&mem->available, 1, 0) == -1) {
        sem_destroy(&mem->free);
        sem_destroy(&mem->mutex);
        goto error;
    }

    // Luego los semaforos que utiliza cada driver
    int creados = 0;
    for (int i = 0; i < 2; i++) {
        if (sem_init(&mem->command_ready[i], 1, 0) == -1) {
            break;
        }
        if (sem_init(&mem->command_done[i], 1, 0) == -1) {
            sem_destroy(&mem->command_ready[i]);
            break;
        }
        if (sem_init(&mem->driver_ready[i], 1, 0) == -1) {
            sem_destroy(&mem->command_done[i]);
            sem_destroy(&mem->command_ready[i]);
            break;
        }
        creados++;
    }

    if (creados != 2) {
        for (int i = 0; i < creados; i++) {
            sem_destroy(&mem->command_ready[i]);
            sem_destroy(&mem->command_done[i]);
            sem_destroy(&mem->driver_ready[i]);
        }
        sem_destroy(&mem->available);
        sem_destroy(&mem->free);
        sem_destroy(&mem->mutex);
        goto error;
    }

    return mem;

error:
    perror("Error al crear semaforos");
    munmap(mem, sizeof(SharedMemory));
    shm_unlink(mem_NAME);
    return NULL;
}

// Conectarse a la memoria que ya creo el proceso padre
SharedMemory *ipc_connect(void) {
    int shm_fd = shm_open(mem_NAME, O_RDWR, 0600);

    if (shm_fd == -1) {
        perror("Error al conectar SHM");
        return NULL;
    }

    SharedMemory *mem = mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);

    if (mem == MAP_FAILED) {
        perror("Error en mmap");
        return NULL;
    }
    return mem;
}

// Desconectar a un proceso sin destruir la memoria de los demas
void ipc_disconnect(SharedMemory *mem) {
    if (mem != NULL) {
        munmap(mem, sizeof(SharedMemory));
    }
}

// Se llama solo desde el padre cuando todos los hijos terminaron
void ipc_destroy(SharedMemory *mem) {
    if (mem == NULL) {
        return;
    }

    for (int i = 0; i < 2; i++) {
        sem_destroy(&mem->command_ready[i]);
        sem_destroy(&mem->command_done[i]);
        sem_destroy(&mem->driver_ready[i]);
    }

    sem_destroy(&mem->mutex);
    sem_destroy(&mem->free);
    sem_destroy(&mem->available);
    munmap(mem, sizeof(SharedMemory));
    shm_unlink(mem_NAME);
}

// Guardar un evento en la cola circular
int ipc_send_event(SharedMemory *mem, const Event *event) {
    if (mem == NULL || event == NULL) {
        return -1;
    }

    // No dejar esperando indefinidamente al driver si la cola se llena
    if (sem_trywait(&mem->free) == -1) {
        if (ipc_wait(&mem->mutex) == 0) {
            mem->lost_events++;
            sem_post(&mem->mutex);
        }
        return -1;
    }

    if (ipc_wait(&mem->mutex) == -1) {
        sem_post(&mem->free);
        return -1;
    }

    mem->events[mem->tail] = *event;
    mem->tail = (mem->tail + 1) % MAX_EVENTS;
    sem_post(&mem->mutex);
    sem_post(&mem->available);
    return 0;
}

// Leer un evento de la cola circular
int ipc_get_event(SharedMemory *mem, Event *event) {
    if (mem == NULL || event == NULL) {
        return -1;
    }

    if (ipc_wait(&mem->available) == -1) {
        return -1;
    }
    if (ipc_wait(&mem->mutex) == -1) {
        sem_post(&mem->available);
        return -1;
    }

    *event = mem->events[mem->head];
    mem->head = (mem->head + 1) % MAX_EVENTS;
    sem_post(&mem->mutex);
    sem_post(&mem->free);
    return 0;
}

// Guardar la capacidad que llego desde un microcontrolador
int ipc_set_capacity(SharedMemory *mem, int node, uint32_t capacity) {
    if (mem == NULL || node < 1 || node > 2 || capacity == 0) {
        return -1;
    }

    if (ipc_wait(&mem->mutex) == -1) {
        return -1;
    }
    mem->node_capacity[node - 1] = capacity;
    sem_post(&mem->mutex);
    return 0;
}

// Leer la capacidad de uno de los nodos
int ipc_get_capacity(SharedMemory *mem, int node, uint32_t *capacity) {
    if (mem == NULL || node < 1 || node > 2 || capacity == NULL) {
        return -1;
    }

    if (ipc_wait(&mem->mutex) == -1) {
        return -1;
    }
    *capacity = mem->node_capacity[node - 1];
    sem_post(&mem->mutex);

    if (*capacity == 0) {
        return -1;
    }
    return 0;
}

// Enviar un comando a un driver y esperar su respuesta
int ipc_send_command(SharedMemory *mem, int node, Command *command) {
    if (mem == NULL || command == NULL || node < 1 || node > 2) {
        return -1;
    }

    int pos = node - 1;
    mem->commands[pos] = *command;
    sem_post(&mem->command_ready[pos]);

    // Se usa un timeout por si uno de los procesos deja de responder
    struct timespec limite;
    clock_gettime(CLOCK_REALTIME, &limite);
    limite.tv_sec += 5;

    int result;
    do {
        result = sem_timedwait(&mem->command_done[pos], &limite);
    } while (result == -1 && errno == EINTR);

    if (result == -1) {
        perror("Timeout esperando driver");
        return -1;
    }

    *command = mem->commands[pos];
    return command->result;
}

// Actualizar si el puerto esta conectado y si el nodo esta ejecutando
int ipc_set_status(SharedMemory *mem, int node, int connected, int started, uint32_t capacity) {
    if (mem == NULL || node < 1 || node > 2) {
        return -1;
    }
    if (ipc_wait(&mem->mutex) == -1) {
        return -1;
    }

    int pos = node - 1;
    mem->connected[pos] = connected;
    mem->started[pos] = started;
    mem->node_capacity[pos] = capacity;

    if (started && mem->start_node_ms[pos] == 0) {
        mem->start_node_ms[pos] = ipc_now_ms();
    }

    sem_post(&mem->mutex);
    return 0;
}

// Guardar la hora del ultimo reporte de tick recibido
int ipc_mark_report(SharedMemory *mem, int node, uint64_t when_ms) {
    if (mem == NULL || node < 1 || node > 2) {
        return -1;
    }
    if (ipc_wait(&mem->mutex) == -1) {
        return -1;
    }

    mem->last_report_ms[node - 1] = when_ms;
    sem_post(&mem->mutex);
    return 0;
}