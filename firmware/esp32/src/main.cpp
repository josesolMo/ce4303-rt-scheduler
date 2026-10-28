#include <Arduino.h>
#include <esp_timer.h>
#if __has_include(<esp_arduino_version.h>)
  #include <esp_arduino_version.h>
#else
  #define ESP_ARDUINO_VERSION_MAJOR 2
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

// Mantener estas rutas relativas a la estructura del proyecto.
extern "C" {
  #include "../../../common/include/protocol.h"
  #include "../../../common/include/wire_format.h"
  #include "rts_engine.h"
  #include "ut.h"
}

// -----------------------------------------------------------------------------
// Hardware ESP32
// -----------------------------------------------------------------------------
// Cuatro LEDs representan el ID de tarea en binario (bit 0 primero).
// GPIO32=bit0, GPIO33=bit1, GPIO26=bit2, GPIO25=bit3.
static const int LED_TASK[4] = {32, 33, 26, 25};
static const int LED_MISS = 23;
static const int BUTTON_PIN = 22;  // botón entre GPIO22 y GND; INPUT_PULLUP

static const uint32_t CALIB_MARGIN_PERCENT = 70;
static const uint32_t CALIB_ROUNDS = 20;
static const uint32_t CALIB_WINDOW_US = 10000; // duración máxima de cada medición
static const uint32_t MISS_LED_US = 300000;
static const uint32_t BUTTON_DEBOUNCE_MS = 50;
static const uint32_t DEFAULT_REPORT_EVERY = 50;

// -----------------------------------------------------------------------------
// Tipos de trama acordados con fake_host.py y wire_format.h
// Formato: AA | tipo | longitud(1) | payload | CRC16-Modbus little-endian
// -----------------------------------------------------------------------------
static const uint8_t SOF = 0xAA;
static const uint8_t MSG_CALIB_REQ_LOCAL       = 0x01;
static const uint8_t MSG_CALIB_RESP_LOCAL      = 0x02;
static const uint8_t MSG_TASK_DEPLOY_LOCAL     = 0x03;
static const uint8_t MSG_ACK_LOCAL             = 0x04;
static const uint8_t MSG_START_SYNC_LOCAL      = 0x05;
static const uint8_t MSG_TICK_REPORT_LOCAL     = 0x06;
static const uint8_t MSG_EVENT_SPORADIC_LOCAL  = 0x07;
static const uint8_t MSG_EVENT_DEADLINE_LOCAL  = 0x08;
static const uint8_t MSG_STOP_REQ_LOCAL        = 0x09;
static const uint8_t MSG_EVENT_RELEASE_LOCAL   = 0x0A;
static const uint8_t MSG_EVENT_FINISH_LOCAL    = 0x0B;
static const uint8_t MSG_EXEC_BATCH_LOCAL      = 0x0C;
static const uint8_t MSG_SPORADIC_SKIP_LOCAL  = 0x0D;
static const uint8_t MSG_EVENT_START_LOCAL     = 0x0E;
static const uint8_t MSG_TRIGGER_TASK_LOCAL    = 0x0F;
static const uint8_t MSG_NODE_CONFIG_LOCAL     = 0x10;
static const uint8_t MSG_NACK_LOCAL            = 0xFF;

static const size_t MAX_RX_BUFFER = 512;
static const uint8_t MAX_FRAME_PAYLOAD = 255;
static const uint8_t MAX_BATCH_IDS = 250; // 5 bytes de cabecera + 250 IDs = payload 255
static const uint8_t EVENT_QUEUE_CAPACITY = 64;

// -----------------------------------------------------------------------------
// Estado del nodo
// -----------------------------------------------------------------------------
static rts_engine_t engine;
static bool engine_initialized = false;
static bool calibrated = false;
static bool scheduler_running = false;
static uint32_t tick_us = 10000; // valor inicial; el host lo reemplaza en CALIB_REQ
static uint32_t ut_per_tick = 0;
static uint32_t report_every = DEFAULT_REPORT_EVERY;
static uint32_t miss_led_ticks = 30;
static uint32_t miss_led_off_tick = 0;

// Timer hardware: la ISR solamente contabiliza un tick y notifica loop().
static volatile uint32_t ticks_hw = 0;
static uint32_t ticks_done = 0;
static portMUX_TYPE tick_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t loop_task_handle = nullptr;
static hw_timer_t *scheduler_timer = nullptr;
static volatile bool timer_active = false;

static uint32_t max_tick_us = 0;
static uint32_t overrun_count = 0;
static uint32_t total_misses = 0;
static uint32_t dropped_events = 0;
static uint8_t last_selected_id = RTS_IDLE;

// Parser UART.
static uint8_t rx_buffer[MAX_RX_BUFFER];
static size_t rx_length = 0;

// Interrupción del pulsador: solo marca que ocurrió una arista.
static volatile bool button_irq_pending = false;
static bool button_debounce_pending = false;
static uint32_t button_debounce_started_ms = 0;

// EXEC_BATCH: IDs seleccionados durante ticks consecutivos.
static uint8_t batch_ids[MAX_BATCH_IDS];
static uint8_t batch_count = 0;
static uint32_t batch_start_tick = 0;

// Eventos generados por el motor; se transmiten fuera del callback.
struct queued_event_t {
  uint8_t message_type;
  uint8_t task_id;
  uint32_t tick;
};
static queued_event_t event_queue[EVENT_QUEUE_CAPACITY];
static uint8_t event_head = 0;
static uint8_t event_tail = 0;
static uint8_t event_count = 0;

// -----------------------------------------------------------------------------
// CRC y framing
// -----------------------------------------------------------------------------
static uint16_t crc16_modbus(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) ? (uint16_t)((crc >> 1) ^ 0xA001U)
                       : (uint16_t)(crc >> 1);
    }
  }
  return crc;
}

static void send_frame(uint8_t message_type, const uint8_t *payload, uint8_t length) {
  uint8_t frame[3 + 255 + 2];
  frame[0] = SOF;
  frame[1] = message_type;
  frame[2] = length;
  if (length > 0 && payload != nullptr) {
    memcpy(&frame[3], payload, length);
  }
  const uint16_t crc = crc16_modbus(frame, 3 + length);
  frame[3 + length] = (uint8_t)(crc & 0xFF);
  frame[4 + length] = (uint8_t)(crc >> 8);
  Serial.write(frame, (size_t)(5 + length));
}

static void send_ack(uint8_t id) {
  send_frame(MSG_ACK_LOCAL, &id, 1);
}

static void send_nack(uint8_t id) {
  send_frame(MSG_NACK_LOCAL, &id, 1);
}

static void send_u32_frame(uint8_t message_type, uint32_t value) {
  uint8_t payload[4];
  wire_put_u32(payload, value);
  send_frame(message_type, payload, sizeof(payload));
}

static void send_tick_id_event(uint8_t message_type, uint32_t tick, uint8_t id) {
  uint8_t payload[5];
  wire_put_u32(payload, tick);
  payload[4] = id;
  send_frame(message_type, payload, sizeof(payload));
}

// El protocolo del Gantt reserva ID 0 para un tick ocioso.
static uint8_t wire_task_id(uint8_t id) {
  return (id == RTS_IDLE) ? 0 : id;
}

// -----------------------------------------------------------------------------
// Calibración
// -----------------------------------------------------------------------------
// Cada medición dura como máximo CALIB_WINDOW_US y el resultado se escala al
// tick completo. Así, con ticks largos (por ejemplo 500 ms para ver los LEDs)
// la calibración no tarda más de lo que el host espera la respuesta.
static uint32_t calibrate(uint32_t requested_tick_us) {
  const uint32_t window_us = (requested_tick_us < CALIB_WINDOW_US) ? requested_tick_us
                                                                   : CALIB_WINDOW_US;
  uint32_t worst = UINT32_MAX;
  for (uint32_t round = 0; round < CALIB_ROUNDS; ++round) {
    uint32_t count = 0;
    const int64_t start_us = esp_timer_get_time();
    while ((uint64_t)(esp_timer_get_time() - start_us) < window_us) {
      unidad_trabajo();
      ++count;
    }
    if (count < worst) worst = count;
  }
  uint64_t scaled = (uint64_t)worst * requested_tick_us / window_us;
  scaled = scaled * CALIB_MARGIN_PERCENT / 100U;
  return (scaled == 0) ? 1 : (uint32_t)scaled;
}

// -----------------------------------------------------------------------------
// LEDs
// -----------------------------------------------------------------------------
static void leds_init(void) {
  for (int i = 0; i < 4; ++i) {
    pinMode(LED_TASK[i], OUTPUT);
    digitalWrite(LED_TASK[i], LOW);
  }
  pinMode(LED_MISS, OUTPUT);
  digitalWrite(LED_MISS, LOW);
}

static void leds_show_task(uint8_t id) {
  const uint8_t bits = (id == RTS_IDLE) ? 0 : (uint8_t)(id & 0x0F);
  for (int bit = 0; bit < 4; ++bit) {
    digitalWrite(LED_TASK[bit], (bits >> bit) & 0x01);
  }
}

static void leds_all_off(void) {
  leds_show_task(RTS_IDLE);
  digitalWrite(LED_MISS, LOW);
}

// -----------------------------------------------------------------------------
// Cola de eventos y callback del motor RTS
// -----------------------------------------------------------------------------
static void queue_event(uint8_t message_type, uint32_t tick, uint8_t task_id) {
  if (event_count >= EVENT_QUEUE_CAPACITY) {
    ++dropped_events;
    return;
  }
  event_queue[event_tail].message_type = message_type;
  event_queue[event_tail].tick = tick;
  event_queue[event_tail].task_id = task_id;
  event_tail = (uint8_t)((event_tail + 1U) % EVENT_QUEUE_CAPACITY);
  ++event_count;
}

static bool task_is_sporadic(uint8_t id) {
  if (!engine_initialized) return false;
  for (int i = 0; i < engine.count; ++i) {
    if (engine.tasks[i].info.id == id) {
      return engine.tasks[i].info.type == 'S';
    }
  }
  return false;
}

static void on_event(const rts_event_t *ev, void *ctx) {
  (void)ctx;
  switch (ev->type) {
    case RTS_EV_RELEASE:
      // 0x0A para toda liberación (el host cuenta liberaciones con este) y,
      // si es la esporádica, además 0x07 (el host mide la latencia con este).
      queue_event(MSG_EVENT_RELEASE_LOCAL, ev->tick, ev->task_id);
      if (task_is_sporadic(ev->task_id)) {
        queue_event(MSG_EVENT_SPORADIC_LOCAL, ev->tick, ev->task_id);
      }
      break;

    case RTS_EV_COMPLETE:
      queue_event(MSG_EVENT_FINISH_LOCAL, ev->tick, ev->task_id);
      break;

    case RTS_EV_DEADLINE_MISS:
      queue_event(MSG_EVENT_DEADLINE_LOCAL, ev->tick, ev->task_id);
      digitalWrite(LED_MISS, HIGH);
      miss_led_off_tick = ev->tick + miss_led_ticks;
      ++total_misses;
      break;

    case RTS_EV_SPORADIC_IGNORED:
      queue_event(MSG_SPORADIC_SKIP_LOCAL, ev->tick, ev->task_id);
      break;

    case RTS_EV_SWITCH:
      // EVENT_START indica que comienza a ejecutarse una tarea (no el estado idle).
      if (ev->task_id != RTS_IDLE) {
        queue_event(MSG_EVENT_START_LOCAL, ev->tick, ev->task_id);
      }
      break;
  }
}

static void drain_event_queue(void) {
  while (event_count > 0) {
    const queued_event_t ev = event_queue[event_head];
    event_head = (uint8_t)((event_head + 1U) % EVENT_QUEUE_CAPACITY);
    --event_count;
    send_tick_id_event(ev.message_type, ev.tick, ev.task_id);
  }
}

// -----------------------------------------------------------------------------
// EXEC_BATCH y reportes periódicos
// -----------------------------------------------------------------------------
static void flush_exec_batch(void) {
  if (batch_count == 0) return;

  uint8_t payload[5 + MAX_BATCH_IDS];
  wire_put_u32(payload, batch_start_tick);
  payload[4] = batch_count;
  memcpy(&payload[5], batch_ids, batch_count);
  send_frame(MSG_EXEC_BATCH_LOCAL, payload, (uint8_t)(5 + batch_count));
  batch_count = 0;
}

static void append_exec_id(uint32_t tick_index, uint8_t id) {
  if (batch_count == 0) batch_start_tick = tick_index;
  batch_ids[batch_count++] = wire_task_id(id);
  if (batch_count >= MAX_BATCH_IDS || batch_count >= report_every) {
    flush_exec_batch();
  }
}

// -----------------------------------------------------------------------------
// Temporizador periódico de hardware, compatible con Arduino-ESP32 2.x/3.x
// -----------------------------------------------------------------------------
static uint32_t read_ticks_hw(void) {
  uint32_t result;
  portENTER_CRITICAL(&tick_mux);
  result = ticks_hw;
  portEXIT_CRITICAL(&tick_mux);
  return result;
}

static void IRAM_ATTR scheduler_timer_isr(void) {
  if (!timer_active) return;

  portENTER_CRITICAL_ISR(&tick_mux);
  ++ticks_hw;
  portEXIT_CRITICAL_ISR(&tick_mux);

  BaseType_t higher_priority_task_woken = pdFALSE;
  TaskHandle_t handle = loop_task_handle;
  if (handle != nullptr) {
    vTaskNotifyGiveFromISR(handle, &higher_priority_task_woken);
  }
  if (higher_priority_task_woken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

static bool start_scheduler_timer(void) {
  if (scheduler_timer != nullptr) return true;

  portENTER_CRITICAL(&tick_mux);
  ticks_hw = 0;
  portEXIT_CRITICAL(&tick_mux);
  ticks_done = 0;
  loop_task_handle = xTaskGetCurrentTaskHandle();

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  scheduler_timer = timerBegin(1000000); // 1 MHz: una cuenta por microsegundo
  if (scheduler_timer == nullptr) return false;
  timerAttachInterrupt(scheduler_timer, &scheduler_timer_isr);
  timer_active = true;
  timerAlarm(scheduler_timer, tick_us, true, 0);
#else
  scheduler_timer = timerBegin(0, 80, true); // 80 MHz / 80 = 1 MHz
  if (scheduler_timer == nullptr) return false;
  timerAttachInterrupt(scheduler_timer, &scheduler_timer_isr, true);
  timerAlarmWrite(scheduler_timer, tick_us, true);
  timer_active = true;
  timerAlarmEnable(scheduler_timer);
#endif
  return true;
}

static void stop_scheduler_timer(void) {
  timer_active = false;
  if (scheduler_timer != nullptr) {
    timerEnd(scheduler_timer);
    scheduler_timer = nullptr;
  }
}

// -----------------------------------------------------------------------------
// Protocolo del host: tareas se despliegan antes de START_SYNC.
// -----------------------------------------------------------------------------
static void handle_calib_req(const uint8_t *payload, uint8_t length) {
  if (length != 4 || scheduler_running) {
    send_nack(0);
    return;
  }

  const uint32_t requested_tick_us = wire_get_u32(payload);
  if (requested_tick_us < 1000U || requested_tick_us > 1000000U) {
    send_nack(0);
    return;
  }

  leds_all_off();

  tick_us = requested_tick_us;
  ut_per_tick = calibrate(tick_us);

  // Una calibración nueva empieza desde cero: se descartan las tareas de una
  // ejecución anterior (el host las vuelve a desplegar). Así no hace falta
  // reiniciar la placa entre ejecuciones del host. El motor queda listo
  // aunque no lleguen tareas: un nodo sin tareas igual debe arrancar,
  // porque después le pueden llegar por modo manual o por reubicación.
  // El algoritmo se toma de la primera tarea que llegue.
  rts_init(&engine, RTS_EDF, ut_per_tick, on_event, nullptr);
  engine_initialized = true;
  miss_led_ticks = (MISS_LED_US + tick_us - 1U) / tick_us;
  if (miss_led_ticks == 0) miss_led_ticks = 1;
  calibrated = true;
  send_u32_frame(MSG_CALIB_RESP_LOCAL, ut_per_tick);
}

static void handle_node_config(const uint8_t *payload, uint8_t length) {
  if (length != 2) {
    send_nack(0);
    return;
  }
  const uint16_t requested_report = (uint16_t)payload[0] |
                                    ((uint16_t)payload[1] << 8);
  if (requested_report == 0) {
    send_nack(0);
    return;
  }
  report_every = requested_report;
  send_ack(0);
}

static void handle_task_deploy(const uint8_t *payload, uint8_t length) {
  const uint8_t id = (length >= 2) ? payload[1] : 0;
  // Se aceptan tareas también con el calendarizador corriendo: las usan el
  // modo manual (3.5) y la reubicación cuando se cae el otro nodo (3.8).
  if (!calibrated || length != 16) {
    send_nack(id);
    return;
  }

  const uint8_t algorithm = payload[0];
  const uint8_t task_id = payload[1];
  const char task_type = (char)payload[2];
  const uint32_t work = wire_get_u32(&payload[3]);
  const uint32_t period = wire_get_u32(&payload[7]);
  const uint32_t deadline = wire_get_u32(&payload[11]);
  const uint8_t criticality = payload[15];

  if ((algorithm != ALG_RMS && algorithm != ALG_EDF) ||
      task_id == 0 || task_id == RTS_IDLE ||
      (task_type != 'P' && task_type != 'S') ||
      work == 0 || period == 0 || deadline == 0) {
    send_nack(task_id);
    return;
  }

  const rts_algo_t rts_algorithm = (algorithm == ALG_RMS) ? RTS_RMS : RTS_EDF;
  if (engine.count == 0) {
    // Primera tarea del nodo: define el algoritmo
    engine.algo = rts_algorithm;
  } else if (engine.algo != rts_algorithm) {
    // Todas las tareas de un mismo nodo deben utilizar el algoritmo desplegado.
    send_nack(task_id);
    return;
  }

  Task task = {
    .id = task_id,
    .type = task_type,
    .W = work,
    .T = period,
    .D = deadline,
    .priority = criticality
  };

  if (rts_add_task(&engine, &task) != 0) {
    send_nack(task_id); // por ejemplo, ID repetido o segunda tarea esporádica
    return;
  }
  send_ack(task_id);
}

static bool trigger_sporadic_id(uint8_t requested_id) {
  if (!engine_initialized || !scheduler_running) return false;
  for (int i = 0; i < engine.count; ++i) {
    if (engine.tasks[i].info.id == requested_id && engine.tasks[i].info.type == 'S') {
      rts_sporadic_trigger(&engine);
      return true;
    }
  }
  return false;
}

static void handle_start_sync(uint8_t length) {
  // Se acepta aunque el nodo no tenga tareas (ver handle_calib_req)
  if (length != 0 || !calibrated || !engine_initialized || scheduler_running) {
    send_nack(0);
    return;
  }

  event_head = event_tail = event_count = 0;
  batch_count = 0;
  max_tick_us = 0;
  overrun_count = 0;
  total_misses = 0;
  dropped_events = 0;
  last_selected_id = RTS_IDLE;
  miss_led_off_tick = 0;
  leds_all_off();

  rts_start(&engine); // establece el tick lógico cero
  if (!start_scheduler_timer()) {
    rts_stop(&engine);
    send_nack(0);
    return;
  }
  scheduler_running = true;
  send_ack(0);
}

static void handle_stop_req(uint8_t length) {
  if (length != 0) {
    send_nack(0);
    return;
  }
  if (scheduler_running) {
    stop_scheduler_timer();
    scheduler_running = false;
    flush_exec_batch();
    drain_event_queue();
    rts_stop(&engine);
  }
  leds_all_off();
  send_ack(0);
}

static void handle_frame(uint8_t message_type, const uint8_t *payload, uint8_t length) {
  switch (message_type) {
    case MSG_CALIB_REQ_LOCAL:
      handle_calib_req(payload, length);
      break;

    case MSG_NODE_CONFIG_LOCAL:
      handle_node_config(payload, length);
      break;

    case MSG_TASK_DEPLOY_LOCAL:
      handle_task_deploy(payload, length);
      break;

    case MSG_START_SYNC_LOCAL:
      handle_start_sync(length);
      break;

    case MSG_STOP_REQ_LOCAL:
      handle_stop_req(length);
      break;

    case MSG_TRIGGER_TASK_LOCAL:
      if (length == 1 && trigger_sporadic_id(payload[0])) {
        send_ack(0);
      } else {
        send_nack((length == 1) ? payload[0] : 0);
      }
      break;

    default:
      send_nack(0);
      break;
  }
}

static void parse_rx_buffer(void) {
  while (true) {
    size_t start = 0;
    while (start < rx_length && rx_buffer[start] != SOF) ++start;
    if (start > 0) {
      memmove(rx_buffer, &rx_buffer[start], rx_length - start);
      rx_length -= start;
    }
    if (rx_length < 3) return;

    const uint8_t payload_length = rx_buffer[2];
    const size_t frame_length = 3U + payload_length + 2U;
    if (rx_length < frame_length) return;

    const uint16_t calculated = crc16_modbus(rx_buffer, 3U + payload_length);
    const uint16_t received = (uint16_t)rx_buffer[3U + payload_length] |
                              ((uint16_t)rx_buffer[4U + payload_length] << 8);
    if (calculated != received) {
      send_nack(0);
      // Descarta solo el byte SOF para poder resincronizar si había otro SOF.
      memmove(rx_buffer, &rx_buffer[1], rx_length - 1U);
      --rx_length;
      continue;
    }

    const uint8_t message_type = rx_buffer[1];
    const uint8_t *payload = &rx_buffer[3];
    handle_frame(message_type, payload, payload_length);

    memmove(rx_buffer, &rx_buffer[frame_length], rx_length - frame_length);
    rx_length -= frame_length;
  }
}

static void poll_serial(void) {
  while (Serial.available() > 0) {
    const int value = Serial.read();
    if (value < 0) break;
    if (rx_length < sizeof(rx_buffer)) {
      rx_buffer[rx_length++] = (uint8_t)value;
    } else {
      rx_length = 0;
      send_nack(0);
    }
  }
  parse_rx_buffer();
}

// -----------------------------------------------------------------------------
// Botón físico: ISR mínima, antirrebote y registro fuera de la ISR.
// El intervalo mínimo T de la tarea lo valida el motor RTS.
// -----------------------------------------------------------------------------
static void IRAM_ATTR button_isr(void) {
  // La ISR no calibra, no imprime y no ejecuta el motor: solo marca la arista.
  button_irq_pending = true;
}

static void process_button(void) {
  bool got_irq = false;
  noInterrupts();
  if (button_irq_pending) {
    button_irq_pending = false;
    got_irq = true;
  }
  interrupts();

  if (got_irq) {
    button_debounce_pending = true;
    button_debounce_started_ms = millis();
  }

  if (button_debounce_pending &&
      (uint32_t)(millis() - button_debounce_started_ms) >= BUTTON_DEBOUNCE_MS) {
    button_debounce_pending = false;
    if (digitalRead(BUTTON_PIN) == LOW && scheduler_running && engine_initialized) {
      // Solo marca la solicitud; la liberación real ocurre en rts_tick().
      for (int i = 0; i < engine.count; ++i) {
        if (engine.tasks[i].info.type == 'S') {
          rts_sporadic_trigger(&engine);
          break;
        }
      }
    }
  }
}

// -----------------------------------------------------------------------------
// Procesamiento de ticks del scheduler
// -----------------------------------------------------------------------------
static void process_pending_ticks(void) {
  const uint32_t hw_snapshot = read_ticks_hw();
  if ((uint32_t)(hw_snapshot - ticks_done) > 1U) {
    ++overrun_count;
  }

  while (ticks_done != read_ticks_hw() && scheduler_running) {
    const int64_t start_us = esp_timer_get_time();
    const uint32_t tick_index = engine.now;

    // Apagar al inicio del tick límite, no al final del tick del evento.
    // Esto mantiene el LED visible incluso cuando tick_us > MISS_LED_US.
    if (engine.now >= miss_led_off_tick) {
      digitalWrite(LED_MISS, LOW);
    }

    const uint8_t id = rts_tick(&engine);
    last_selected_id = id;
    leds_show_task(id);
    append_exec_id(tick_index, id);
    rts_run(&engine);
    ++ticks_done;

    drain_event_queue();

    if (report_every > 0 && (engine.now % report_every) == 0) {
      send_tick_id_event(MSG_TICK_REPORT_LOCAL, engine.now, wire_task_id(last_selected_id));
      // Si el lote no se vació automáticamente (por ejemplo, N > 250), cerrar
      // aquí el fragmento restante para que el reporte termine en este tick.
      flush_exec_batch();
    }

    // Incluye planificación, trabajo, encolado/transmisión de eventos y reportes.
    const uint32_t elapsed_us = (uint32_t)(esp_timer_get_time() - start_us);
    if (elapsed_us > max_tick_us) max_tick_us = elapsed_us;
  }
}

void setup(void) {
  // Búfer de salida grande: Serial.write() copia ahí y retorna sin esperar a
  // que salgan los bytes, así el envío no atrasa el tick (seccion 3.6).
  Serial.setTxBufferSize(1024);
  Serial.begin(115200);
  leds_init();
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), button_isr, FALLING);

  // No cargar tareas ni iniciar el timer aquí. El host gobierna la secuencia:
  // CALIB_REQ -> NODE_CONFIG -> TASK_DEPLOY* -> START_SYNC -> STOP_REQ.
  loop_task_handle = xTaskGetCurrentTaskHandle();
}

void loop(void) {
  // Espera acotada: permite recibir comandos aun antes de START_SYNC,
  // sin usar delay() ni ejecutar el trabajo del scheduler fuera de sus ticks.
  (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));

  poll_serial();
  process_button();
  if (scheduler_running) process_pending_ticks();
}