// gpiochip.h -- Registration interface for a GPIO controller chip implementation
// (gpiochip_bcm2835.c/gpiochip_bcm2712.c/gpiochip_rp1.c). Each chip file uses
// DECLARE_GPIO_CHIP() to place a GPIO_CHIP_T describing itself into the "gpiochips" linker
// section; gpiolib.c (gpiolib_init()) walks that section at runtime via
// __start_gpiochips/__stop_gpiochips to auto-probe every chip linked into the binary
// against the running system's device tree. Identifiers here are kept as-is (public
// contract shared with gpiolib.c and every gpiochip_*.c chip file), not renamed to the
// project's camelHump convention.
#ifndef GPIOCHIP_H
#define GPIOCHIP_H

#include "gpiolib.h"

// Declares and places one GPIO_CHIP_T into the "gpiochips" section. 'name' becomes both the
// C symbol name (name ## _chip) and the chip's displayed name; 'compatible' is the
// device-tree "compatible" string to match against; 'iface' is a pointer to this chip's
// GPIO_CHIP_INTERFACE_T; 'size' is the number of GPIOs the chip provides; 'data' is an
// opaque chip-specific value (e.g. a register base offset) passed back into every
// interface callback via the GPIO_CHIP_T*.
#define DECLARE_GPIO_CHIP(name, compatible, iface, size, data) \
    GPIO_CHIP_T name ## _chip __attribute__ ((section ("gpiochips"))) = \
    { #name, compatible, iface, size, data }

typedef struct GPIO_CHIP_INTERFACE_ GPIO_CHIP_INTERFACE_T;

// One entry in the "gpiochips" linker section: a chip's identity plus a pointer to the
// operations table (interface) that implements it. 'data' is opaque to gpiolib.c -- each
// chip's own interface functions are the only code that interprets it.
typedef struct GPIO_CHIP_
{
    const char *name;
    const char *compatible;
    const GPIO_CHIP_INTERFACE_T *interface;
    int size;
    uintptr_t data;
} GPIO_CHIP_T;

// Operations a GPIO chip implementation must provide. gpiolib.c calls these against
// whichever chip's device-tree "compatible" string matched during probing; every callback
// receives back the opaque 'priv' pointer gpio_create_instance()/gpio_probe_instance()
// returned, so a chip implementation can keep arbitrary per-instance state without
// gpiolib.c needing to know its shape.
struct GPIO_CHIP_INTERFACE_
{
    // Allocates and returns a new per-instance state block for this chip, initialized from
    // device-tree node 'dtnode' (e.g. reading register-window size/count). Returns NULL on
    // failure (chip not actually present, or device-tree data missing/invalid).
    void * (*gpio_create_instance)(const GPIO_CHIP_T *chip, const char *dtnode);

    // Returns the number of GPIOs this chip instance actually provides (may differ from the
    // GPIO_CHIP_T's static 'size' if that varies by SoC variant).
    int (*gpio_count)(void *priv);

    // Completes instance setup once the physical register window has been mmap()'d to
    // 'base'; returns the (possibly updated) 'priv' pointer other callbacks should use
    // afterward, or NULL on failure.
    void * (*gpio_probe_instance)(void *priv, volatile uint32_t *base);

    // Returns the current function-select value of GPIO 'gpio' on this chip instance.
    GPIO_FSEL_T (*gpio_get_fsel)(void *priv, uint32_t gpio);

    // Sets the function-select of GPIO 'gpio' to 'func'. No return value.
    void (*gpio_set_fsel)(void *priv, uint32_t gpio, const GPIO_FSEL_T func);

    // Sets the output drive level of GPIO 'gpio'. No return value.
    void (*gpio_set_drive)(void *priv, uint32_t gpio, GPIO_DRIVE_T drv);

    // Sets the direction of GPIO 'gpio'. No return value.
    void (*gpio_set_dir)(void *priv, uint32_t gpio, GPIO_DIR_T dir);

    // Returns the current direction of GPIO 'gpio'.
    GPIO_DIR_T (*gpio_get_dir)(void *priv, uint32_t gpio);

    int (*gpio_get_level)(void *priv, uint32_t gpio);  /* The actual level observed */
    GPIO_DRIVE_T (*gpio_get_drive)(void *priv, uint32_t gpio);  /* What it is being driven as */

    // Returns the current pull resistor setting of GPIO 'gpio'.
    GPIO_PULL_T (*gpio_get_pull)(void *priv, uint32_t gpio);

    // Sets the pull resistor setting of GPIO 'gpio'. No return value.
    void (*gpio_set_pull)(void *priv, uint32_t gpio, GPIO_PULL_T pull);

    // Returns a human-readable name for GPIO 'gpio' on this chip, or NULL if none.
    const char * (*gpio_get_name)(void *priv, uint32_t gpio);

    // Returns a human-readable name for what function-select value 'fsel' means on this
    // specific GPIO/chip combination, or NULL if unknown.
    const char * (*gpio_get_fsel_name)(void *priv, uint32_t gpio, GPIO_FSEL_T fsel);
};

// Linker-provided bounds of the "gpiochips" section: every GPIO_CHIP_T placed there by
// DECLARE_GPIO_CHIP() in any linked-in gpiochip_*.c file lies between these two symbols,
// which is how gpiolib.c enumerates "every chip implementation this binary was built with"
// without a separate registration list to keep in sync.
extern const GPIO_CHIP_T __start_gpiochips;
extern const GPIO_CHIP_T __stop_gpiochips;

#endif
