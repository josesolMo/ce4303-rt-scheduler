#ifndef LINK_H
#define LINK_H

/*
Comunicación serial con el host por USB.

Recepción: se leen los bytes que haya, sin esperar, y se arman las
tramas byte por byte. Cuando una trama está completa y el CRC está
bien, se llama la función on_frame que le pasa el main.

Envío: las tramas no se mandan directo. Se guardan en un búfer
circular y link_flush() las va mandando según haya espacio en el USB.
Así el calendarizador nunca se queda esperando al USB.
 */

#include <stdint.h>
#include <stdbool.h>

// Función que recibe cada trama válida
typedef void (*link_frame_cb_t)(uint8_t type, const uint8_t *payload, uint8_t len);

void link_init(void);

// Lee los bytes disponibles y procesa las tramas completas
void link_poll(link_frame_cb_t on_frame);

// Arma una trama y la guarda en el búfer de salida.
// Retorna false si no había espacio (la trama se descarta).
bool link_send(uint8_t type, const uint8_t *payload, uint8_t len);

// Manda por USB lo que quepa del búfer de salida, sin esperar
void link_flush(void);

// Contadores para depurar
uint32_t link_bad_frames(void);    // tramas recibidas con error
uint32_t link_dropped(void);       // tramas que no cupieron en el búfer

#endif // LINK_H