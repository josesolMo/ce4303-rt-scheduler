#include <stdio.h>
#include "../../../common/include/protocol.h"
#include <Arduino.h>

extern "C" {
  #include "rts_engine.h"
  #include "ut.h"
}

// --- PINES ---
const int LED_AZULES[4] = {32, 33, 26, 25};
const int LED_ROJO = 23;
const int BUTTON_PIN = 22;

// --- TIEMPOS ---
static const uint32_t TICK_US = 10000;
static const uint32_t TICK_MS = TICK_US / 1000;

static const int CALIB_MARGIN = 70;
static const uint32_t MISS_LED_TICKS = 30;

// Actualizacion visual: una vez cada 100 ms.
// El scheduler sigue trabajando con su tick de 10 ms.
static const uint32_t LED_DISPLAY_MS = 100;

static rts_engine_t engine;
static uint32_t ut_per_tick = 0;
static uint32_t miss_led_off = 0;

static uint32_t ultimo_tick_ms = 0;
static uint32_t ultima_actualizacion_led_ms = 0;
static uint8_t tarea_para_mostrar = RTS_IDLE;

// --- ANTIRREBOTE ---
static int ultima_lectura_cruda = HIGH;
static int estado_estable_boton = HIGH;
static uint32_t ultimo_cambio_boton_ms = 0;
static const uint32_t TIEMPO_DEBOUNCE_MS = 50;

// --- CALIBRACION ---
static uint32_t calibrate(uint32_t tick_len_us) {
    uint32_t minimo = UINT32_MAX;

    for (int r = 0; r < 20; r++) {
        uint32_t count = 0;
        uint32_t inicio = micros();

        while ((uint32_t)(micros() - inicio) < tick_len_us) {
            unidad_trabajo();
            count++;
        }

        if (count < minimo) {
            minimo = count;
        }
    }

    uint32_t resultado = (minimo * CALIB_MARGIN) / 100;
    return (resultado == 0) ? 1 : resultado;
}

// --- LEDS: UN LED POR ID DE TAREA ---
// T1 -> LED 1
// T2 -> LED 2
// T3 -> LED 3
// T4 -> LED 4
// ID inactivo o desconocido -> todos apagados.
static void leds_show_task(uint8_t id) {
    uint8_t patron = 0;

    switch (id) {
        case 1:
            patron = 0b0001;
            break;

        case 2:
            patron = 0b0010;
            break;

        case 3:
            patron = 0b0100;
            break;

        case 4:
            patron = 0b1000;
            break;

        default:
            patron = 0;
            break;
    }

    for (int i = 0; i < 4; i++) {
        digitalWrite(LED_AZULES[i], (patron >> i) & 0x01);
    }
}

// --- EVENTOS DEL MOTOR RTS ---
static void on_event(const rts_event_t *ev, void *ctx) {
    (void)ctx;

    switch (ev->type) {
        case RTS_EV_DEADLINE_MISS:
            Serial.printf(
                "[%lu] PLAZO PERDIDO T%u\n",
                (unsigned long)ev->tick,
                ev->task_id
            );

            digitalWrite(LED_ROJO, HIGH);
            miss_led_off = ev->tick + MISS_LED_TICKS;
            break;

        case RTS_EV_SPORADIC_IGNORED:
            Serial.printf(
                "[%lu] Solicitud esporadica ignorada T%u\n",
                (unsigned long)ev->tick,
                ev->task_id
            );
            break;

        case RTS_EV_RELEASE:
            if (ev->task_id == 3) {
                Serial.printf(
                    "[%lu] Libera esporadica T3\n",
                    (unsigned long)ev->tick
                );
            }
            break;

        case RTS_EV_COMPLETE:
            if (ev->task_id == 3) {
                Serial.printf(
                    "[%lu] Termina T3 (respuesta %lu ticks)\n",
                    (unsigned long)ev->tick,
                    (unsigned long)ev->value
                );
            }
            break;

        case RTS_EV_SWITCH:
            break;
    }
}

// --- TAREAS DE PRUEBA ---
static void load_test_tasks(void) {
    Task a = {
        .id = 1,
        .type = 'P',
        .W = 2 * ut_per_tick,
        .T = 5,
        .D = 5,
        .priority = 1
    };

    Task b = {
        .id = 2,
        .type = 'P',
        .W = 4 * ut_per_tick,
        .T = 7,
        .D = 7,
        .priority = 1
    };

    Task s = {
        .id = 3,
        .type = 'S',
        .W = 2 * ut_per_tick,
        .T = 10,
        .D = 10,
        .priority = 2
    };

    rts_add_task(&engine, &a);
    rts_add_task(&engine, &b);
    rts_add_task(&engine, &s);
}

// --- LECTURA DEL BOTON CON ANTIRREBOTE ---
static void revisar_boton(void) {
    const uint32_t ahora = millis();
    const int lectura = digitalRead(BUTTON_PIN);

    // Registrar cada cambio de la lectura fisica.
    if (lectura != ultima_lectura_cruda) {
        ultima_lectura_cruda = lectura;
        ultimo_cambio_boton_ms = ahora;
    }

    // Aceptar el cambio solo cuando ha permanecido estable.
    if ((uint32_t)(ahora - ultimo_cambio_boton_ms)
            >= TIEMPO_DEBOUNCE_MS
        && lectura != estado_estable_boton) {

        estado_estable_boton = lectura;

        // INPUT_PULLUP: LOW significa boton presionado.
        if (estado_estable_boton == LOW) {
            Serial.println(
                "Boton detectado: solicitando tarea esporadica T3"
            );

            rts_sporadic_trigger(&engine);
        }
    }
}

void setup(void) {
    Serial.begin(115200);
    delay(2000);

    for (int i = 0; i < 4; i++) {
        pinMode(LED_AZULES[i], OUTPUT);
        digitalWrite(LED_AZULES[i], LOW);
    }

    pinMode(LED_ROJO, OUTPUT);
    digitalWrite(LED_ROJO, LOW);

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    // Inicializar el antirrebote con el estado real del pin.
    ultima_lectura_cruda = digitalRead(BUTTON_PIN);
    estado_estable_boton = ultima_lectura_cruda;
    ultimo_cambio_boton_ms = millis();

    // Prueba individual de los LED.
    for (int i = 0; i < 4; i++) {
        digitalWrite(LED_AZULES[i], HIGH);
        delay(100);
        digitalWrite(LED_AZULES[i], LOW);
    }

    Serial.println("Nodo ESP32: iniciando calibracion...");

    ut_per_tick = calibrate(TICK_US);

    Serial.printf(
        "Tick: %lu us -> %lu UT por tick (%d%% de margen)\n",
        (unsigned long)TICK_US,
        (unsigned long)ut_per_tick,
        CALIB_MARGIN
    );

    rts_init(&engine, RTS_EDF, ut_per_tick, on_event, NULL);
    load_test_tasks();
    rts_start(&engine);

    // Iniciar la base temporal despues de arrancar el motor.
    ultimo_tick_ms = millis();
    ultima_actualizacion_led_ms = ultimo_tick_ms;
    tarea_para_mostrar = RTS_IDLE;

    Serial.println("=== Motor RTS iniciado ===");
    Serial.println("Monitor serial configurado a 115200 baudios.");
}

void loop(void) {
    // El boton se lee en cada iteracion, sin bloquear el scheduler.
    revisar_boton();

    uint32_t ahora = millis();

    // Tick logico del motor: 10 ms.
    if ((uint32_t)(ahora - ultimo_tick_ms) >= TICK_MS) {
        ultimo_tick_ms += TICK_MS;

        uint8_t id = rts_tick(&engine);
        tarea_para_mostrar = id;

        rts_run(&engine);

        // Apagar el LED rojo al terminar su intervalo.
        if ((int32_t)(engine.now - miss_led_off) >= 0) {
            digitalWrite(LED_ROJO, LOW);
        }
    }

    // Actualizar la representacion visual cada 100 ms.
    // Esto no introduce delays en el scheduler.
    ahora = millis();

    if ((uint32_t)(ahora - ultima_actualizacion_led_ms)
            >= LED_DISPLAY_MS) {

        leds_show_task(tarea_para_mostrar);
        ultima_actualizacion_led_ms = ahora;
    }
}
