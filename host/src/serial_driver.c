#define _DEFAULT_SOURCE

#include "../include/serial_driver.h"
#include "../../common/include/protocol.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <poll.h>

// Con esta se abre y cierra el puerto serial
int serial_open(const char *port){
    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);

    if (fd == -1){
        perror("Error al abrir puerto serial");
        return -1;
    }

    struct termios config;

    if (tcgetattr(fd, &config) == -1){
        perror("Error al obtener configuraciones");
        close(fd);
        return -1;
    }

    // Se configura una comunicación a 115200 baudios
    cfsetispeed(&config, B115200);
    cfsetospeed(&config, B115200);

    // Configurar comunicacion de 8 bits, sin paridad
    cfmakeraw(&config);
    config.c_cflag |= (CLOCAL | CREAD);
    config.c_cflag &= ~CSIZE;
    config.c_cflag |= CS8;

    // No esperar datos dentro de read()
    config.c_cc[VMIN] = 0;
    config.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &config) == -1) {
        perror("Error al configurar puerto");
        close(fd);
        return -1;
    }

    // Volver a modo bloqueante
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags == -1 ||
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == -1) {

        perror("Error al configurar descriptor");
        close(fd);
        return -1;
    }

    printf("Puerto serial abierto: %s\n", port);

    return fd;
}

// Cerrar puerto serial
void serial_close(int fd) {

    if (fd >= 0) {
        close(fd);
    }
}

// Enviar una trama por el puerto serial
int serial_send(int fd, uint8_t type, const uint8_t *data, uint8_t len){
    if (fd < 0 || (len > 0 && data == NULL)) {
        return -1;
    }

    // Máximo: 3 bytes de header + 255 de datos + 2 CRC
    uint8_t frame[260];

    // Se crea el frame con el protocolo de common
    size_t tam = protocol_build_frame(type, data, len, frame);

    if (tam == 0){return -1;};

    size_t sent = 0;

    // Enviar todos los bytes ajio
    while (sent < tam) {

        ssize_t n = write(fd, frame + sent, tam - sent);

        if (n < 0) {
            if (errno == EINTR){continue;}

            perror("Error al enviar el frame");
            return -1;
        }

        if (n == 0) {return -1;}

        sent += (size_t)n;
    }
    return 0;
}

static int serial_read_bytes(int fd, uint8_t *buffer, size_t quantity, int timeout_ms){

    size_t received = 0;

    while (received < quantity) {
        // Esto es la estructura de la biblioteca poll
        struct pollfd pfd;

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        // Se esperan los datos gracias a poll
        int result = poll(&pfd, 1, timeout_ms);

        if (result == 0){
            return SERIAL_TIMEOUT;
        }

        if (result < 0) {
            if (errno == EINTR){continue;}
            
            perror("Error en poll");
            return SERIAL_ERROR;
        }

        // Detectar problemas con el puerto
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return SERIAL_ERROR;
        }

        if (!(pfd.revents & POLLIN)) {
            return SERIAL_ERROR;
        }

        ssize_t n = read(fd, buffer + received, quantity - received);

        if (n < 0) {

            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }

            perror("Error al leer serial");
            return SERIAL_ERROR;
        }

        if (n == 0) {return SERIAL_ERROR;}

        received += (size_t)n;
    }

    return SERIAL_OK;
}


// Recibir una trama completa
int serial_receive(int fd, uint8_t *type, uint8_t *data, size_t capacity, uint8_t *len, int timeout_ms) {

    if (fd < 0 || type == NULL || len == NULL) {
        return SERIAL_ERROR;
    }

    uint8_t trama[260];

    // Buscar el byte de inicio
    do {

        int resultado = serial_read_bytes(fd, trama, 1, timeout_ms);

        if (resultado != SERIAL_OK) {return resultado;}

    } while (trama[0] != START_BYTE);

    // Leer tipo de mensaje y longitud que son las siguientes posiciones de trama[1 y 2]
    int resultado = serial_read_bytes(
        fd, trama + 1, 2, timeout_ms
    );

    if (resultado != SERIAL_OK) {
        return resultado;
    }

    uint8_t tam_datos = trama[2];

    // Leer datos y CRC
    resultado = serial_read_bytes(
        fd, trama + 3, tam_datos + 2, timeout_ms
    );

    if (resultado != SERIAL_OK) {
        return resultado;
    }
    // Esto es del CRC
    // Comprobar que la trama sea correcta recibe la trama y longitud
    if (!protocol_validate_frame(trama, 3 + tam_datos + 2)) {
        return SERIAL_INVALID;
    }
  
    // Comprobar espacio disponible en memoria, para las tramas pues
    if (tam_datos > capacity || (tam_datos > 0 && data == NULL)) {
        return SERIAL_INVALID;
    }

    // Extraer la informacion
    *type = trama[1];
    *len = tam_datos;
    // memcpy copia los bytes de un lugar a otro en memoria, copia la trama en data
    memcpy(data, trama + 3, tam_datos);
    
    return SERIAL_OK;
}
