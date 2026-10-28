
#include "../include/calibration.h"
#include "../include/serial_driver.h"
#include "../../common/include/protocol.h"

#include <stdio.h>

// Para calibrar un nodo
int node_calibration(int fd, uint32_t tick_us, uint32_t *capacity) {

    if (fd < 0 || tick_us == 0 || capacity == NULL) {
        return -1;
    }

    uint8_t datos[4];

    // Guardar duracion del tick en 4 bytes
    datos[0] = (uint8_t)(tick_us & 0xFF);
    datos[1] = (uint8_t)((tick_us >> 8) & 0xFF);
    datos[2] = (uint8_t)((tick_us >> 16) & 0xFF);
    datos[3] = (uint8_t)((tick_us >> 24) & 0xFF);

    // Enviar solicitud de calibracion
    if (serial_send(fd, MSG_CALIB_REQ, datos, 4) == -1) {
        printf("Error al solicitar calibracion\n");
        return -1;
    }

    printf("Solicitud de calibracion enviada\n");

    uint8_t type;
    uint8_t respuesta[255];
    uint8_t len;

    // Esperar respuesta del nodo
    for (int i = 0; i < 3; i++) {

        int resultado = serial_receive(fd, &type, respuesta, sizeof(respuesta), &len, 2000);

        if (resultado == SERIAL_INVALID) {
            printf("Trama de calibracion corrupta\n");
            continue;
        }

        if (resultado != SERIAL_OK) {
            printf("Error o timeout de calibracion\n");
            return -1;
        }

        // Ignorar ACK si el nodo lo envia primero
        if (type == MSG_ACK) {
            continue;
        }

        if (type != MSG_CALIB_RESP || len != 4) {
            printf("Respuesta de calibracion incorrecta\n");
            return -1;
        }

        // Convertir los 4 bytes en la capacidad
        uint32_t sn =
            (uint32_t)respuesta[0] |
            ((uint32_t)respuesta[1] << 8) |
            ((uint32_t)respuesta[2] << 16) |
            ((uint32_t)respuesta[3] << 24);

        if (sn == 0) {
            printf("Capacidad invalida\n");
            return -1;
        }

        // Hace que la capacidad sea el resultado del nodo 
        *capacity = sn;

        printf("Capacidad recibida: %u UT/tick\n", (unsigned int)sn);

        return 0;
    }

    printf("No se recibio una calibracion valida\n");
    return -1;
}
