// Reemplazo mínimo de pico/stdlib.h para compilar el firmware en la PC.
// Solo declara lo que usan main.c y link.c; la implementación está en pc_stubs.c.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef unsigned int uint;

#define GPIO_OUT 1
#define GPIO_IN 0
#define GPIO_IRQ_EDGE_FALL 4

typedef struct repeating_timer { int unused; } repeating_timer_t;
typedef bool (*repeating_timer_callback_t)(repeating_timer_t *);
typedef void (*gpio_irq_callback_t)(uint, uint32_t);

// En la PC no hay LEDs ni botón: estas funciones no hacen nada
static inline void gpio_init(uint p) { (void)p; }
static inline void gpio_set_dir(uint p, bool o) { (void)p; (void)o; }
static inline void gpio_pull_up(uint p) { (void)p; }
static inline void gpio_put(uint p, bool v) { (void)p; (void)v; }
static inline void gpio_put_masked(uint32_t m, uint32_t v) { (void)m; (void)v; }

void gpio_set_irq_enabled_with_callback(uint gpio, uint32_t events, bool en, gpio_irq_callback_t cb);
uint64_t time_us_64(void);
bool add_repeating_timer_us(int64_t delay_us, repeating_timer_callback_t cb, void *user, repeating_timer_t *t);
bool cancel_repeating_timer(repeating_timer_t *t);
int getchar_timeout_us(uint32_t timeout_us);
int putchar_raw(int c);
bool stdio_init_all(void);
