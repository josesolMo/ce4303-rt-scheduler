#include "link.h"

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "tusb.h"
#include "protocol.h"

#define MAX_FRAME     (3 + 255 + 2)   // encabezado + payload máximo + CRC
#define TX_BUF_SIZE   2048            // búfer circular de salida
#define RX_TIMEOUT_US 100000          // si una trama se queda a medias 100 ms, se descarta

// ---------------------------------------------------------------------------
// Recepción
// ---------------------------------------------------------------------------

// Estados para armar una trama byte por byte
typedef enum {
    RX_WAIT_START,   // esperando 0xAA
    RX_TYPE,
    RX_LEN,
    RX_PAYLOAD,
    RX_CRC_LO,
    RX_CRC_HI
} rx_state_t;

static rx_state_t rx_state = RX_WAIT_START;
static uint8_t    rx_buf[MAX_FRAME];
static uint16_t   rx_pos = 0;          // bytes guardados en rx_buf
static uint8_t    rx_len = 0;          // largo del payload de la trama actual
static uint64_t   rx_last_us = 0;      // cuándo llegó el último byte
static uint32_t   bad_frames = 0;

// ---------------------------------------------------------------------------
// Envío (búfer circular)
// ---------------------------------------------------------------------------

static uint8_t  tx_buf[TX_BUF_SIZE];
static uint16_t tx_head = 0;   // donde se escribe el siguiente byte
static uint16_t tx_tail = 0;   // el siguiente byte que se manda
static uint32_t dropped = 0;


static uint16_t tx_used(void)
{
    return (uint16_t)((tx_head - tx_tail + TX_BUF_SIZE) % TX_BUF_SIZE);
}

static uint16_t tx_free(void)
{
    // Se deja un espacio vacío para distinguir lleno de vacío
    return (uint16_t)(TX_BUF_SIZE - 1 - tx_used());
}

// ---------------------------------------------------------------------------
// Funciones públicas
// ---------------------------------------------------------------------------

void link_init(void)
{
    // Las tramas son binarias: no convertir '\n' en "\r\n"
    stdio_set_translate_crlf(&stdio_usb, false);

    rx_state = RX_WAIT_START;
    rx_pos = 0;
    tx_head = tx_tail = 0;
}

void link_poll(link_frame_cb_t on_frame)
{
    uint64_t now = time_us_64();

    // Si una trama quedó incompleta mucho tiempo, se descarta
    if (rx_state != RX_WAIT_START && now - rx_last_us > RX_TIMEOUT_US) {
        rx_state = RX_WAIT_START;
        bad_frames++;
    }

    // Leer todos los bytes que haya, sin esperar
    while (true) {
        int c = getchar_timeout_us(0);
        if (c < 0) {
            break;      // no hay más bytes por ahora
        }
        uint8_t b = (uint8_t)c;
        rx_last_us = now;

        switch (rx_state) {
        case RX_WAIT_START:
            if (b == START_BYTE) {
                rx_buf[0] = b;
                rx_pos = 1;
                rx_state = RX_TYPE;
            }
            // cualquier otro byte se ignora
            break;

        case RX_TYPE:
            rx_buf[rx_pos++] = b;
            rx_state = RX_LEN;
            break;

        case RX_LEN:
            rx_buf[rx_pos++] = b;
            rx_len = b;
            rx_state = (rx_len > 0) ? RX_PAYLOAD : RX_CRC_LO;
            break;

        case RX_PAYLOAD:
            rx_buf[rx_pos++] = b;
            if (rx_pos == 3 + rx_len) {
                rx_state = RX_CRC_LO;
            }
            break;

        case RX_CRC_LO:
            rx_buf[rx_pos++] = b;
            rx_state = RX_CRC_HI;
            break;

        case RX_CRC_HI:
            rx_buf[rx_pos++] = b;
            rx_state = RX_WAIT_START;

            // Trama completa: revisar CRC
            if (protocol_validate_frame(rx_buf, rx_pos)) {
                on_frame(rx_buf[1], &rx_buf[3], rx_len);
            } else {
                bad_frames++;
                uint8_t id = 0;
                link_send(MSG_NACK_ERR, &id, 1);   // se reporta al host
            }
            break;
        }
    }
}

bool link_send(uint8_t type, const uint8_t *payload, uint8_t len)
{
    uint8_t frame[MAX_FRAME];
    size_t n = protocol_build_frame(type, payload, len, frame);

    if (n == 0 || n > tx_free()) {
        dropped++;
        return false;
    }

    for (size_t i = 0; i < n; i++) {
        tx_buf[tx_head] = frame[i];
        tx_head = (uint16_t)((tx_head + 1) % TX_BUF_SIZE);
    }
    return true;
}

void link_flush(void)
{
    // Sin host conectado no se manda nada. Las tramas se quedan en el
    // búfer y, si se llena, las nuevas se descartan. El calendarizador
    // sigue funcionando igual.
    if (!stdio_usb_connected()) {
        return;
    }

    // Solo se manda lo que quepa ahora en el USB, para no quedarse esperando
    uint32_t space = tud_cdc_write_available();
    while (space > 0 && tx_tail != tx_head) {
        putchar_raw(tx_buf[tx_tail]);
        tx_tail = (uint16_t)((tx_tail + 1) % TX_BUF_SIZE);
        space--;
    }
}

uint32_t link_bad_frames(void)
{
    return bad_frames;
}

uint32_t link_dropped(void)
{
    return dropped;
}