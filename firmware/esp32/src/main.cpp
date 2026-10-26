#include <stdio.h>
#include "../../../common/include/protocol.h"
#include "../../core/rts_engine.h"
#include <Arduino.h>

#define BUTTON_PIN 22  // Pin GPIO D22

void setup() {
    Serial.begin(115200);
    
    // Configurar el pin como entrada utilizando la resistencia de pull-up interna
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    
    Serial.println("=== Nodo ESP32: Prueba de Botón Iniciada ===");
}

void loop() {
    int estadoBoton = digitalRead(BUTTON_PIN);

    // Si el botón NO está presionado, lee HIGH (1)
    // Si el botón SÍ está presionado, lee LOW  (0)
    if (estadoBoton == LOW) {
        Serial.println("Boton de interrupcion!");
        delay(500); // Pausa para mitigar el rebote mecánico
    }
}