#include "../include/driver.h"
#include "../include/calibration.h"
#include "../include/node_events.h"
#include "../include/task_deploy.h"
#include "../include/serial_driver.h"
#include "../../common/include/protocol.h"
#include "../../common/include/wire_format.h"
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

// Unicamente este proceso puede leer el serial de su microcontrolador.
static int recibir_y_guardar(SharedMemory *m, int nodo, int fd, int espera_ms,
                            int *tipo_recibido, int *id_recibido) {
    uint8_t tipo, len, p[255];
    int r = serial_receive(fd, &tipo, p, sizeof(p), &len, espera_ms);
    if (r == SERIAL_TIMEOUT) {
        return 0;
    }
    if (r == SERIAL_INVALID) {
        Event ev = {EVENTO_TRAMA_MALA, nodo, 0, 0, 0};
        ipc_send_event(m, &ev);
        return 2;
    }
    if (r != SERIAL_OK) {
        return -1;
    }
    if (tipo_recibido) {
        *tipo_recibido = tipo;
    }
    if (id_recibido) {
        *id_recibido = len ? p[0] : 0;
    }
    if (tipo == MSG_ACK || tipo == MSG_NACK_ERR) {
        return 3;
    }
    if (node_process_message(m, nodo, tipo, p, len) < 0) {
        Event ev = {EVENTO_TRAMA_MALA, nodo, 0, 0, 0};
        ipc_send_event(m, &ev);
    }
    return 1;
}

// Esperar un ACK sin perder eventos que lleguen mientras tanto.
static int esperar_ack(SharedMemory *m, int nodo, int fd, int id) {
    uint64_t limite = ipc_now_ms() + 2000;
    while (ipc_now_ms() < limite) {
        int tipo = -1, recibida = -1;
        int restante = (int)(limite - ipc_now_ms());
        int r = recibir_y_guardar(m, nodo, fd, restante, &tipo, &recibida);
        if (r == -1 || r == 0) {
            return -1;
        }
        if (r == 3) {
            if (tipo == MSG_NACK_ERR) {
                return -1;
            }
            if (tipo == MSG_ACK && recibida == id) {
                return 0;
            }
            // ACK de una operacion anterior: no confirma esta.
        }
    }
    return -1;
}

static int procesar_comando(SharedMemory *m, int nodo, int fd,
                           int *corriendo, uint32_t *capacidad) {
    int i = nodo - 1;
    Command *c = &m->commands[i];
    int resultado = -1;
    if (c->type == CMD_SALIR) {
        if (fd >= 0 && *corriendo) {
            serial_send(fd, MSG_STOP_REQ, NULL, 0);
        }
        *corriendo = 0;
        resultado = 0;
    } else if (fd < 0) {
        resultado = -1;
    } else if (c->type == CMD_CALIBRAR) {
        resultado = node_calibration(fd, c->tick_us, capacidad);
        ipc_set_status(m, nodo, 1, *corriendo, *capacidad);
        c->capacity = *capacidad;
    } else if (c->type == CMD_CONFIGURAR) {
        uint8_t datos[2] = {(uint8_t)c->report_every, (uint8_t)(c->report_every >> 8)};
        if (serial_send(fd, MSG_NODE_CONFIG, datos, 2) == 0)
            resultado = esperar_ack(m, nodo, fd, 0);
    } else if (c->type == CMD_DESPLEGAR) {
        if (deploy_task_send_only(fd, &c->task, c->algorithm) == 0)
            resultado = esperar_ack(m, nodo, fd, c->task.id);
        if (resultado == 0) {
            Event ev = {EVENTO_TAREA_DESPLEGADA, nodo, c->task.id, 0, 0};
            ipc_send_event(m, &ev);
        }
    } else if (c->type == CMD_INICIAR) {
        if (serial_send(fd, MSG_START_SYNC, NULL, 0) == 0) {
            // El reloj del nodo inicia cuando recibe la trama.
            resultado = esperar_ack(m, nodo, fd, 0);
            if (resultado == 0) {
                *corriendo = 1;
            }
            ipc_set_status(m, nodo, 1, *corriendo, *capacidad);
        }
    } else if (c->type == CMD_DETENER) {
        if (serial_send(fd, MSG_STOP_REQ, NULL, 0) == 0)
            resultado = esperar_ack(m, nodo, fd, 0);
        *corriendo = 0;
        ipc_set_status(m, nodo, 1, 0, *capacidad);
    } else if (c->type == CMD_DISPARAR) {
        uint8_t id = c->task.id;
        if (serial_send(fd, MSG_TRIGGER_TASK, &id, 1) == 0)
            resultado = esperar_ack(m, nodo, fd, 0);
    }
    c->result = resultado;
    sem_post(&m->command_done[i]);
    return c->type == CMD_SALIR;
}

void driver_ejecutar(SharedMemory *m, int nodo, const char *puerto) {
    int i = nodo - 1;
    int fd = serial_open(puerto);
    int corriendo = 0;
    uint32_t capacidad = 0;
    ipc_set_status(m, nodo, fd >= 0, 0, 0);
    sem_post(&m->driver_ready[i]);
    for (;;) {
        // Procesar comandos del padre.
        if (sem_trywait(&m->command_ready[i]) == 0) {
            if (procesar_comando(m, nodo, fd, &corriendo, &capacidad)) {
                break;
            }
            continue;
        }
        if (fd >= 0 && corriendo) {
            // poll dentro de serial_receive duerme mientras espera bytes.
            int r = recibir_y_guardar(m, nodo, fd, 80, NULL, NULL);
            if (r == -1) {
                fprintf(stderr, "Nodo %d: puerto desconectado\n", nodo);
                ipc_set_status(m, nodo, 0, 0, capacidad);
                serial_close(fd);
                fd = -1;
                corriendo = 0;
            }
        } else {
            // Dormir hasta que llegue un comando, sin busy waiting.
            if (ipc_wait(&m->command_ready[i]) == -1) {
                break;
            }
            if (procesar_comando(m, nodo, fd, &corriendo, &capacidad)) {
                break;
            }
        }
    }
    serial_close(fd);
    ipc_set_status(m, nodo, 0, 0, capacidad);
    ipc_disconnect(m);
    _exit(0);
}
