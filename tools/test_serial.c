
#include <stdio.h>
#include <unistd.h>
#include <pty.h>

#include "../host/include/serial_driver.h"
#include "../common/include/protocol.h"

int main(void) {

    int master, slave;
    char puerto[128];

    // Crear un puerto serial virtual
    if (openpty(&master, &slave, puerto, NULL, NULL) == -1) {
        perror("Error en openpty");
        return 1;
    }

    close(slave);

    printf("Puerto virtual: %s\n", puerto);

    // Abrir puerto desde el host
    int fd = serial_open(puerto);

    if (fd == -1) {
        close(master);
        return 1;
    }

    uint8_t tipo;
    uint8_t datos[255];
    uint8_t longitud;

    // Simular mensaje enviado por el host
    printf("\nHost enviando mensaje...\n");

    if (serial_send(fd, MSG_ACK, NULL, 0) == -1) {
        printf("Error al enviar\n");
        goto error;
    }

    // Recibir desde el nodo simulado
    int resultado = serial_receive(
        master, &tipo, datos, sizeof(datos), &longitud, 2000
    );

    if (resultado != SERIAL_OK || tipo != MSG_ACK ||
        longitud != 0) {
        printf("Error al recibir en nodo\n");
        goto error;
    }

    printf("Nodo recibio MSG_ACK correctamente\n");

    // Simular respuesta del nodo
    printf("\nNodo enviando respuesta...\n");

    if (serial_send(master, MSG_ACK, NULL, 0) == -1) {
        goto error;
    }

    // Recibir desde el host
    resultado = serial_receive(
        fd, &tipo, datos, sizeof(datos), &longitud, 2000
    );

    if (resultado != SERIAL_OK || tipo != MSG_ACK ||
        longitud != 0) {
        printf("Error al recibir en host\n");
        goto error;
    }

    printf("Host recibio MSG_ACK correctamente\n");

    serial_close(fd);
    close(master);

    printf("\nPrueba serial completada\n");

    return 0;

error:
    serial_close(fd);
    close(master);
    return 1;
}
