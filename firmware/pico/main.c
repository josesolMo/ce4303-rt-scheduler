/*
 * Nodo Raspberry Pi Pico W
 *
 * PASO 1: calendarizador corriendo en la Pico con tareas fijas.
 * Todavía no hay host: las tareas están escritas aquí abajo y los eventos
 * se imprimen por USB para verlos con screen.
 *
 * Conexiones:
 *   GP2..GP5 -> LEDs con el id de la tarea en binario (bit 0 en GP2)
 *   GP6      -> LED de plazo perdido
 *   GP15     -> botón a GND (pull-up interno)
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "rts_engine.h"

// ---------------------------------------------------------------------------
// Configuración
// ---------------------------------------------------------------------------

#define LED_BASE        2       // primer LED de la tarea (GP2)
#define LED_COUNT       4       // GP2..GP5
#define LED_MISS        6       // LED de plazo perdido
#define BTN_PIN         15      // botón de la tarea esporádica

#define DEBOUNCE_US     200000  // 200 ms entre pulsaciones válidas
#define MISS_LED_TICKS  3       // ticks que se queda encendido el LED rojo

// Para el paso 1 usamos un tick lento (0.5 s) para poder ver los LEDs.
// Con el host, este valor llega en la solicitud de calibración (10 ms).
#define TICK_US         500000

// Temporal: en el paso 2 este valor sale de la calibración.
#define UT_PER_TICK     1

// ---------------------------------------------------------------------------
// Estado global
// ---------------------------------------------------------------------------

static rts_engine_t engine;

// La interrupción del timer suma 1 en cada tick. El lazo principal lleva
// su propia cuenta de cuántos ticks ya procesó y compara las dos.
static volatile uint32_t ticks_hw = 0;
static uint32_t ticks_done = 0;

static uint64_t last_press_us = 0;   // para el debounce del botón
static uint32_t miss_led_off = 0;    // tick en que se apaga el LED rojo

// ---------------------------------------------------------------------------
// Interrupciones
// ---------------------------------------------------------------------------

// Timer de hardware: solo cuenta ticks
static bool on_tick(repeating_timer_t *t)
{
    (void)t;
    ticks_hw++;
    return true;    // true = seguir repitiendo
}

// Botón: solo registra la liberación, el calendarizador hace el resto
static void on_button(uint gpio, uint32_t events)
{
    (void)events;
    if (gpio != BTN_PIN) {
        return;
    }

    uint64_t now = time_us_64();
    if (now - last_press_us < DEBOUNCE_US) {
        return;     // rebote del botón, se ignora
    }
    last_press_us = now;

    rts_sporadic_trigger(&engine);
}

// ---------------------------------------------------------------------------
// LEDs
// ---------------------------------------------------------------------------

static void leds_init(void)
{
    for (int i = 0; i < LED_COUNT; i++) {
        gpio_init(LED_BASE + i);
        gpio_set_dir(LED_BASE + i, GPIO_OUT);
    }
    gpio_init(LED_MISS);
    gpio_set_dir(LED_MISS, GPIO_OUT);
}

// Muestra el id en binario. Si no corre ninguna tarea, todos apagados.
static void leds_show_task(uint8_t id)
{
    uint32_t value = (id == RTS_IDLE) ? 0 : (id & 0x0F);
    uint32_t mask  = ((1u << LED_COUNT) - 1) << LED_BASE;
    gpio_put_masked(mask, value << LED_BASE);
}

// ---------------------------------------------------------------------------
// Eventos del calendarizador
// ---------------------------------------------------------------------------

// Por ahora solo imprime. En el paso 4 esto va a meter los eventos
// en el búfer circular para mandarlos al host.
static void on_event(const rts_event_t *ev, void *ctx)
{
    (void)ctx;

    switch (ev->type) {
    case RTS_EV_RELEASE:
        printf("[%3lu] libera T%u (plazo abs %lu)\n",
               (unsigned long)ev->tick, ev->task_id, (unsigned long)ev->value);
        break;
    case RTS_EV_COMPLETE:
        printf("[%3lu] termina T%u (respuesta %lu ticks)\n",
               (unsigned long)ev->tick, ev->task_id, (unsigned long)ev->value);
        break;
    case RTS_EV_DEADLINE_MISS:
        printf("[%3lu] PLAZO PERDIDO T%u\n",
               (unsigned long)ev->tick, ev->task_id);
        gpio_put(LED_MISS, 1);
        miss_led_off = ev->tick + MISS_LED_TICKS;
        break;
    case RTS_EV_SPORADIC_IGNORED:
        printf("[%3lu] boton ignorado T%u\n",
               (unsigned long)ev->tick, ev->task_id);
        break;
    case RTS_EV_SWITCH:
        break;
    }
}

// ---------------------------------------------------------------------------
// Tareas de prueba (en el paso 3 llegan del host)
// ---------------------------------------------------------------------------

static void load_test_tasks(void)
{
    // Las mismas del caso 1 de test_rts, más una esporádica.
    // Sin la esporádica cabe en EDF; si se presiona el botón seguido,
    // se pasa de 1 y se deberían ver plazos perdidos.
    Task a = { .id = 1, .type = 'P', .W = 2, .T = 5,  .D = 5,  .priority = 1 };
    Task b = { .id = 2, .type = 'P', .W = 4, .T = 7,  .D = 7,  .priority = 1 };
    Task s = { .id = 3, .type = 'S', .W = 2, .T = 10, .D = 10, .priority = 2 };

    rts_add_task(&engine, &a);
    rts_add_task(&engine, &b);
    rts_add_task(&engine, &s);
}

// ---------------------------------------------------------------------------
// Programa principal
// ---------------------------------------------------------------------------

int main(void)
{
    stdio_init_all();
    leds_init();

    // Botón con pull-up: en reposo lee 1, al presionar baja a 0
    gpio_init(BTN_PIN);
    gpio_set_dir(BTN_PIN, GPIO_IN);
    gpio_pull_up(BTN_PIN);
    gpio_set_irq_enabled_with_callback(BTN_PIN, GPIO_IRQ_EDGE_FALL, true, &on_button);

    // Darle tiempo a la computadora de abrir el puerto USB antes de imprimir.
    // (Solo para el paso 1, no es parte de la temporización.)
    sleep_ms(3000);
    printf("Nodo Pico - paso 1\n");

    rts_init(&engine, RTS_EDF, UT_PER_TICK, on_event, NULL);
    load_test_tasks();
    rts_start(&engine);

    // Tick por hardware. El negativo hace que el período se mida desde el
    // inicio de cada llamada, así no se acumula atraso.
    repeating_timer_t timer;
    add_repeating_timer_us(-TICK_US, on_tick, NULL, &timer);

    while (true) {
        // Dormir hasta la próxima interrupción (timer, botón o USB)
        __wfi();

        // Procesar todos los ticks pendientes
        while (ticks_done != ticks_hw) {
            uint8_t id = rts_tick(&engine);
            leds_show_task(id);
            rts_run(&engine);
            ticks_done++;

            // Apagar el LED rojo cuando ya pasó su tiempo
            if (engine.now >= miss_led_off) {
                gpio_put(LED_MISS, 0);
            }
        }
    }
}