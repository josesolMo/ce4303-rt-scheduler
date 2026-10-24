
#include "../include/task_deploy.h"
#include "../include/serial_driver.h"
#include "../../common/include/protocol.h"

#include <stdio.h>

// Guardar un entero de 4 bytes en little endian
static void save_u32(uint8_t *datos, uint32_t valor) {

    datos[0] = (uint8_t)(valor & 0xFF);
    datos[1] = (uint8_t)((valor >> 8) & 0xFF);
    datos[2] = (uint8_t)((valor >> 16) & 0xFF);
    datos[3] = (uint8_t)((valor >> 24) & 0xFF);
}

// Enviar una tarea al microcontrolador
int deploy_task(int fd, const Task *task, uint8_t algoritm) {

    if (fd < 0 || task == NULL) {
        return -1;
    }

    // Comprobar que los datos sean validos
    if (algoritm != RMS &&
        algoritm != EDF) {
        return -1;
    }

    if (task->type != 'P' && task->type != 'S') {
        return -1;
    }

    if (task->W == 0 || task->T == 0 || task->D == 0) {
        return -1;
    }

    uint8_t datos[16];

    // Preparar la informacion de la tarea
    datos[0] = algoritm;
    datos[1] = task->id;
    datos[2] = (uint8_t)task->type;

    save_u32(datos + 3, task->W);
    save_u32(datos + 7, task->T);
    save_u32(datos + 11, task->D);

    datos[15] = task->priority;

    // Enviar trama con el protocolo de common
    if (serial_send(fd, MSG_TASK_DEPLOY, datos, 16) == -1) {
        printf("Error al enviar tarea %u\n", task->id);
        return -1;
    }

    printf("Tarea %u enviada\n", task->id);

    uint8_t tipo;
    uint8_t respuesta[255];
    uint8_t longitud;

    // Esperar confirmacion del nodo
    int resultado = serial_receive(fd, &tipo, respuesta, sizeof(respuesta), &longitud, 2000);

    if (resultado != SERIAL_OK) {
        printf("Error o timeout esperando ACK de tarea %u\n", task->id);
        return -1;
    }

    // El nodo indica que rechazo la tarea
    if (tipo == MSG_NACK_ERR) {
        printf("Nodo rechazo la tarea %u\n", task->id);
        return -1;
    }

    // Verificar el ACK y el ID de la tarea
    if (tipo != MSG_ACK || longitud != 1 || respuesta[0] != task->id) {
        printf("ACK incorrecto para tarea %u\n", task->id);
        return -1;
    }
    printf("Tarea %u confirmada por el nodo\n", task->id);
    return 0;
}
