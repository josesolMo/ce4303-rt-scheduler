
#ifndef SERIAL_DRIVER_H
#define SERIAL_DRIVER_H

#include <stdint.h>
#include <stddef.h>

// Resultados al recibir una trama
#define SERIAL_OK 1
#define SERIAL_TIMEOUT 0
#define SERIAL_ERROR -1
#define SERIAL_INVALID -2

// Abrir y configurar puerto serial
int serial_open(const char *port);

// Cerrar puerto serial
void serial_close(int fd);

// Enviar una trama
int serial_send(int fd, uint8_t type, const uint8_t *data, uint8_t len);

// Recibir una trama
int serial_receive(int fd, uint8_t *type, uint8_t *data, size_t capacity, uint8_t *len, int timeout_ms);

#endif