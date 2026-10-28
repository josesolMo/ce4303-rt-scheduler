/*
 * Nodo de PC: el firmware de la Pico (firmware/pico/main.c y link.c)
 * corriendo en Linux, para probar el host sin las placas.
 *
 * - El USB se reemplaza por un puerto serial virtual (pty).
 * - El timer de hardware se reemplaza por el reloj de la PC.
 * - No hay LEDs ni botón (la esporádica se dispara con TRIGGER_TASK).
 *
 * Uso:
 *   ./pc_node /tmp/ttyNODO
 * y en host.conf poner ese mismo nombre como puerto del nodo.
 */
#define _GNU_SOURCE
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include <pty.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

stdio_driver_t stdio_usb;

static int fd = -1;               // lado "placa" del puerto virtual
static uint64_t t0;               // momento en que arrancó el programa
static const char *link_path = NULL;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}

uint64_t time_us_64(void)
{
    return now_us() - t0;
}

// ---- Timer del tick ----
static repeating_timer_callback_t tick_cb = NULL;
static uint64_t period_us = 0;
static uint64_t next_fire = 0;

bool add_repeating_timer_us(int64_t delay_us, repeating_timer_callback_t cb,
                            void *user, repeating_timer_t *t)
{
    (void)user; (void)t;
    tick_cb = cb;
    period_us = (uint64_t)(delay_us < 0 ? -delay_us : delay_us);
    next_fire = time_us_64() + period_us;
    return true;
}

bool cancel_repeating_timer(repeating_timer_t *t)
{
    (void)t;
    tick_cb = NULL;
    return true;
}

// __wfi(): duerme hasta el próximo tick o hasta que lleguen bytes
void __wfi(void)
{
    int wait_ms = 5;
    if (tick_cb != NULL) {
        uint64_t now = time_us_64();
        if (now >= next_fire) {
            repeating_timer_t t;
            tick_cb(&t);
            next_fire += period_us;
            return;
        }
        uint64_t left_ms = (next_fire - now) / 1000;
        wait_ms = (left_ms < 5) ? (int)left_ms : 5;
    }
    struct pollfd p = { fd, POLLIN, 0 };
    poll(&p, 1, wait_ms);
}

// ---- Serial ----
int getchar_timeout_us(uint32_t timeout_us)
{
    (void)timeout_us;
    unsigned char c;
    return (read(fd, &c, 1) == 1) ? c : -1;
}

int putchar_raw(int c)
{
    unsigned char b = (unsigned char)c;
    if (write(fd, &b, 1) != 1) {
        // si el host no está leyendo, el byte se pierde (como en la placa)
    }
    return c;
}

uint32_t tud_cdc_write_available(void) { return 256; }
bool stdio_usb_connected(void) { return true; }
bool stdio_init_all(void) { return true; }
void stdio_set_translate_crlf(stdio_driver_t *d, bool b) { (void)d; (void)b; }
void gpio_set_irq_enabled_with_callback(uint gpio, uint32_t events, bool en,
                                        gpio_irq_callback_t cb)
{
    (void)gpio; (void)events; (void)en; (void)cb;
}

// ---- Arranque ----
static void cleanup(int sig)
{
    (void)sig;
    if (link_path) {
        unlink(link_path);
    }
    _exit(0);
}

int pico_main(void);   // el main() del firmware, renombrado al compilar

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Uso: %s /tmp/ttyNODO\n", argv[0]);
        return 1;
    }

    int master, slave;
    char name[256];
    if (openpty(&master, &slave, name, NULL, NULL) != 0) {
        perror("openpty");
        return 1;
    }
    struct termios tio;
    tcgetattr(slave, &tio);
    cfmakeraw(&tio);
    tcsetattr(slave, TCSANOW, &tio);

    fd = master;
    fcntl(fd, F_SETFL, O_NONBLOCK);
    t0 = now_us();

    // Enlace con un nombre fijo para poner en host.conf
    link_path = argv[1];
    unlink(link_path);
    if (symlink(name, link_path) != 0) {
        perror("symlink");
        return 1;
    }
    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);

    printf("Nodo de PC escuchando en %s (Ctrl+C para salir)\n", link_path);
    fflush(stdout);
    return pico_main();
}
