// Reemplazo mínimo de pico/stdio_usb.h para compilar el firmware en la PC.
#pragma once
#include <stdbool.h>

typedef struct { int unused; } stdio_driver_t;
extern stdio_driver_t stdio_usb;

void stdio_set_translate_crlf(stdio_driver_t *driver, bool translate);
bool stdio_usb_connected(void);
