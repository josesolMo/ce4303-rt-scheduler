#include <stdio.h>
#include <assert.h>
#include "../common/include/protocol.h"

int main(void) {
    printf("=== PRUEBA DE MÓDULO 'common' (Protocolo + CRC16) ===\n\n");

    uint8_t payload[] = {0x0A, 0x0B, 0x0C, 0x0D}; // Datos simulados
    uint8_t buffer[64];

    // 1. Probar construcción de trama
    size_t frame_len = protocol_build_frame(MSG_TASK_DEPLOY, payload, sizeof(payload), buffer);
    printf("[+] Trama construida con éxito. Longitud total: %zu bytes.\n", frame_len);

    // 2. Probar validación de trama correcta
    int is_valid = protocol_validate_frame(buffer, frame_len);
    printf("[+] Validación de trama original: %s\n", is_valid ? "ÉXITO (1)" : "FALLO (0)");
    assert(is_valid == 1);

    // 3. Probar detección de corrupción (Simular error de puerto serie)
    buffer[4] ^= 0xFF; // Alterar un byte del payload
    int is_corrupted_valid = protocol_validate_frame(buffer, frame_len);
    printf("[+] Validación de trama corrupta: %s\n", !is_corrupted_valid ? "ERROR DETECTADO CORRECTAMENTE (0)" : "FALLO DE DETECCIÓN (1)");
    assert(is_corrupted_valid == 0);

    printf("\nTodas las pruebas del módulo 'common' pasaron con éxito!\n");
    return 0;
}