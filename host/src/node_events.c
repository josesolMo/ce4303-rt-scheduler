#include "../include/node_events.h"
#include "../include/serial_driver.h"
#include "../../common/include/protocol.h"
#include "../../common/include/wire_format.h"

#include <stdio.h>

// Enviar inicio al microcontrolador
int node_send_init(int fd) {
    if (fd < 0) {
        return -1;
    }
    if (serial_send(fd, MSG_START_SYNC, NULL, 0) == -1) {
        printf("Error al enviar inicio\n");
        return -1;
    }
    return 0;
}

// Guardar un evento en memoria compartida
static int save_event(SharedMemory *mem, int type, int node, int task, uint32_t tick) {
    Event event = {type, node, task, tick, 0};
    return ipc_send_event(mem, &event);
}

// Procesar una trama que ya paso la revision del CRC
int node_process_message(SharedMemory *mem, int node, uint8_t type, const uint8_t *data, uint8_t len) {
    if (mem == NULL || node < 1 || node > 2 || (len > 0 && data == NULL)) {
        return NODE_INVALID;
    }

    // Un paquete de ejecucion tiene: tick inicial, cantidad e IDs de tareas
    if (type == MSG_EXEC_BATCH) {
        if (len < 6 || data[4] == 0 || data[4] > 250 || len != (uint8_t)(5 + data[4])) {
            return NODE_INVALID;
        }

        uint32_t tick = wire_get_u32(data);
        if ((uint64_t)tick + data[4] > UINT32_MAX) {
            return NODE_INVALID;
        }

        for (int i = 0; i < data[4]; i++) {
            if (save_event(mem, EVENTO_EJECUCION, node, data[5 + i], tick + (uint32_t)i) == -1) {
                return NODE_ERROR;
            }
        }
        return NODE_EVENT;
    }

    // Elegir el tipo de evento que utiliza el host
    int event_type = 0;
    if (type == MSG_TICK_REPORT) {
        event_type = EVENTO_TICK;
    } else if (type == MSG_EVENT_RELEASE) {
        event_type = EVENTO_LIBERACION;
    } else if (type == MSG_EVENT_FINISH) {
        event_type = EVENTO_FINALIZACION;
    } else if (type == MSG_EVENT_DEADLINE) {
        event_type = EVENTO_DEADLINE;
    } else if (type == MSG_EVENT_SPORADIC) {
        event_type = EVENTO_ESPORADICO;
    } else if (type == MSG_SPORADIC_SKIP) {
        event_type = EVENTO_ESPORADICO_IGNORADO;
    } else if (type == MSG_EVENT_START) {
        event_type = EVENTO_INICIO;
    } else {
        return NODE_IGNORED;
    }

    // Revisar longitud ANTES de leer los datos del payload
    if (len != 5) {
        return NODE_INVALID;
    }

    uint32_t tick = wire_get_u32(data);
    int task = data[4];

    if (event_type == EVENTO_TICK) {
        ipc_mark_report(mem, node, ipc_now_ms());
    }

    if (save_event(mem, event_type, node, task, tick) == -1) {
        return NODE_ERROR;
    }
    return NODE_EVENT;
}

// Version independiente de recepcion, utilizada por las pruebas anteriores
int node_receive_event(int fd, SharedMemory *mem, int node, int timeout_ms) {
    if (fd < 0 || mem == NULL || node < 1 || node > 2 || timeout_ms < 0) {
        return NODE_ERROR;
    }

    uint8_t type;
    uint8_t data[255];
    uint8_t len;
    int result = serial_receive(fd, &type, data, sizeof(data), &len, timeout_ms);

    if (result == SERIAL_TIMEOUT) {
        return NODE_TIMEOUT;
    }
    if (result == SERIAL_INVALID) {
        return NODE_INVALID;
    }
    if (result != SERIAL_OK) {
        return NODE_ERROR;
    }
    if (type == MSG_ACK) {
        return NODE_IGNORED;
    }
    if (type == MSG_NACK_ERR) {
        return NODE_ERROR;
    }

    return node_process_message(mem, node, type, data, len);
}