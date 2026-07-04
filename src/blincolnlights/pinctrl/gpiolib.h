// gpiolib.h -- Per-GPIO-pin API: function-select, direction, pull, drive, and level for a
// single logical GPIO number, backed by whichever GPIO_CHIP_T (see gpiochip.h) the running
// SoC actually has (BCM2835/2711, BCM2712, or RP1 -- see gpiochip_bcm2835.c/gpiochip_bcm2712.c/
// gpiochip_rp1.c). This is the public surface other project code (e.g. newpanel.c) links
// against directly; identifiers here are kept as-is (not renamed to the project's camelHump
// convention) since they are part of that external contract, not local to this directory.
#ifndef GPIOLIB_H
#define GPIOLIB_H

#include <stdint.h>

#define NUM_HDR_PINS 40
#define MAX_GPIO_PINS 300

// Largest 'count' gpio_set_multi_drive() accepts in one call (audit O4, Phase 5). Generous
// for this project's actual use (the PiDP-1 panel's widest single batch is the 18-pin
// COLUMNS bus); kept as a fixed bound rather than a dynamic allocation since this call sits
// in the panel driver's per-row hot path.
#define GPIO_MULTI_MAX_PINS 64

// Sentinel "GPIO number" values returned by gpio_for_pin() for header pins that are not a
// general-purpose GPIO (ground, a fixed supply rail, or something chip-specific).
#define GPIO_INVALID (~0U)
#define GPIO_GND (~1U)
#define GPIO_5V (~2U)
#define GPIO_3V3 (~3U)
#define GPIO_1V8 (~4U)
#define GPIO_OTHER (~5U)

// Pin function-select value. FUNC0-FUNC8 are the raw, chip-specific alternate-function
// codes (meaning varies by chip); INPUT/OUTPUT/GPIO/NONE are portable requests interpreted
// by each chip's gpio_set_fsel() implementation.
typedef enum
{
    GPIO_FSEL_FUNC0,
    GPIO_FSEL_FUNC1,
    GPIO_FSEL_FUNC2,
    GPIO_FSEL_FUNC3,
    GPIO_FSEL_FUNC4,
    GPIO_FSEL_FUNC5,
    GPIO_FSEL_FUNC6,
    GPIO_FSEL_FUNC7,
    GPIO_FSEL_FUNC8,
    /* ... */
    GPIO_FSEL_INPUT = 0x10,
    GPIO_FSEL_OUTPUT,
    GPIO_FSEL_GPIO, /* Preserves direction if possible, else input */
    GPIO_FSEL_NONE, /* If possible, else input */
    GPIO_FSEL_MAX
} GPIO_FSEL_T;

typedef enum
{
    PULL_NONE,
    PULL_DOWN,
    PULL_UP,
    PULL_MAX
} GPIO_PULL_T;

typedef enum
{
    DIR_INPUT,
    DIR_OUTPUT,
    DIR_MAX,
} GPIO_DIR_T;

typedef enum
{
    DRIVE_LOW,
    DRIVE_HIGH,
    DRIVE_MAX
} GPIO_DRIVE_T;

// Locates and memory-maps the running system's GPIO chip(s) via device-tree probing
// (see util.h/gpiochip.h). Returns 0 on success, nonzero if no supported chip was found or
// the mapping failed.
int gpiolib_init(void);

// Same as gpiolib_init(), but restricted to the single named chip (matched against each
// registered GPIO_CHIP_T's "name" or "compatible" string) instead of auto-probing every
// chip linked into the binary. Returns 0 on success, nonzero on failure.
int gpiolib_init_by_name(const char *name);

// Performs the mmap() of the physical register window(s) resolved by gpiolib_init()/
// gpiolib_init_by_name(); split out from init so callers can inspect chip identity first.
// Returns 0 on success, nonzero if the mapping failed.
int gpiolib_mmap(void);

// Registers a callback invoked with a formatted message string for verbose/diagnostic
// output from the gpiolib/gpiochip layer. No return value. Pass NULL to disable.
void gpiolib_set_verbose(void (*callback)(const char *));

// Returns nonzero if 'gpio' is a valid GPIO number on the currently-probed chip(s), else 0.
int gpio_num_is_valid(unsigned gpio);

// Returns the current direction (DIR_INPUT/DIR_OUTPUT) of 'gpio'.
GPIO_DIR_T gpio_get_dir(unsigned gpio);

// Sets the direction of 'gpio'. No return value.
void gpio_set_dir(unsigned gpio, GPIO_DIR_T dir);

// Returns the current function-select value of 'gpio' (one of the chip's raw FUNCn codes).
GPIO_FSEL_T gpio_get_fsel(unsigned gpio);

// Sets the function-select of 'gpio' to 'func' (a raw FUNCn code, or one of the portable
// INPUT/OUTPUT/GPIO/NONE requests -- see GPIO_FSEL_T above). No return value.
void gpio_set_fsel(unsigned gpio, const GPIO_FSEL_T func);

// Sets the output drive level (DRIVE_LOW/DRIVE_HIGH) of 'gpio'. No return value.
void gpio_set_drive(unsigned gpio, GPIO_DRIVE_T drv);

// Sets the output drive level of 'count' GPIOs at once (audit O4, Phase 5). 'gpios' and
// 'drvs' are parallel arrays of length 'count' ('count' must not exceed
// GPIO_MULTI_MAX_PINS above). Behavior is identical to calling gpio_set_drive() once per
// entry, in order; the only difference callers may rely on is that this can complete in far
// fewer underlying register writes when every entry lands on a chip whose registers support
// batching (see gpio_set_multi_drive in GPIO_CHIP_INTERFACE_T, gpiochip.h). A single call
// may span multiple probed chip instances (e.g. pins split across a "main" GPIO chip and an
// AON chip); each instance touched is still batched separately. 'gpios' is declared as
// "const int *", not "const unsigned *" like every scalar gpio_*() function above -- this
// matches how callers such as newpanel.c already keep their own pin-number arrays (e.g.
// COLUMNS[]/ADDR[]) typed "int", avoiding a cast at every call site for what is always a
// small non-negative pin number. No return value.
void gpio_set_multi_drive(const int *gpios, const GPIO_DRIVE_T *drvs, int count);

// Drives 'gpio' high. No return value. Equivalent to gpio_set_drive(gpio, DRIVE_HIGH).
void gpio_set(unsigned gpio);

// Drives 'gpio' low. No return value. Equivalent to gpio_set_drive(gpio, DRIVE_LOW).
void gpio_clear(unsigned gpio);

int gpio_get_level(unsigned gpio);  /* The actual level observed */
GPIO_DRIVE_T gpio_get_drive(unsigned gpio);  /* What it is being driven as */

// Returns the current pull resistor setting (PULL_NONE/PULL_DOWN/PULL_UP) of 'gpio'.
GPIO_PULL_T gpio_get_pull(unsigned gpio);

// Sets the pull resistor setting of 'gpio'. No return value.
void gpio_set_pull(unsigned gpio, GPIO_PULL_T pull);

// Reports the lowest and highest valid GPIO numbers on the currently-probed chip(s) via
// *first/*last. No return value.
void gpio_get_pin_range(unsigned *first, unsigned *last);

// Maps a 40-pin header position (1-based, 1..NUM_HDR_PINS) to a GPIO number, or one of the
// GPIO_GND/GPIO_5V/GPIO_3V3/GPIO_1V8/GPIO_OTHER sentinels for a non-GPIO pin, or
// GPIO_INVALID if 'pin' is out of range.
unsigned gpio_for_pin(int pin);

// Inverse of gpio_for_pin(): returns the 1-based header pin number that carries 'gpio', or
// -1 if 'gpio' is not present on the header.
int gpio_to_pin(unsigned gpio);

// Looks up a GPIO by its chip-defined name (e.g. from device-tree gpio-line-names),
// comparing at most 'namelen' characters. Returns the GPIO number, or GPIO_INVALID if no
// name matches.
unsigned gpio_get_gpio_by_name(const char *name, int namelen);

// Returns the chip-defined name of 'gpio' (e.g. "GPIO2", or a device-tree gpio-line-names
// entry), or NULL if none is available.
const char *gpio_get_name(unsigned gpio);

// Returns a human-readable name for what function-select value 'fsel' currently means on
// 'gpio' specifically (chip- and sometimes pin-dependent), or NULL if unknown.
const char *gpio_get_gpio_fsel_name(unsigned gpio, GPIO_FSEL_T fsel);

// Returns a human-readable name for function-select value 'fsel' in general (not tied to a
// specific pin), or NULL if unknown.
const char *gpio_get_fsel_name(GPIO_FSEL_T fsel);

// Returns a human-readable name ("none"/"down"/"up") for pull setting 'pull'.
const char *gpio_get_pull_name(GPIO_PULL_T pull);

// Returns a human-readable name ("low"/"high") for drive setting 'drive'.
const char *gpio_get_drive_name(GPIO_DRIVE_T drive);

#endif
