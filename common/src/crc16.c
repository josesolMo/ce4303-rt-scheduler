#include "../include/crc16.h"

uint16_t crc16_compute(const uint8_t *data, size_t length) {
    uint16_t crc = 0xFFFF;

    if (data == NULL || length == 0) {
        return 0;
    }

    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001; // Polinomio 0x8005 invertido
            } else {
                crc >>= 1;
            }
        }
    }

    return crc;
}