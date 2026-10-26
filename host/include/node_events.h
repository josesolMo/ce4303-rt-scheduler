#ifndef NODE_EVENTS_H
#define NODE_EVENTS_H

#include <stdint.h>
#include "ipc_shm.h"

#define NODE_EVENT 1
#define NODE_IGNORED 2
#define NODE_TIMEOUT 0
#define NODE_ERROR -1
#define NODE_INVALID -2

// Enviar el mensaje de inicio a un nodo
int node_send_init(int fd);

// Recibir un mensaje directamente (para pruebas individuales)
int node_receive_event(int fd, SharedMemory *mem, int node, int timeout_ms);

// Convertir una trama ya recibida en un evento de la cola
int node_process_message(SharedMemory *mem, int node, uint8_t type, const uint8_t *data, uint8_t len);

#endif