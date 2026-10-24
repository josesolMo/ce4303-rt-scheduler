
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "../host/include/calibration.h"
#include "../host/include/serial_driver.h"
#include "../host/include/ipc_shm.h"
#include "../common/include/protocol.h"

int main(void) {

    int canal[2];

    // Crear conexion para la prueba
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, canal) == -1) {
        perror("Error en socketpair");
        return 1;
    }

    pid_t pid = fork();

    if (pid == -1) {
        perror("Error en fork");
        close(canal[0]);
        close(canal[1]);
        return 1;
    }

    if (pid == 0) {

        // Proceso que simula el microcontrolador
        close(canal[0]);

        uint8_t tipo;
        uint8_t datos[255];
        uint8_t longitud;

        int resultado = serial_receive(
            canal[1], &tipo, datos, sizeof(datos),
            &longitud, 2000
        );

        if (resultado != SERIAL_OK ||
            tipo != MSG_CALIB_REQ || longitud != 4) {
            _exit(1);
        }

        // Obtener duracion del tick
        uint32_t tick =
            (uint32_t)datos[0] |
            ((uint32_t)datos[1] << 8) |
            ((uint32_t)datos[2] << 16) |
            ((uint32_t)datos[3] << 24);

        if (tick != 10000) {
            _exit(1);
        }

        // Capacidad simulada: 25 UT por tick
        uint8_t respuesta[4] = {25, 0, 0, 0};

        if (serial_send(canal[1], MSG_CALIB_RESP,
                          respuesta, 4) == -1) {
            _exit(1);
        }

        // Esperar a que el host cierre la conexion
        char fin;
        (void)read(canal[1], &fin, 1);

        close(canal[1]);
        _exit(0);
    }

    // Proceso host
    close(canal[1]);

    SharedMemory *mem = ipc_create();
    uint32_t sn = 0;
    uint32_t guardada = 0;
    int correcto = 1;

    if (mem == NULL) {
        correcto = 0;
    }

    // Solicitar calibracion
    if (correcto && node_calibration(canal[0], 10000, &sn) == -1) {
        correcto = 0;
    }

    // Guardar capacidad del nodo 1
    if (correcto && ipc_set_capacity(mem, 1, sn) == -1) {
        correcto = 0;
    }

    // Leerla desde la memoria compartida
    if (correcto && ipc_get_capacity(mem, 1, &guardada) == -1) {
        correcto = 0;
    }

    if (correcto) {
        printf("Capacidad guardada: %u UT/tick\n",
               (unsigned int)guardada);

        printf("Prueba de calibracion completada\n");
    }

    // Liberar recursos
    if (mem != NULL) {
        ipc_destroy(mem);
    }

    close(canal[0]);

    int estado;

    if (waitpid(pid, &estado, 0) == -1 ||
        !WIFEXITED(estado) || WEXITSTATUS(estado) != 0) {
        correcto = 0;
    }

    return correcto ? 0 : 1;
}
