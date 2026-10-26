#include "../include/task_deploy.h"
#include "../include/serial_driver.h"
#include "../../common/include/protocol.h"

#include <stdio.h>

// Convertir un entero en 4 bytes little endian
static void save_u32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)(value & 0xFF);
    data[1] = (uint8_t)((value >> 8) & 0xFF);
    data[2] = (uint8_t)((value >> 16) & 0xFF);
    data[3] = (uint8_t)((value >> 24) & 0xFF);
}

// Enviar los datos de una tarea sin esperar la respuesta
int deploy_task_send_only(int fd, const Task *task, uint8_t algoritm) {
    if (fd < 0 || task == NULL) {
        return -1;
    }
    if (algoritm != RMS && algoritm != EDF) {
        return -1;
    }
    if (task->type != 'P' && task->type != 'S') {
        return -1;
    }
    if (task->id == 0 || task->W == 0 || task->T == 0 || task->D == 0) {
        return -1;
    }

    uint8_t data[16];
    data[0] = algoritm;
    data[1] = task->id;
    data[2] = (uint8_t)task->type;
    save_u32(data + 3, task->W);
    save_u32(data + 7, task->T);
    save_u32(data + 11, task->D);
    data[15] = task->priority;

    // protocol.c agrega el header y CRC automaticamente
    if (serial_send(fd, MSG_TASK_DEPLOY, data, 16) == -1) {
        printf("Error al enviar tarea %u\n", task->id);
        return -1;
    }
    return 0;
}

// Enviar una tarea y esperar su ACK
int deploy_task(int fd, const Task *task, uint8_t algoritm) {
    if (deploy_task_send_only(fd, task, algoritm) == -1) {
        return -1;
    }

    uint8_t type;
    uint8_t response[255];
    uint8_t len;
    int result = serial_receive(fd, &type, response, sizeof(response), &len, 2000);

    if (result != SERIAL_OK) {
        printf("Error esperando ACK de la tarea %u\n", task->id);
        return -1;
    }
    if (type != MSG_ACK || len != 1 || response[0] != task->id) {
        printf("ACK incorrecto de la tarea %u\n", task->id);
        return -1;
    }

    printf("Tarea %u confirmada\n", task->id);
    return 0;
}