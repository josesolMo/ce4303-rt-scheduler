#ifndef TASK_TYPES_H
#define TASK_TYPES_H

#include <stdint.h>

typedef struct {
    uint8_t id;
    char type;         // 'P' (Periódica) o 'S' (Esporádica)
    uint32_t W;        // Trabajo en UTs
    uint32_t T;        // Período en ticks
    uint32_t D;        // Límite/Deadline en ticks
    uint8_t priority;  // Criticidad
} Task;

#endif