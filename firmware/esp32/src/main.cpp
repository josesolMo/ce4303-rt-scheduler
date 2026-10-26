#include <stdio.h>
#include "../../../common/include/protocol.h"
#include "../../core/rts_engine.h"
#include <Arduino.h>

// --- MAPEO DE PINES ---
// 4 LEDs Azules para tareas periódicas / estados
const int LED_AZUL_1 = 32;
const int LED_AZUL_2 = 33;
const int LED_AZUL_3 = 26;
const int LED_AZUL_4 = 25; 

// LED Rojo para interrupciones / eventos esporádicos
const int LED_ROJO = 23;

// Botón para simular eventos de interrupción
const int BUTTON_PIN = 22;

// Variables para control de tiempo y antirrebote (debounce) simple
unsigned long ultimoDebounce = 0;
const unsigned long tiempoDebounce = 50; 
int ultimoEstadoBoton = HIGH;

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n=== INICIANDO BANCO DE PRUEBAS DE HARDWARE (ESP32) ===");

    // Configurar pines de los LEDs azules como Salidas
    pinMode(LED_AZUL_1, OUTPUT);
    pinMode(LED_AZUL_2, OUTPUT);
    pinMode(LED_AZUL_3, OUTPUT);
    pinMode(LED_AZUL_4, OUTPUT);

    // Configurar pin del LED rojo como Salida
    pinMode(LED_ROJO, OUTPUT);

    // Configurar pin del botón como Entrada con Pull-Up interno
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    Serial.println("¡Hardware configurado correctamente! Probando secuencia inicial de LEDs...");
    
    // Pequeña secuencia de bienvenida visual en los LEDs
    digitalWrite(LED_ROJO, HIGH);
    delay(200);
    digitalWrite(LED_AZUL_1, HIGH); delay(150);
    digitalWrite(LED_AZUL_2, HIGH); delay(150);
    digitalWrite(LED_AZUL_3, HIGH); delay(150);
    digitalWrite(LED_AZUL_4, HIGH); delay(150);
    
    // Apagar todos
    digitalWrite(LED_ROJO, LOW);
    digitalWrite(LED_AZUL_1, LOW);
    digitalWrite(LED_AZUL_2, LOW);
    digitalWrite(LED_AZUL_3, LOW);
    digitalWrite(LED_AZUL_4, LOW);
    
    Serial.println("Sistema listo. Presiona el botón (D22) para encender el LED rojo (D23).");
}

void loop() {
    // 1. LECTURA DEL BOTÓN CON DEBOUNCE BÁSICO
    int lecturaActual = digitalRead(BUTTON_PIN);

    // Si el estado del botón cambió (debido a ruido o pulsación real)
    if (lecturaActual != ultimoEstadoBoton) {
        ultimoDebounce = millis();
    }

    // Si ha pasado el tiempo suficiente para confirmar la lectura estable
    if ((millis() - ultimoDebounce) > tiempoDebounce) {
        // LOW significa que el botón fue presionado
        if (lecturaActual == LOW) {
            Serial.println("[EVENTO] Boton presionado en GPIO 22! Activando LED Rojo (Interrupcion).");
            
            // LED rojo para simular atención a interrupción
            digitalWrite(LED_ROJO, HIGH);
            
            // LEDs azules para verificar mapeo
            digitalWrite(LED_AZUL_1, HIGH);
            delay(100);
            digitalWrite(LED_AZUL_2, HIGH);
            delay(100);
            digitalWrite(LED_AZUL_3, HIGH);
            delay(100);
            digitalWrite(LED_AZUL_4, HIGH);
            delay(200);
            
            // Apagar todo de vuelta
            digitalWrite(LED_ROJO, LOW);
            digitalWrite(LED_AZUL_1, LOW);
            digitalWrite(LED_AZUL_2, LOW);
            digitalWrite(LED_AZUL_3, LOW);
            digitalWrite(LED_AZUL_4, LOW);
            
            Serial.println("[ESTADO] Sistema restablecido a reposo.");
        }
    }
    ultimoEstadoBoton = lecturaActual;

    // 2. CICLO DEMOSTRATIVO DE TAREAS PERÍODICAS (Simulación)
    static unsigned long tiempoAnterior = 0;
    if (millis() - tiempoAnterior >= 2000) { // Cada 2 segundos
        tiempoAnterior = millis();
        Serial.println("[TICK] Nodo activo. Latido de vida enviado.");
    }
}