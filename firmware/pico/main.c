

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "rts_engine.h"
#include "ut.h"

// ---------------------------------------------------------------------------
// Configuración
// ---------------------------------------------------------------------------

#define LED_BASE        2       // primer LED de la tarea (GP2)
#define LED_COUNT       4       // GP2..GP5
#define LED_MISS        6       // LED de plazo perdido
#define BTN_PIN         15      // botón de la tarea esporádica

#define DEBOUNCE_US     200000  // 200 ms entre pulsaciones válidas
#define MISS_LED_US     300000  // tiempo que se queda encendido el LED rojo

// Calibración
#define CALIB_ROUNDS    20      // cuántos ticks se miden
#define CALIB_MARGIN    70      // % del tick que se usa para trabajo

// Temporal: en el paso 3 este valor llega en la solicitud de calibración.
#define DEFAULT_TICK_US 10000   // 10 ms

// ---------------------------------------------------------------------------
// Estado global
// ---------------------------------------------------------------------------

static rts_engine_t engine;

static uint32_t tick_us;           // duración del tick
static uint32_t ut_per_tick;       // s_n, sale de la calibración
static uint32_t miss_led_ticks;    // MISS_LED_US convertido a ticks

// La interrupción del timer suma 1 en cada tick. El lazo principal lleva
// su propia cuenta de cuántos ticks ya procesó y compara las dos.
static volatile uint32_t ticks_hw = 0;
static uint32_t ticks_done = 0;

static uint64_t last_press_us = 0;   // para el debounce del botón
static uint32_t miss_led_off = 0;    // tick en que se apaga el LED rojo

// Mediciones para revisar que el trabajo cabe en el tick
static uint32_t max_tick_us = 0;     // lo más que tardó un tick
static uint32_t overruns = 0;        // veces que el lazo se atrasó
static uint32_t total_misses = 0;

// ---------------------------------------------------------------------------
// Calibración
// ---------------------------------------------------------------------------

// Mide cuántas unidad_trabajo() caben en un tick.
// Se mide varias veces y se toma el peor caso (el menor), porque las
// interrupciones del USB pueden robar tiempo en algunas mediciones.
// Al final se deja solo CALIB_MARGIN % para que sobre tiempo para el
// calendarizador, los LEDs y el serial.
static uint32_t calibrate(uint32_t tick_len_us)
{
    uint32_t worst = UINT32_MAX;

    for (int r = 0; r < CALIB_ROUNDS; r++) {
        uint32_t count = 0;
        uint64_t start = time_us_64();

        while (time_us_64() - start < tick_len_us) {
            unidad_trabajo();
            count++;
        }

        if (count < worst) {
            worst = count;
        }
    }

    uint32_t s = (worst * CALIB_MARGIN) / 100;
    return (s == 0) ? 1 : s;
}

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

// Con ticks de 10 ms hay demasiados eventos para imprimirlos todos, así
// que solo se imprimen los importantes. En el paso 4 todos van al host.
static void on_event(const rts_event_t *ev, void *ctx)
{
    (void)ctx;

    switch (ev->type) {
    case RTS_EV_DEADLINE_MISS:
        printf("[%lu] PLAZO PERDIDO T%u\n",
               (unsigned long)ev->tick, ev->task_id);
        gpio_put(LED_MISS, 1);
        miss_led_off = ev->tick + miss_led_ticks;
        total_misses++;
        break;
    case RTS_EV_SPORADIC_IGNORED:
        printf("[%lu] boton ignorado T%u\n",
               (unsigned long)ev->tick, ev->task_id);
        break;
    case RTS_EV_RELEASE:
        if (ev->task_id == 3) {
            printf("[%lu] libera esporadica T3\n", (unsigned long)ev->tick);
        }
        break;
    case RTS_EV_COMPLETE:
        if (ev->task_id == 3) {
            printf("[%lu] termina T3 (respuesta %lu ticks)\n",
                   (unsigned long)ev->tick, (unsigned long)ev->value);
        }
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
    // Las mismas del paso 1, pero ahora W está en UT reales.
    // Multiplicando por s_n, cada tarea sigue costando los mismos ticks
    // (T1 = 2 ticks, T2 = 4 ticks, T3 = 2 ticks).
    Task a = { .id = 1, .type = 'P', .W = 2 * ut_per_tick, .T = 5,  .D = 5,  .priority = 1 };
    Task b = { .id = 2, .type = 'P', .W = 4 * ut_per_tick, .T = 7,  .D = 7,  .priority = 1 };
    Task s = { .id = 3, .type = 'S', .W = 2 * ut_per_tick, .T = 10, .D = 10, .priority = 2 };

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
    // (Solo mientras no hay host, no es parte de la temporización.)
    sleep_ms(3000);
    printf("Nodo Pico - paso 2\n");

    // Calibración
    tick_us = DEFAULT_TICK_US;
    ut_per_tick = calibrate(tick_us);
    miss_led_ticks = MISS_LED_US / tick_us;
    if (miss_led_ticks == 0) {
        miss_led_ticks = 1;
    }
    printf("Tick: %lu us  ->  s_n = %lu UT por tick (con %d%% de margen)\n",
           (unsigned long)tick_us, (unsigned long)ut_per_tick, CALIB_MARGIN);

    rts_init(&engine, RTS_EDF, ut_per_tick, on_event, NULL);
    load_test_tasks();
    rts_start(&engine);

    // Tick por hardware. El negativo hace que el período se mida desde el
    // inicio de cada llamada, así no se acumula atraso.
    repeating_timer_t timer;
    add_repeating_timer_us(-(int64_t)tick_us, on_tick, NULL, &timer);

    uint32_t ticks_per_second = 1000000 / tick_us;

    while (true) {
        // Dormir hasta la próxima interrupción (timer, botón o USB)
        __wfi();

        // Si hay más de un tick pendiente, el anterior tardó demasiado
        if (ticks_hw - ticks_done > 1) {
            overruns++;
        }

        // Procesar todos los ticks pendientes
        while (ticks_done != ticks_hw) {
            uint64_t t0 = time_us_64();

            uint8_t id = rts_tick(&engine);
            leds_show_task(id);
            rts_run(&engine);
            ticks_done++;

            uint32_t elapsed = (uint32_t)(time_us_64() - t0);
            if (elapsed > max_tick_us) {
                max_tick_us = elapsed;
            }

            // Apagar el LED rojo cuando ya pasó su tiempo
            if (engine.now >= miss_led_off) {
                gpio_put(LED_MISS, 0);
            }

            // Resumen cada segundo
            if (engine.now % ticks_per_second == 0) {
                printf("[%lu] tick max: %lu us (%lu%% del tick)  atrasos: %lu  perdidos: %lu\n",
                       (unsigned long)engine.now,
                       (unsigned long)max_tick_us,
                       (unsigned long)(max_tick_us * 100 / tick_us),
                       (unsigned long)overruns,
                       (unsigned long)total_misses);
                max_tick_us = 0;
            }
        }
    }
}