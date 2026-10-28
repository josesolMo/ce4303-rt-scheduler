/*
 * Nodo Raspberry Pi Pico W
 *
 * Recibe los mensajes del host, ejecuta las tareas con el calendarizador
 * local y le manda al host los eventos, la ejecución de cada tick
 * (EXEC_BATCH) y el reporte periódico (TICK_REPORT).
 * El USB solo lleva tramas del protocolo (ver common/include/wire_format.h).
 *
 * Orden normal de mensajes:
 *   CALIB_REQ -> NODE_CONFIG -> TASK_DEPLOY (una por tarea) -> START_SYNC
 *   ... (TRIGGER_TASK, más TASK_DEPLOY en modo manual) ... -> STOP_REQ
 *
 * Conexiones:
 *   GP2..GP5 -> LEDs con el id de la tarea en binario (bit 0 en GP2)
 *   GP6      -> LED de plazo perdido
 *   GP15     -> botón a GND (pull-up interno)
 */

#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "rts_engine.h"
#include "ut.h"
#include "protocol.h"
#include "wire_format.h"
#include "link.h"

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
#define CALIB_WINDOW_US 10000   // duración máxima de cada medición (10 ms)

// Cada cuántos ticks se manda el EXEC_BATCH (máximo 250)
#define BATCH_TICKS     25

// Límites para no aceptar valores absurdos del host
#define TICK_US_MIN     1000      // 1 ms
#define TICK_US_MAX     1000000   // 1 s

// ---------------------------------------------------------------------------
// Estado global
// ---------------------------------------------------------------------------

static rts_engine_t engine;

static uint32_t tick_us = 0;          // duración del tick (llega del host)
static uint32_t ut_per_tick = 0;      // s_n, 0 = todavía no se ha calibrado
static uint32_t miss_led_ticks = 1;   // MISS_LED_US convertido a ticks
static uint16_t report_every = 0;     // cada cuántos ticks se manda TICK_REPORT

// Timer del tick
static repeating_timer_t timer;
static bool timer_on = false;

// La interrupción del timer suma 1 en cada tick. El lazo principal lleva
// su propia cuenta de cuántos ticks ya procesó y compara las dos.
static volatile uint32_t ticks_hw = 0;
static uint32_t ticks_done = 0;

static uint64_t last_press_us = 0;   // para el debounce del botón
static uint32_t miss_led_off = 0;    // tick en que se apaga el LED rojo

// EXEC_BATCH: id de la tarea que corrió en cada tick (0 = ninguna)
static uint8_t  batch_ids[BATCH_TICKS];
static uint8_t  batch_count = 0;
static uint32_t batch_start = 0;     // tick del primer id del lote

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
// Timer del tick
// ---------------------------------------------------------------------------

static void timer_stop(void)
{
    if (timer_on) {
        cancel_repeating_timer(&timer);
        timer_on = false;
    }
}

static void timer_start(void)
{
    timer_stop();
    ticks_hw = 0;
    ticks_done = 0;
    // El negativo hace que el período se mida desde el inicio de cada
    // llamada, así no se acumula atraso.
    add_repeating_timer_us(-(int64_t)tick_us, on_tick, NULL, &timer);
    timer_on = true;
}

// ---------------------------------------------------------------------------
// Calibración
// ---------------------------------------------------------------------------

// Mide cuántas unidad_trabajo() caben en un tick.
// Se mide varias veces y se toma el peor caso (el menor), porque las
// interrupciones del USB pueden robar tiempo en algunas mediciones.
// Al final se deja solo CALIB_MARGIN % para que sobre tiempo para el
// calendarizador, los LEDs y el serial.

// Si el tick es largo (por ejemplo 500 ms para ver los LEDs), medir 20
// ticks completos tardaría demasiado y el host se cansaría de esperar.
// Por eso cada medición dura como máximo CALIB_WINDOW_US y el resultado
// se escala a la duración real del tick.
static uint32_t calibrate(uint32_t tick_len_us)
{
    uint32_t window = (tick_len_us < CALIB_WINDOW_US) ? tick_len_us : CALIB_WINDOW_US;
    uint32_t worst = UINT32_MAX;

    for (int r = 0; r < CALIB_ROUNDS; r++) {
        uint32_t count = 0;
        uint64_t start = time_us_64();

        while (time_us_64() - start < window) {
            unidad_trabajo();
            count++;
        }

        if (count < worst) {
            worst = count;
        }
    }

    // Escalar de la ventana medida al tick completo y aplicar el margen
    uint64_t s = (uint64_t)worst * tick_len_us / window;
    s = s * CALIB_MARGIN / 100;
    return (s == 0) ? 1 : (uint32_t)s;
}

// ---------------------------------------------------------------------------
// Eventos del calendarizador
// ---------------------------------------------------------------------------

// Busca si una tarea es la esporádica
static bool is_sporadic(uint8_t id)
{
    for (int i = 0; i < engine.count; i++) {
        if (engine.tasks[i].info.id == id) {
            return engine.tasks[i].info.type == 'S';
        }
    }
    return false;
}

// Traduce cada evento del calendarizador a su mensaje para el host.
// Todos llevan el mismo payload: tick[4] + id[1].
// No se manda directo: link_send lo deja en el búfer de salida.
static void on_event(const rts_event_t *ev, void *ctx)
{
    (void)ctx;

    uint8_t p[5];
    wire_put_u32(p, ev->tick);
    p[4] = ev->task_id;

    switch (ev->type) {
    case RTS_EV_RELEASE:
        link_send(MSG_EVENT_RELEASE, p, 5);
        // Para la esporádica también se avisa aparte: el host mide la
        // latencia desde aquí hasta que la tarea empieza a correr.
        if (is_sporadic(ev->task_id)) {
            link_send(MSG_EVENT_SPORADIC, p, 5);
        }
        break;

    case RTS_EV_COMPLETE:
        link_send(MSG_EVENT_FINISH, p, 5);
        break;

    case RTS_EV_DEADLINE_MISS:
        link_send(MSG_EVENT_DEADLINE, p, 5);
        gpio_put(LED_MISS, 1);
        miss_led_off = ev->tick + miss_led_ticks;
        break;

    case RTS_EV_SPORADIC_IGNORED:
        link_send(MSG_SPORADIC_SKIP, p, 5);
        break;

    case RTS_EV_SWITCH:
        // No se manda: el host saca los cambios de tarea del EXEC_BATCH
        break;
    }
}

// ---------------------------------------------------------------------------
// Reportes periódicos
// ---------------------------------------------------------------------------

// Manda el lote de ids acumulado: tick_inicial[4] cantidad[1] ids[cantidad]
static void batch_flush(void)
{
    if (batch_count == 0) {
        return;
    }

    uint8_t p[5 + BATCH_TICKS];
    wire_put_u32(p, batch_start);
    p[4] = batch_count;
    for (int i = 0; i < batch_count; i++) {
        p[5 + i] = batch_ids[i];
    }
    link_send(MSG_EXEC_BATCH, p, (uint8_t)(5 + batch_count));

    batch_count = 0;
}

// Guarda qué tarea corrió en este tick. El host usa 0 para "ninguna".
static void batch_add(uint32_t tick, uint8_t id)
{
    if (batch_count == 0) {
        batch_start = tick;
    }
    batch_ids[batch_count++] = (id == RTS_IDLE) ? 0 : id;

    if (batch_count == BATCH_TICKS) {
        batch_flush();
    }
}

// TICK_REPORT: tick[4] + tarea actual[1]. Es el "sigo vivo" del nodo.
static void send_tick_report(uint32_t tick, uint8_t id)
{
    uint8_t p[5];
    wire_put_u32(p, tick);
    p[4] = (id == RTS_IDLE) ? 0 : id;
    link_send(MSG_TICK_REPORT, p, 5);
}

// ---------------------------------------------------------------------------
// Respuestas al host
// ---------------------------------------------------------------------------

static void send_ack(uint8_t id)
{
    link_send(MSG_ACK, &id, 1);
}

static void send_nack(uint8_t id)
{
    link_send(MSG_NACK_ERR, &id, 1);
}

// ---------------------------------------------------------------------------
// Mensajes del host
// ---------------------------------------------------------------------------

// CALIB_REQ: tick_us[4] -> responde CALIB_RESP con s_n[4]
static void handle_calib(const uint8_t *p, uint8_t len)
{
    if (len != 4) {
        send_nack(0);
        return;
    }

    uint32_t t = wire_get_u32(p);
    if (t < TICK_US_MIN || t > TICK_US_MAX) {
        send_nack(0);
        return;
    }

    // Una calibración nueva empieza todo desde cero
    timer_stop();
    leds_show_task(RTS_IDLE);
    gpio_put(LED_MISS, 0);

    tick_us = t;
    ut_per_tick = calibrate(tick_us);
    miss_led_ticks = MISS_LED_US / tick_us;
    if (miss_led_ticks == 0) {
        miss_led_ticks = 1;
    }

    // El algoritmo se cambia cuando llegue la primera tarea
    rts_init(&engine, RTS_EDF, ut_per_tick, on_event, NULL);

    uint8_t resp[4];
    wire_put_u32(resp, ut_per_tick);
    link_send(MSG_CALIB_RESP, resp, 4);
}

// NODE_CONFIG: N_reporte[2] -> ACK 0
static void handle_config(const uint8_t *p, uint8_t len)
{
    if (len != 2) {
        send_nack(0);
        return;
    }

    uint16_t n = (uint16_t)(p[0] | (p[1] << 8));
    if (n == 0) {
        send_nack(0);
        return;
    }

    report_every = n;
    send_ack(0);
}

// TASK_DEPLOY: alg[1] id[1] tipo[1] W[4] T[4] D[4] criticidad[1] -> ACK id
static void handle_deploy(const uint8_t *p, uint8_t len)
{
    if (len != 16) {
        send_nack(0);
        return;
    }

    uint8_t alg = p[0];
    Task t;
    t.id       = p[1];
    t.type     = (char)p[2];
    t.W        = wire_get_u32(p + 3);
    t.T        = wire_get_u32(p + 7);
    t.D        = wire_get_u32(p + 11);
    t.priority = p[15];

    // Sin calibrar no se pueden recibir tareas
    if (ut_per_tick == 0 || (alg != ALG_RMS && alg != ALG_EDF)) {
        send_nack(t.id);
        return;
    }

    // El algoritmo se toma de la primera tarea. Las siguientes tienen
    // que traer el mismo. Tambien vale si ya esta corriendo sin tareas
    // (le llegan por modo manual o por reubicacion).
    rts_algo_t algo = (alg == ALG_RMS) ? RTS_RMS : RTS_EDF;
    if (engine.count == 0) {
        engine.algo = algo;
    } else if (engine.algo != algo) {
        send_nack(t.id);
        return;
    }

    // rts_add_task revisa los datos, ids repetidos, espacio y que haya
    // máximo una esporádica
    if (rts_add_task(&engine, &t) == 0) {
        send_ack(t.id);
    } else {
        send_nack(t.id);
    }
}

// START_SYNC: sin payload -> tick 0 y ACK 0
static void handle_start(uint8_t len)
{
    if (len != 0 || ut_per_tick == 0) {
        send_nack(0);
        return;
    }

    batch_count = 0;
    rts_start(&engine);
    timer_start();
    gpio_put(LED_MISS, 0);
    send_ack(0);
}

// STOP_REQ: sin payload -> detiene y ACK 0
static void handle_stop(void)
{
    timer_stop();
    rts_stop(&engine);
    batch_flush();      // mandar los últimos ticks antes del ACK
    leds_show_task(RTS_IDLE);
    gpio_put(LED_MISS, 0);
    send_ack(0);
}

// TRIGGER_TASK: id[1] -> igual que presionar el botón, ACK 0
static void handle_trigger(const uint8_t *p, uint8_t len)
{
    (void)p;
    if (len != 1) {
        send_nack(0);
        return;
    }
    rts_sporadic_trigger(&engine);
    send_ack(0);
}

// Se llama por cada trama válida que llega
static void on_frame(uint8_t type, const uint8_t *p, uint8_t len)
{
    switch (type) {
    case MSG_CALIB_REQ:    handle_calib(p, len);   break;
    case MSG_NODE_CONFIG:  handle_config(p, len);  break;
    case MSG_TASK_DEPLOY:  handle_deploy(p, len);  break;
    case MSG_START_SYNC:   handle_start(len);      break;
    case MSG_STOP_REQ:     handle_stop();          break;
    case MSG_TRIGGER_TASK: handle_trigger(p, len); break;
    default:               send_nack(0);           break;   // tipo desconocido
    }
}

// ---------------------------------------------------------------------------
// Programa principal
// ---------------------------------------------------------------------------

int main(void)
{
    stdio_init_all();
    link_init();
    leds_init();

    // Botón con pull-up: en reposo lee 1, al presionar baja a 0
    gpio_init(BTN_PIN);
    gpio_set_dir(BTN_PIN, GPIO_IN);
    gpio_pull_up(BTN_PIN);
    gpio_set_irq_enabled_with_callback(BTN_PIN, GPIO_IRQ_EDGE_FALL, true, &on_button);

    // Calendarizador vacío hasta que llegue la calibración
    rts_init(&engine, RTS_EDF, 1, on_event, NULL);

    while (true) {
        // Dormir hasta la próxima interrupción (timer, botón o USB)
        __wfi();

        // 1. Lo prioritario: procesar los ticks pendientes
        while (engine.running && ticks_done != ticks_hw) {
            uint32_t tick = engine.now;     // rts_run lo avanza, se guarda antes

            uint8_t id = rts_tick(&engine);
            leds_show_task(id);
            rts_run(&engine);
            ticks_done++;

            // Reportes para el host
            batch_add(tick, id);
            if (report_every > 0 && (tick + 1) % report_every == 0) {
                send_tick_report(tick, id);
            }

            // Apagar el LED rojo cuando ya pasó su tiempo
            if (engine.now >= miss_led_off) {
                gpio_put(LED_MISS, 0);
            }
        }

        // 2. Mensajes que hayan llegado del host
        link_poll(on_frame);

        // 3. Mandar lo que haya en el búfer de salida
        link_flush();
    }
}