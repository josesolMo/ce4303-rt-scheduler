#ifndef WIRE_FORMAT_H
#define WIRE_FORMAT_H

// Acuerdo entre Linux, ESP32 y Pico. Todos los uint32 van little endian.
#include <stdint.h>

#define ALG_RMS 0
#define ALG_EDF 1

// Mensajes nuevos que complementan protocol.h
#define MSG_EVENT_RELEASE  0x0A
#define MSG_EVENT_FINISH   0x0B
#define MSG_EXEC_BATCH     0x0C
#define MSG_SPORADIC_SKIP  0x0D
#define MSG_EVENT_START    0x0E
#define MSG_TRIGGER_TASK   0x0F
#define MSG_NODE_CONFIG    0x10

// MSG_CALIB_REQ   -> tick_us[4]
// MSG_CALIB_RESP  -> ut_por_tick[4]
// MSG_NODE_CONFIG -> N_reporte[2] (cada cuantos ticks enviar reporte)
// MSG_TASK_DEPLOY -> alg[1], id[1], tipo[1], W[4], T[4], D[4], criticidad[1] (16 bytes)
// MSG_ACK         -> id[1] para tarea; 0 para inicio/stop/trigger
// MSG_START_SYNC  -> sin payload; comienza tick 0 al recibirlo
// MSG_STOP_REQ    -> sin payload
// MSG_TICK_REPORT -> tick[4], tarea_actual[1] (5 bytes)
// MSG_EVENT_*     -> tick[4], id_tarea[1] (5 bytes)
// MSG_EXEC_BATCH  -> tick_inicial[4], cantidad[1], id_por_tick[cantidad]
// MSG_TRIGGER_TASK -> id_tarea[1] (disparo desde host, si lo aceptan)
// MSG_NACK_ERR    -> id[1] opcional
// id 0 en MSG_EXEC_BATCH significa tick ocioso

static inline void wire_put_u32(uint8_t *p, uint32_t n) {
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
    p[2] = (uint8_t)(n >> 16);
    p[3] = (uint8_t)(n >> 24);
}

static inline uint32_t wire_get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#endif
