
#ifndef CALIBRATION_H
#define CALIBRATION_H

#include <stdint.h>

// Solicitar y recibir la calibracion de un nodo
int node_calibration(int fd, uint32_t tick_us, uint32_t *capacidad);

#endif
