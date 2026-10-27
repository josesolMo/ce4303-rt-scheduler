#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define START_BYTE 0xAA

// Tipos de Mensajes (Opcodes)
#define MSG_CALIB_REQ         0x01
#define MSG_CALIB_RESP        0x02
#define MSG_TASK_DEPLOY       0x03
#define MSG_ACK               0x04
#define MSG_START_SYNC        0x05
#define MSG_TICK_REPORT       0x06
#define MSG_EVENT_SPORADIC    0x07
#define MSG_EVENT_DEADLINE    0x08
#define MSG_STOP_REQ          0x09
#define MSG_NACK_ERR          0xFF

typedef struct __attribute__((packed)) {
    uint8_t start_byte;
    uint8_t type_id;
    uint8_t payload_len;
} FrameHeader;

/*
 * Protocolo serial del proyecto:
 * [START (1B) | TYPE (1B) | LEN (1B) | PAYLOAD (N) | CRC16 (2B)]
 *
 * El payload y su longitud dependen del tipo de mensaje. El host y los nodos
 * deben documentar exactamente el formato de cada payload para mantener la
 * compatibilidad entre las dos partes.
 *
 * Mensajes utilizados por la version actual del host:
 *   MSG_CALIB_REQ   -> tick_us[4]
 *   MSG_CALIB_RESP  -> ut_por_tick[4]
 *   MSG_TASK_DEPLOY -> alg[1], id[1], tipo[1], W[4], T[4], D[4], criticidad[1]
 *   MSG_ACK        -> id[1]
 *   MSG_START_SYNC -> sin payload
 *   MSG_STOP_REQ   -> sin payload
 */

/**
 * @brief Construye una trama completa lista para enviar por puerto serial.
 *        Agrega el START_BYTE, TYPE, PAYLOAD_LEN, PAYLOAD y el CRC16 al final.
 * 
 * @param type_id ID del tipo de mensaje (ej. MSG_CALIB_REQ).
 * @param payload Puntero a los datos del mensaje (puede ser NULL si payload_len es 0).
 * @param payload_len Longitud en bytes del payload.
 * @param out_buffer Buffer donde se escribirá la trama completa.
 * @return size_t Longitud total en bytes de la trama generada.
 */
size_t protocol_build_frame(uint8_t type_id, const uint8_t *payload, uint8_t payload_len, uint8_t *out_buffer);

/**
 * @brief Valida si un buffer recibido contiene una trama correcta.
 *        Verifica el START_BYTE y recalcula el CRC16.
 * 
 * @param buffer Buffer de datos recibidos.
 * @param buffer_len Longitud de datos en el buffer.
 * @return int 1 si la trama es válida, 0 si está corrupta o incompleta.
 */
int protocol_validate_frame(const uint8_t *buffer, size_t buffer_len);

#endif // PROTOCOL_H