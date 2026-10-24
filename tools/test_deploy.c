
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "../host/include/task_deploy.h"
#include "../host/include/serial_driver.h"
#include "../common/include/protocol.h"

// Leer un entero de 4 bytes
uint32_t leer_u32(const uint8_t *datos) {

    return (uint32_t)datos[0] |
           ((uint32_t)datos[1] << 8) |
           ((uint32_t)datos[2] << 16) |
           ((uint32_t)datos[3] << 24);
}

// Simular la recepcion de una tarea
int recibir_tarea(int fd, const Task *esperada) {

    uint8_t tipo;
    uint8_t datos[255];
    uint8_t longitud;

    int resultado = serial_receive(
        fd, &tipo, datos, sizeof(datos), &longitud, 2000
    );

    if (resultado != SERIAL_OK ||
        tipo != MSG_TASK_DEPLOY || longitud != 16) {
        return -1;
    }

    // Comprobar los datos recibidos
    if (datos[0] != EDF ||
        datos[1] != esperada->id ||
        datos[2] != (uint8_t)esperada->type ||
        leer_u32(datos + 3) != esperada->W ||
        leer_u32(datos + 7) != esperada->T ||
        leer_u32(datos + 11) != esperada->D ||
        datos[15] != esperada->priority) {

        return -1;
    }

    // Confirmar que recibimos la tarea
    uint8_t ack[1] = {esperada->id};

    if (serial_send(fd, MSG_ACK, ack, 1) == -1) {
        return -1;
    }

    return 0;
}

int main(void) {

    // Tareas de prueba
    Task tareas[2] = {
        {1, 'P', 40, 20, 20, 2},
        {2, 'P', 15, 30, 30, 1}
    };

    int canal[2];

    // Crear conexion simulada
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

        // Nodo simulado
        close(canal[0]);

        for (int i = 0; i < 2; i++) {

            if (recibir_tarea(canal[1], &tareas[i]) == -1) {
                close(canal[1]);
                _exit(1);
            }
        }

        // Esperar que el host cierre la conexion
        char fin;
        (void)read(canal[1], &fin, 1);

        close(canal[1]);
        _exit(0);
    }

    // Host Linux
    close(canal[1]);

    int correcto = 1;

    for (int i = 0; i < 2; i++) {

        if (deploy_task(canal[0], &tareas[i],
                            EDF) == -1) {

            correcto = 0;
            break;
        }
    }

    close(canal[0]);

    // Esperar al nodo simulado
    int estado;

    if (waitpid(pid, &estado, 0) == -1 ||
        !WIFEXITED(estado) || WEXITSTATUS(estado) != 0) {

        correcto = 0;
    }

    if (correcto) {
        printf("Prueba de despliegue completada\n");
    } else {
        printf("Error en prueba de despliegue\n");
    }

    return correcto ? 0 : 1;
}
