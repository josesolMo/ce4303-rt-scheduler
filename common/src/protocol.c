#include "../include/protocol.h"
#include "../include/crc16.h"
#include <string.h>

size_t protocol_build_frame(uint8_t type_id, const uint8_t *payload, uint8_t payload_len, uint8_t *out_buffer) {
    if (out_buffer == NULL) {
        return 0;
    }

    // Armar el encabezado
    FrameHeader *header = (FrameHeader *)out_buffer;
    header->start_byte = START_BYTE;
    header->type_id = type_id;
    header->payload_len = payload_len;

    // Copiar el payload si existe
    if (payload != NULL && payload_len > 0) {
        memcpy(out_buffer + sizeof(FrameHeader), payload, payload_len);
    }

    // Calcular CRC16 sobre Header + Payload
    size_t data_len = sizeof(FrameHeader) + payload_len;
    uint16_t crc = crc16_compute(out_buffer, data_len);

    // Adjuntar CRC16 al final de la trama (Little Endian)
    out_buffer[data_len] = (uint8_t)(crc & 0xFF);
    out_buffer[data_len + 1] = (uint8_t)((crc >> 8) & 0xFF);

    return data_len + 2; // Retorna la longitud total enviada
}

int protocol_validate_frame(const uint8_t *buffer, size_t buffer_len) {
    if (buffer == NULL || buffer_len < sizeof(FrameHeader) + 2) {
        return 0; // Trama incompleta o puntero nulo
    }

    const FrameHeader *header = (const FrameHeader *)buffer;
    if (header->start_byte != START_BYTE) {
        return 0; // Byte de inicio incorrecto
    }

    size_t expected_total_len = sizeof(FrameHeader) + header->payload_len + 2;
    if (buffer_len < expected_total_len) {
        return 0; // Faltan bytes por recibir según la longitud del payload
    }

    // Recalcular CRC16 sobre Header + Payload
    size_t data_len = sizeof(FrameHeader) + header->payload_len;
    uint16_t calculated_crc = crc16_compute(buffer, data_len);

    // Extraer el CRC16 que viene grabado al final del buffer
    uint16_t received_crc = buffer[data_len] | (buffer[data_len + 1] << 8);

    // Comprobar que coincidan
    return (calculated_crc == received_crc) ? 1 : 0;
}