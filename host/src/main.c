#define _POSIX_C_SOURCE 200809L
#include "../include/ipc_shm.h"
#include "../include/host_config.h"
#include "../include/planner_bridge.h"
#include "../include/reporter.h"
#include "../include/driver.h"
#include "../../common/include/wire_format.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <termios.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t terminar = 0;
// Permite terminar tambien con Ctrl+C sin cortar la limpieza del host
static void ctrl_c(int signo) {
    (void)signo;
    terminar = 1;
}

// Permitir salir con una sola pulsacion de q en una terminal.
static int preparar_teclado(struct termios *anterior) {
    if (!isatty(STDIN_FILENO)) {
        return 0;
    }
    if (tcgetattr(STDIN_FILENO, anterior) == -1) {
        return 0;
    }
    struct termios t = *anterior;
    t.c_lflag &= ~ICANON;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == -1) {
        return 0;
    }
    return 1;
}

static void guardar(SharedMemory *m, int tipo, int nodo, int tarea, int valor) {
    Event e = {tipo, nodo, tarea, 0, valor};
    if (ipc_send_event(m, &e) == -1) {
        fprintf(stderr, "Cola de eventos llena\n");
    }
}

static int pedir(SharedMemory *m, int nodo, int tipo,
                 const Task *t, const Config *c) {
    Command cmd = {0};
    cmd.type = tipo;
    cmd.tick_us = (uint32_t)c->tick_ms * 1000;
    cmd.report_every = (uint16_t)c->reporte_cada;
    cmd.algorithm = (uint8_t)c->algoritmo;
    if (t) {
        cmd.task = *t;
    }
    int r = ipc_send_command(m, nodo, &cmd);
    if (tipo == CMD_CALIBRAR && r == 0) {
        printf("Nodo %d: %u UT/tick\n", nodo, (unsigned)cmd.capacity);
    }
    return r;
}

static int iniciar_ambos(SharedMemory *m, int activos[2], const Config *c) {
    // Enviamos los dos comandos antes de esperar las respuestas.
    for (int i = 0; i < 2; i++) {
        if (!activos[i]) {
            continue;
        }
        Command cmd = {0};
        cmd.type = CMD_INICIAR;
        cmd.tick_us = (uint32_t)c->tick_ms * 1000;
        cmd.report_every = (uint16_t)c->reporte_cada;
        m->commands[i] = cmd;
    }
    for (int i = 0; i < 2; i++) {
        if (activos[i]) {
            sem_post(&m->command_ready[i]);
        }
    }
    int correcto = 1;
    for (int i = 0; i < 2; i++) {
        if (!activos[i]) {
            continue;
        }
        // Se espera ACK de ambos sin enviar START de forma secuencial.
        struct timespec limite;
        clock_gettime(CLOCK_REALTIME, &limite);
        limite.tv_sec += 5;
        int r;
        do {
            r = sem_timedwait(&m->command_done[i], &limite);
        } while (r == -1 && errno == EINTR);
        if (r == -1 || m->commands[i].result != 0) {
            fprintf(stderr, "Nodo %d no confirmo inicio\n", i + 1);
            activos[i] = 0;
            correcto = 0;
        }
    }
    if (m->start_node_ms[0] && m->start_node_ms[1]) {
        uint64_t x = m->start_node_ms[0], y = m->start_node_ms[1];
        printf("Diferencia entre ACKs de START: %llu ms (no es desfase real de ticks)\n",
               (unsigned long long)(x > y ? x - y : y - x));
    }
    return correcto ? 0 : -1;
}

static void reaccion_falla(SharedMemory *m, const Plan *p, const Config *c,
                           int nodo_caido, int activos[2]) {
    printf("Nodo %d caido. Se avisa al planificador.\n", nodo_caido);
    guardar(m, EVENTO_NODO_CAIDO, nodo_caido, 0, 0);
    activos[nodo_caido - 1] = 0;
    int vivo = nodo_caido == 1 ? 2 : 1;
    Task reubicar[MAX_TAREAS];
    int cantidad = 0;
    uint32_t sn[2] = {m->node_capacity[0], m->node_capacity[1]};
    if (activos[vivo - 1]) {
        cantidad = planner_reubicar(p, sn, nodo_caido, vivo, reubicar, MAX_TAREAS);
    }
    if (cantidad < 0) {
        cantidad = 0;
    }
    if (cantidad > MAX_TAREAS) {
        cantidad = MAX_TAREAS;
    }
    for (int j = 0; j < cantidad; j++) {
        if (reubicar[j].type == 'S') {
            continue; // Esporadicas no se reubican.
        }
        if (pedir(m, vivo, CMD_DESPLEGAR, &reubicar[j], c) != 0) {
            guardar(m, EVENTO_TAREA_RECHAZADA, nodo_caido, reubicar[j].id, 2);
        }
    }
    for (int i = 0; i < p->cantidad; i++) {
        if (p->nodo[i] != nodo_caido) {
            continue;
        }
        int rescatada = 0;
        for (int j = 0; j < cantidad; j++) {
            if (p->tareas[i].id == reubicar[j].id && reubicar[j].type == 'P') {
                rescatada = 1;
            }
        }
        if (!rescatada) {
            guardar(m, EVENTO_TAREA_RECHAZADA, nodo_caido, p->tareas[i].id,
                    p->tareas[i].type == 'S' ? 3 : 1);
        }
    }
}

static void vigilar(SharedMemory *m, const Plan *p, const Config *c, int activos[2]) {
    uint64_t ahora = ipc_now_ms();
    // Cada reporte tiene N ticks y se toleran K reportes perdidos consecutivos.
    uint64_t max_ms = (uint64_t)c->tick_ms * c->reporte_cada * c->reportes_perdidos;
    for (int i = 0; i < 2; i++) {
        if (!activos[i]) {
            continue;
        }
        int conectado;
        uint64_t ultimo, inicio;
        if (ipc_wait(&m->mutex) != 0) {
            return;
        }
        conectado = m->connected[i];
        ultimo = m->last_report_ms[i];
        inicio = m->start_node_ms[i];
        sem_post(&m->mutex);
        uint64_t base = ultimo ? ultimo : inicio;
        if (!conectado || (base && ahora > base && ahora - base >= max_ms)) {
            reaccion_falla(m, p, c, i + 1, activos);
        }
    }
}

static void manual(SharedMemory *m, Plan *p, const Config *c, int activos[2], const char *linea) {
    // Formato: a 8 P 20 40 40 2
    int id, w, t, d, pr;
    char tipo, extra;
    if (sscanf(linea, "a %d %c %d %d %d %d %c", &id, &tipo, &w, &t, &d, &pr, &extra) != 6 ||
        id < 1 || id > 255 || (tipo != 'P' && tipo != 'S') ||
        w <= 0 || t <= 0 || d <= 0 || pr < 0 || pr > 255 || plan_buscar(p, id) >= 0) {
        printf("Entrada invalida. Usa: a ID P/S W T D criticidad\n");
        return;
    }
    Task tarea = {(uint8_t)id, tipo, (uint32_t)w, (uint32_t)t, (uint32_t)d, (uint8_t)pr};
    int nodo = 0;
    uint32_t sn[2] = {m->node_capacity[0], m->node_capacity[1]};
    if (planner_admitir_manual(p, sn, tarea, &nodo) != 0 || nodo < 1 || nodo > 2 || !activos[nodo - 1]) {
        printf("No admitida: no supera la admision o el nodo no esta activo\n");
        guardar(m, EVENTO_TAREA_RECHAZADA, 0, tarea.id, 4);
        return;
    }
    if (pedir(m, nodo, CMD_DESPLEGAR, &tarea, c) != 0) {
        guardar(m, EVENTO_TAREA_RECHAZADA, nodo, tarea.id, 5);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s host/config/host.conf\n", argv[0]);
        return 1;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, ctrl_c);
    Config config;
    Plan plan;
    if (config_leer(argv[1], &config) != 0) {
        fprintf(stderr, "Error de configuracion\n");
        return 1;
    }
    memset(&plan, 0, sizeof(plan));
    printf("Planificador host activo; la conformidad completa requiere validar firmware y hardware\n");
    SharedMemory *m = ipc_create();
    if (!m) {
        fprintf(stderr, "SHM anterior? Ver README para limpiar\n");
        return 1;
    }
    pid_t hijos[3] = {-1, -1, -1};
    int creados = 0;
    // Primero creamos los 2 drivers para calibrar.
    for (int i = 0; i < 2; i++) {
        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            terminar = 1;
            break;
        }
        if (pid == 0) {
            driver_ejecutar(m, i + 1, config.puerto[i]);
            _exit(1);
        }
        hijos[i] = pid;
        creados++;
    }
    int activos[2] = {0, 0};
    if (!terminar) {
        for (int i = 0; i < 2; i++) {
            if (ipc_wait(&m->driver_ready[i]) != 0) {
                terminar = 1;
                break;
            }
            activos[i] = m->connected[i];
        }
    }
    for (int i = 0; i < 2 && !terminar; i++) {
        if (!activos[i] || pedir(m, i + 1, CMD_CALIBRAR, NULL, &config) != 0) {
            fprintf(stderr, "Error en calibracion del nodo %d\n", i + 1);
            terminar = 1;
        }
    }
    // La admision y asignacion requieren las capacidades calibradas de ambos nodos.
    if (!terminar) {
        uint32_t sn[2] = {m->node_capacity[0], m->node_capacity[1]};
        if (planner_preparar(&config, sn, &plan) != 0) {
            fprintf(stderr, "Error de tareas, asignaciones o planificador\n");
            terminar = 1;
        }
    }
    // Crear el reportero una vez que existe el plan; antes del despliegue.
    if (!terminar) {
        pid_t pid = fork();
        if (pid == -1) {
            perror("fork reportero");
            terminar = 1;
        }
        else if (pid == 0) {
            reporter_ejecutar(m, &config, &plan);
        }
        else {
            hijos[2] = pid;
            creados++;
        }
    }
    for (int i = 0; i < 2 && !terminar; i++) {
        if (pedir(m, i + 1, CMD_CONFIGURAR, NULL, &config) != 0) {
            fprintf(stderr, "Error al configurar periodo de reportes del nodo %d\n", i + 1);
            terminar = 1;
        }
    }
    for (int i = 0; i < plan.cantidad && !terminar; i++) {
        if (pedir(m, plan.nodo[i], CMD_DESPLEGAR, &plan.tareas[i], &config) != 0) {
            fprintf(stderr, "Error al desplegar tarea %u\n", plan.tareas[i].id);
            guardar(m, EVENTO_TAREA_RECHAZADA, plan.nodo[i], plan.tareas[i].id, 5);
            terminar = 1;
        }
    }
    if (!terminar) {
        iniciar_ambos(m, activos, &config);
    }
    int error_inicio = terminar ? 1 : 0;

    if (!terminar) {
        printf("Host activo. q para salir; a ID P/S W T D criticidad + Enter para modo manual.\n");
    }
    struct termios teclado_anterior;
    int teclado_modificado = preparar_teclado(&teclado_anterior);
    int hay_stdin = 1;
    char linea[256] = {0};
    size_t indice = 0;
    while (!terminar && (activos[0] || activos[1])) {
        if (hay_stdin) {
            struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
            int r = poll(&pfd, 1, 100);
            if (r > 0 && (pfd.revents & POLLIN)) {
                char tecla;
                ssize_t cantidad = read(STDIN_FILENO, &tecla, 1);
                if (cantidad == 0) {
                    hay_stdin = 0;
                }
                else if (cantidad == 1) {
                    if (tecla == 'q' && indice == 0) {
                        terminar = 1;
                    }
                    else if (tecla == '\n') {
                        linea[indice] = 0;
                        if (linea[0] == 'a') {
                            manual(m, &plan, &config, activos, linea);
                        }
                        indice = 0;
                    } else if (tecla == 127 || tecla == '\b') {
                        if (indice) {
                            indice--;
                        }
                    } else if (indice < sizeof(linea) - 1) {
                        linea[indice++] = tecla;
                    }
                }
            } else if (r > 0 && (pfd.revents & (POLLHUP | POLLNVAL))) {
                hay_stdin = 0;
            }
        } else {
            // No hay teclado (por ejemplo ejecutado como servicio).
            struct pollfd ninguno = {-1, 0, 0};
            poll(&ninguno, 0, 100);
        }
        vigilar(m, &plan, &config, activos);
    }
    if (teclado_modificado) {
        tcsetattr(STDIN_FILENO, TCSANOW, &teclado_anterior);
    }
    // Cierre ordenado: parar, cerrar drivers, esperar y cerrar reportero.
    for (int i = 0; i < 2 && i < creados; i++) {
        if (activos[i]) {
            pedir(m, i + 1, CMD_DETENER, NULL, &config);
        }
    }
    for (int i = 0; i < 2 && i < creados; i++) {
        pedir(m, i + 1, CMD_SALIR, NULL, &config);
    }
    for (int i = 0; i < 2 && i < creados; i++) {
        waitpid(hijos[i], NULL, 0);
    }
    if (creados == 3) {
        Event fin = {EVENTO_FIN, 0, 0, 0, 0};
        // Entregar FIN cuando el reportero sigue vivo.
        int vivo = 1, enviado = 0;
        for (int intentos = 0; intentos < 100; intentos++) {
            int estado;
            pid_t r = waitpid(hijos[2], &estado, WNOHANG);
            if (r == hijos[2] || r == -1) {
                vivo = 0;
                break;
            }
            if (ipc_send_event(m, &fin) == 0) {
                enviado = 1;
                break;
            }
            struct pollfd ninguno = {-1, 0, 0};
            poll(&ninguno, 0, 20);
        }
        if (vivo) {
            if (!enviado) {
                kill(hijos[2], SIGTERM);
            }
            waitpid(hijos[2], NULL, 0);
        }
    }
    printf("Eventos descartados por cola llena: %u\n", m->lost_events);
    ipc_destroy(m);
    printf("Fin del host. Revisa %s/\n", config.carpeta_reporte);
    return error_inicio;
}