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
#include <time.h>

// Obtener hora para controlar los timeouts
static long long serial_now_ms(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (long long)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

// Abrir el puerto serial y configurarlo a 115200 baudios
int serial_open(const char *port) {
    if (port == NULL) {
        return -1;
    }

    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd == -1) {
        perror("Error al abrir puerto serial");
        return -1;
    }

    struct termios config;
    if (tcgetattr(fd, &config) == -1) {
        perror("Error en tcgetattr");
        close(fd);
        return -1;
    }

    // Configuracion de 8 bits, sin paridad y sin procesar texto
    cfmakeraw(&config);
    if (cfsetispeed(&config, B115200) == -1 || cfsetospeed(&config, B115200) == -1) {
        close(fd);
        return -1;
    }

    config.c_cflag |= (CLOCAL | CREAD);
    config.c_cflag &= ~CSIZE;
    config.c_cflag |= CS8;
    config.c_cc[VMIN] = 0;
    config.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &config) == -1) {
        perror("Error en tcsetattr");
        close(fd);
        return -1;
    }

    // Se deja no bloqueante para controlar la espera con poll()
    printf("Puerto serial abierto: %s\n", port);
    return fd;
}

// Cerrar un puerto que se abrio anteriormente
void serial_close(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

// Enviar una trama completa, incluso si write() envia solo una parte
int serial_send(int fd, uint8_t type, const uint8_t *data, uint8_t len) {
    if (fd < 0 || (len > 0 && data == NULL)) {
        return -1;
    }

    // El protocolo agrega 3 bytes de header y 2 de CRC
    uint8_t frame[260];
    size_t size = protocol_build_frame(type, data, len, frame);
    if (size == 0) {
        return -1;
    }

    size_t sent = 0;
    long long deadline = serial_now_ms() + 2000;

    while (sent < size) {
        ssize_t n = write(fd, frame + sent, size - sent);

        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n == -1 && errno == EINTR) {
            continue;
        }
        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int remaining = (int)(deadline - serial_now_ms());
            if (remaining <= 0) {
                return -1;
            }

            struct pollfd pfd = {fd, POLLOUT, 0};
            int result = poll(&pfd, 1, remaining);
            if (result > 0 && (pfd.revents & POLLOUT)) {
                continue;
            }
            if (result == -1 && errno == EINTR) {
                continue;
            }
        }

        perror("Error al enviar trama");
        return -1;
    }

    return 0;
}

// Leer una cantidad exacta de bytes, con limite total de tiempo
static int serial_read_bytes(int fd, uint8_t *buffer, size_t quantity, long long deadline) {
    size_t received = 0;

    while (received < quantity) {
        int remaining = (int)(deadline - serial_now_ms());
        if (remaining <= 0) {
            return SERIAL_TIMEOUT;
        }

        struct pollfd pfd = {fd, POLLIN, 0};
        int result = poll(&pfd, 1, remaining);

        if (result == 0) {
            return SERIAL_TIMEOUT;
        }
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SERIAL_ERROR;
        }
        if (pfd.revents & (POLLERR | POLLNVAL)) {
            return SERIAL_ERROR;
        }
        if (!(pfd.revents & POLLIN)) {
            return SERIAL_ERROR;
        }

        ssize_t n = read(fd, buffer + received, quantity - received);
        if (n > 0) {
            received += (size_t)n;
            continue;
        }
        if (n == 0) {
            return SERIAL_ERROR;
        }
        if (errno == EINTR || errno == EAGAIN) {
            continue;
        }
        return SERIAL_ERROR;
    }

    return SERIAL_OK;
}

// Recibir una trama y comprobar el CRC antes de entregar el payload
int serial_receive(int fd, uint8_t *type, uint8_t *data, size_t capacity, uint8_t *len, int timeout_ms) {
    if (fd < 0 || type == NULL || len == NULL || timeout_ms < 0) {
        return SERIAL_ERROR;
    }

    uint8_t frame[260];
    long long deadline = serial_now_ms() + timeout_ms;

    // Buscar el byte de inicio de la trama
    do {
        int result = serial_read_bytes(fd, frame, 1, deadline);
        if (result != SERIAL_OK) {
            return result;
        }
    } while (frame[0] != START_BYTE);

    // Primero leer tipo y longitud
    int result = serial_read_bytes(fd, frame + 1, 2, deadline);
    if (result != SERIAL_OK) {
        return result;
    }

    uint8_t data_size = frame[2];

    // Luego leer payload y CRC
    result = serial_read_bytes(fd, frame + 3, (size_t)data_size + 2, deadline);
    if (result != SERIAL_OK) {
        return result;
    }

    if (!protocol_validate_frame(frame, (size_t)data_size + 5)) {
        return SERIAL_INVALID;
    }
    if (data_size > capacity || (data_size > 0 && data == NULL)) {
        return SERIAL_INVALID;
    }

    *type = frame[1];
    *len = data_size;
    if (data_size > 0) {
        memcpy(data, frame + 3, data_size);
    }
    return SERIAL_OK;
}