// test_gpio_multi_pi4.c -- standalone real-hardware test for gpio_set_multi_drive() (audit
// O4, Phase 5), meant to be run on an actual Pi 4 (BCM2711). NOT part of the normal build --
// no Makefile target references it; compile and run by hand from this directory:
//
//   make                    (builds the normal gpiochip_*.o/gpiolib.o/util.o objects)
//   cc -I. -o test_gpio_multi_pi4 test_gpio_multi_pi4.c gpiolib.o gpiochip_bcm2835.o
//      gpiochip_bcm2712.o gpiochip_rp1.o util.o
//   sudo ./test_gpio_multi_pi4              (or run as a user in the gpio/gpiomem group --
//                                             gpiolib_mmap() needs /dev/gpiomem or /dev/mem)
//
// Optionally pass a list of GPIO numbers on the command line to test a different pin set;
// with no arguments it defaults to the exact 22 pins the PiDP-1 panel driver uses (COLUMNS[]/
// ADDR[] in src/blincolnlights/panel_pidp1/newpanel.c).
//
// WARNING: this program actively drives real GPIO outputs high and low. Do not run it with
// anything other than the panel (or nothing at all) connected to the pins under test.
//
// What it checks: for several hundred randomized on/off patterns, the test pins are driven
// two ways -- once through the pre-O4 path (gpio_set_drive(), one call per pin) and once
// through the new gpio_set_multi_drive() (one batched call covering every pin) -- and the
// actual observed level of every pin is read back afterward via gpio_get_level() (backed by
// real BCM2711 GPLEV registers, not a simulated one). If the two paths ever disagree, or
// either disagrees with the pattern that was asked for, that's a real hardware bug in the
// batched path. This is the real-silicon half of O4's verification; a sandbox-only
// equivalence test against a fake in-memory register file already confirmed the masking
// arithmetic is correct in the abstract (see TODO.md's O4 entry) -- this confirms the actual
// BCM2711 register writes take effect the same way on real hardware.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gpiolib.h"

#define MAX_TEST_PINS 32
#define NUM_TRIALS 500

// The 18 COLUMNS + 4 ADDR GPIO numbers newpanel.c drives (src/blincolnlights/panel_pidp1/
// newpanel.c's COLUMNS[]/ADDR[] arrays). Kept as a plain copy here rather than an #include of
// newpanel.c/panel_pidp1.h, since this test has no need for the rest of that file's
// dependencies (shared memory, configuration parsing, pthreads).
static int defaultPins[] =
{
    26, 19, 13, 6, 5, 11, 9, 10, 18, 23, 24, 25, 8, 7, 12, 16, 20, 21,  // COLUMNS
    4, 17, 27, 22                                                      // ADDR
};

static unsigned seed = 0xC0FFEEu;

// Cheap deterministic PRNG (statistical quality is irrelevant here, just need varied on/off
// patterns trial to trial). Returns the next 32-bit value in the sequence.
static unsigned
nextRand(void)
{
    seed = seed * 1103515245u + 12345u;
    return(seed);
}

// Entry point. Returns 0 if every trial's readback matched expectations on every test pin,
// 1 if gpiolib/hardware setup failed or at least one mismatch was found.
int
main(int argc, char *argv[])
{
int pins[MAX_TEST_PINS];
GPIO_DRIVE_T drvs[MAX_TEST_PINS];
int stateSingle[MAX_TEST_PINS];
int stateMulti[MAX_TEST_PINS];
int numPins;
int trial, i, fails, expected;
int ngpio;

    numPins = 0;

    if(argc > 1)
    {
        for(i = 1; (i < argc) && (numPins < MAX_TEST_PINS); i++)
        {
            pins[numPins++] = atoi(argv[i]);
        }

        if((argc - 1) > MAX_TEST_PINS)
        {
            printf("Warning: only the first %d of %d pins given were kept.\n",
                MAX_TEST_PINS, argc - 1);
        }
    }
    else
    {
        numPins = (int)(sizeof(defaultPins) / sizeof(defaultPins[0]));
        memcpy(pins, defaultPins, sizeof(defaultPins));
    }

    printf("gpio_set_multi_drive() real-hardware equivalence test\n");
    printf("Testing %d pin(s):", numPins);

    for(i = 0; i < numPins; i++)
    {
        printf(" %d", pins[i]);
    }

    printf("\n");
    printf("This will actively drive these GPIOs high/low. Make sure nothing but the\n");
    printf("panel (or nothing) is connected to them -- a connected panel will flicker,\n");
    printf("that's expected and harmless.\n");
    printf("Starting in 3 seconds... (Ctrl-C now to abort)\n");
    fflush(stdout);
    sleep(3);

    ngpio = gpiolib_init();
    if(ngpio <= 0)
    {
        fprintf(stderr, "gpiolib_init() found no GPIO chip (ngpio=%d) -- wrong hardware, or\n"
            "missing /proc/device-tree access?\n", ngpio);
        return(1);
    }

    if(gpiolib_mmap())
    {
        fprintf(stderr, "gpiolib_mmap() failed -- try running as root (or check\n"
            "/dev/gpiomem permissions).\n");
        return(1);
    }

    for(i = 0; i < numPins; i++)
    {
        gpio_set_fsel(pins[i], GPIO_FSEL_OUTPUT);
    }

    fails = 0;

    for(trial = 0; trial < NUM_TRIALS; trial++)
    {
        for(i = 0; i < numPins; i++)
        {
            drvs[i] = (nextRand() & 1) ? DRIVE_HIGH : DRIVE_LOW;
        }

        // Old path: one gpio_set_drive() call per pin.
        for(i = 0; i < numPins; i++)
        {
            gpio_set_drive(pins[i], drvs[i]);
        }

        for(i = 0; i < numPins; i++)
        {
            stateSingle[i] = gpio_get_level(pins[i]);
        }

        // New path: the same target pattern, one batched call.
        gpio_set_multi_drive(pins, drvs, numPins);

        for(i = 0; i < numPins; i++)
        {
            stateMulti[i] = gpio_get_level(pins[i]);
        }

        for(i = 0; i < numPins; i++)
        {
            expected = (drvs[i] == DRIVE_HIGH) ? 1 : 0;

            if((stateSingle[i] != expected) || (stateMulti[i] != expected))
            {
                fails++;

                if(fails <= 10)
                {
                    printf("MISMATCH trial %d pin %d: expected=%d single=%d multi=%d\n",
                        trial, pins[i], expected, stateSingle[i], stateMulti[i]);
                }
            }
        }
    }

    // Leave every test pin quiet (driven low, then released back to input) before exiting.
    for(i = 0; i < numPins; i++)
    {
        drvs[i] = DRIVE_LOW;
    }

    gpio_set_multi_drive(pins, drvs, numPins);

    for(i = 0; i < numPins; i++)
    {
        gpio_set_fsel(pins[i], GPIO_FSEL_INPUT);
    }

    printf("%d/%d pin-checks mismatched across %d trials\n", fails, numPins * NUM_TRIALS,
        NUM_TRIALS);
    printf(fails ? "FAIL\n" : "PASS\n");

    return(fails != 0);
}
