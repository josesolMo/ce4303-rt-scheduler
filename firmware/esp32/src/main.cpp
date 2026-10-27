#include <stdio.h>
#include "../../../common/include/protocol.h"
#include <Arduino.h>
#include <esp_timer.h>
#if __has_include(<esp_arduino_version.h>)
  #include <esp_arduino_version.h>
#else
  // Si el core es antiguo, usar la API de timer anterior a Arduino-ESP32 3.x.
  #define ESP_ARDUINO_VERSION_MAJOR 2
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// El encabezado ya declara sus funciones con extern "C" cuando se compila en C++.

extern "C" {
  #include "ut.h"
  #include "rts_engine.h"
}

// ---------------------------------------------------------------------------
// Configuracion
// ---------------------------------------------------------------------------

// ID binario en cuatro LEDs (LSB primero):
// GPIO 32 = bit 0, GPIO 33 = bit 1, GPIO 26 = bit 2, GPIO 25 = bit 3.
const int LED_AZULES[4] = {32, 33, 26, 25};
const int LED_ROJO = 23;
const int BUTTON_PIN = 22;

static const uint32_t TICK_US = 100000;          // 10 ms por tick
static const uint32_t MISS_LED_US = 300000;      // LED rojo: 300 ms
static const uint32_t CALIB_MARGIN = 70;         // 70% del tick para unidad_trabajo()
static const uint32_t CALIB_ROUNDS = 20;
static const uint32_t BUTTON_STABLE_MS = 50;     // antirrebote estable
static const uint32_t BUTTON_MIN_INTERVAL_MS = 200; // 200 ms entre pulsaciones aceptadas

// ---------------------------------------------------------------------------
// Estado global
// ---------------------------------------------------------------------------

static rts_engine_t engine;
static uint32_t tick_us = TICK_US;
static uint32_t ut_per_tick = 0;
static uint32_t miss_led_ticks = 0;
static uint32_t miss_led_off = 0;

// El timer de hardware solo incrementa ticks_hw y despierta loop().
// El motor RTS nunca se ejecuta desde la interrupcion del timer.
static volatile uint32_t ticks_hw = 0;
static uint32_t ticks_done = 0;
static portMUX_TYPE tick_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t loop_task_handle = nullptr;
static hw_timer_t *scheduler_timer = nullptr;

// Estadisticas equivalentes a las del nodo Pico.
static uint32_t max_tick_us = 0;
static uint32_t overruns = 0;
static uint32_t total_misses = 0;

// Antirrebote y limitacion de frecuencia del boton.
static int button_raw_last = HIGH;
static int button_stable_state = HIGH;
static uint32_t button_last_raw_change_ms = 0;
static uint32_t button_last_accepted_ms = 0;
static bool button_has_accepted_press = false;

// ---------------------------------------------------------------------------
// Temporizador periodico del ESP32
// ---------------------------------------------------------------------------

static uint32_t read_ticks_hw(void)
{
    uint32_t result;
    portENTER_CRITICAL(&tick_mux);
    result = ticks_hw;
    portEXIT_CRITICAL(&tick_mux);
    return result;
}

static void IRAM_ATTR scheduler_timer_isr(void)
{
    portENTER_CRITICAL_ISR(&tick_mux);
    ticks_hw++;
    portEXIT_CRITICAL_ISR(&tick_mux);

    // Despertar loop() para que procese los ticks pendientes.
    BaseType_t higher_priority_task_woken = pdFALSE;
    TaskHandle_t handle = loop_task_handle;
    if (handle != nullptr) {
        vTaskNotifyGiveFromISR(handle, &higher_priority_task_woken);
    }
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

// Configura el timer de hardware con un contador de 1 MHz (1 cuenta = 1 us).
// Arduino-ESP32 3.x y 2.x cambiaron la API del timer, por eso se contemplan ambas.
static bool start_scheduler_timer(void)
{
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    scheduler_timer = timerBegin(1000000);
    if (scheduler_timer == nullptr) {
        return false;
    }
    timerAttachInterrupt(scheduler_timer, &scheduler_timer_isr);
    timerAlarm(scheduler_timer, tick_us, true, 0);
#else
    scheduler_timer = timerBegin(0, 80, true); // APB 80 MHz / 80 = 1 MHz
    if (scheduler_timer == nullptr) {
        return false;
    }
    timerAttachInterrupt(scheduler_timer, &scheduler_timer_isr, true);
    timerAlarmWrite(scheduler_timer, tick_us, true);
    timerAlarmEnable(scheduler_timer);
#endif
    return true;
}

// ---------------------------------------------------------------------------
// Calibracion: mide UT por tick y reserva CALIB_MARGIN % para trabajo
// ---------------------------------------------------------------------------

static uint32_t calibrate(uint32_t tick_len_us)
{
    uint32_t worst = UINT32_MAX;

    for (uint32_t r = 0; r < CALIB_ROUNDS; r++) {
        uint32_t count = 0;
        const int64_t start = esp_timer_get_time();

        while ((uint64_t)(esp_timer_get_time() - start) < tick_len_us) {
            unidad_trabajo();
            count++;
        }

        if (count < worst) {
            worst = count;
        }
    }

    const uint32_t result = (worst * CALIB_MARGIN) / 100;
    return (result == 0) ? 1 : result;
}

// ---------------------------------------------------------------------------
// LEDs: representar el ID de la tarea en binario, igual que la Pico
// ---------------------------------------------------------------------------

static void leds_init(void)
{
    for (int i = 0; i < 4; i++) {
        pinMode(LED_AZULES[i], OUTPUT);
        digitalWrite(LED_AZULES[i], LOW);
    }

    pinMode(LED_ROJO, OUTPUT);
    digitalWrite(LED_ROJO, LOW);
}

static void leds_show_task(uint8_t id)
{
    const uint32_t value = (id == RTS_IDLE) ? 0 : (id & 0x0F);

    for (int i = 0; i < 4; i++) {
        digitalWrite(LED_AZULES[i], (value >> i) & 0x01);
    }
}

// ---------------------------------------------------------------------------
// Callback de eventos del motor RTS
// ---------------------------------------------------------------------------

static void on_event(const rts_event_t *ev, void *ctx)
{
    (void)ctx;

    switch (ev->type) {
        case RTS_EV_DEADLINE_MISS:
            Serial.printf("[%lu] PLAZO PERDIDO T%u\n",
                          (unsigned long)ev->tick, ev->task_id);
            digitalWrite(LED_ROJO, HIGH);
            miss_led_off = ev->tick + miss_led_ticks;
            total_misses++;
            break;

        case RTS_EV_SPORADIC_IGNORED:
            Serial.printf("[%lu] boton ignorado T%u\n",
                          (unsigned long)ev->tick, ev->task_id);
            break;

        case RTS_EV_RELEASE:
            if (ev->task_id == 3) {
                Serial.printf("[%lu] Libera esporadica T3\n",
                              (unsigned long)ev->tick);
            }
            break;

        case RTS_EV_COMPLETE:
            if (ev->task_id == 3) {
                Serial.printf("[%lu] Termina T3 (respuesta %lu ticks)\n",
                              (unsigned long)ev->tick,
                              (unsigned long)ev->value);
            }
            break;

        case RTS_EV_SWITCH:
            // Se deja sin imprimir para no añadir carga de Serial en cada cambio.
            break;
    }
}

// ---------------------------------------------------------------------------
// Tareas de prueba: mismos IDs y parámetros que el nodo Raspberry Pi Pico
// ---------------------------------------------------------------------------

static bool load_test_tasks(void)
{
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

    if (rts_add_task(&engine, &a) != 0 ||
        rts_add_task(&engine, &b) != 0 ||
        rts_add_task(&engine, &s) != 0) {
        Serial.println("ERROR: no se pudieron registrar todas las tareas RTS.");
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Boton esporadico
// Se lee desde loop(), nunca desde la interrupcion/temporizador.
// ---------------------------------------------------------------------------

static void check_button(void)
{
    const uint32_t now_ms = millis();
    const int raw = digitalRead(BUTTON_PIN);

    if (raw != button_raw_last) {
        button_raw_last = raw;
        button_last_raw_change_ms = now_ms;
    }

    // Aceptar un cambio de estado solo si permanecio estable 50 ms.
    if ((uint32_t)(now_ms - button_last_raw_change_ms) >= BUTTON_STABLE_MS &&
        raw != button_stable_state) {

        button_stable_state = raw;

        // INPUT_PULLUP: LOW significa pulsado.
        if (button_stable_state == LOW) {
            const bool interval_ok =
                !button_has_accepted_press ||
                (uint32_t)(now_ms - button_last_accepted_ms) >= BUTTON_MIN_INTERVAL_MS;

            if (interval_ok) {
                button_last_accepted_ms = now_ms;
                button_has_accepted_press = true;

                Serial.println("Boton detectado: solicitando tarea esporadica T3");
                rts_sporadic_trigger(&engine);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void setup(void)
{
    Serial.begin(115200);
    delay(2000);

    leds_init();
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    // Secuencia de arranque para comprobar los cuatro LEDs.
    for (int i = 0; i < 4; i++) {
        digitalWrite(LED_AZULES[i], HIGH);
        delay(100);
        digitalWrite(LED_AZULES[i], LOW);
    }

    button_raw_last = digitalRead(BUTTON_PIN);
    button_stable_state = button_raw_last;
    button_last_raw_change_ms = millis();

    Serial.println("Nodo ESP32 - iniciando calibracion...");

    tick_us = TICK_US;
    ut_per_tick = calibrate(tick_us);
    miss_led_ticks = MISS_LED_US / tick_us;
    if (miss_led_ticks == 0) {
        miss_led_ticks = 1;
    }

    Serial.printf("Tick: %lu us -> s_n = %lu UT por tick (con %lu%% de margen)\n",
                  (unsigned long)tick_us,
                  (unsigned long)ut_per_tick,
                  (unsigned long)CALIB_MARGIN);

    rts_init(&engine, RTS_EDF, ut_per_tick, on_event, nullptr);

    if (!load_test_tasks()) {
        while (true) {
            delay(1000);
        }
    }

    rts_start(&engine);

    // Inicializar contador y guardar el handle del task Arduino loop.
    portENTER_CRITICAL(&tick_mux);
    ticks_hw = 0;
    portEXIT_CRITICAL(&tick_mux);
    ticks_done = 0;
    loop_task_handle = xTaskGetCurrentTaskHandle();

    // Timer de hardware independiente de loop(): solo cuenta ticks.
    if (!start_scheduler_timer()) {
        Serial.println("ERROR: no se pudo iniciar el timer de hardware del ESP32.");
        while (true) {
            delay(1000);
        }
    }

    Serial.println("=== Motor RTS iniciado ===");
}

// ---------------------------------------------------------------------------
// Loop: despierta con cada tick y procesa todos los ticks pendientes
// ---------------------------------------------------------------------------

void loop(void)
{
    // Dormir hasta que la ISR del timer notifique un tick.
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    check_button();

    uint32_t hw_snapshot = read_ticks_hw();
    if ((uint32_t)(hw_snapshot - ticks_done) > 1) {
        overruns++;
    }

    // Si el procesamiento se atraso, procesar todos los ticks acumulados.
    while (ticks_done != read_ticks_hw()) {
        const int64_t start_us = esp_timer_get_time();

        const uint8_t id = rts_tick(&engine);
        leds_show_task(id);
        rts_run(&engine);
        ticks_done++;

        const uint32_t elapsed_us = (uint32_t)(esp_timer_get_time() - start_us);
        if (elapsed_us > max_tick_us) {
            max_tick_us = elapsed_us;
        }

        // Apagar el LED rojo al vencer el intervalo establecido.
        if (engine.now >= miss_led_off) {
            digitalWrite(LED_ROJO, LOW);
        }

        // Resumen una vez por segundo, como en la Raspberry Pi Pico.
        const uint32_t ticks_per_second = 1000000UL / tick_us;
        if (ticks_per_second != 0 && engine.now % ticks_per_second == 0) {
            Serial.printf("[%lu] tick max: %lu us (%lu%% del tick)  atrasos: %lu  perdidos: %lu\n",
                          (unsigned long)engine.now,
                          (unsigned long)max_tick_us,
                          (unsigned long)((uint64_t)max_tick_us * 100 / tick_us),
                          (unsigned long)overruns,
                          (unsigned long)total_misses);
            max_tick_us = 0;
        }
    }
}
