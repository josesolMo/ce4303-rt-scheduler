#ifndef CRC16_H
#define CRC16_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Calcula el checksum CRC-16 (Polinomio 0x8005 / MODBUS) sobre un arreglo de bytes.
 * 
 * @param data Puntero al buffer de datos.
 * @param length Longitud en bytes del buffer.
 * @return uint16_t Valor CRC-16 de 2 bytes resultante.
 */
uint16_t crc16_compute(const uint8_t *data, size_t length);

#endif // CRC16_H