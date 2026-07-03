// gpiolib.c -- Implementation of the per-GPIO-pin API declared in gpiolib.h. Locates every
// GPIO_CHIP_T linked into the binary (see gpiochip.h) via device-tree probing, builds a flat
// GPIO-number space across however many chips are found (chip 0 at GPIO 0, chip 1 starting
// at the next round-hundred boundary, etc.), and dispatches every gpio_*() call to whichever
// chip instance owns that GPIO number. Function names declared in gpiolib.h are kept as-is
// (external callers, e.g. newpanel.c, depend on them); everything else here (statics, file-
// scope globals, and the locally-defined GPIO_CHIP_INSTANCE_T) is local to this file and
// follows the project's camelHump + pointer-P-suffix convention.

#define _FILE_OFFSET_BITS 64
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "gpiochip.h"
#include "util.h"

#define ARRAY_SIZE(_a) (sizeof(_a) / sizeof(_a[0]))

#define MAX_GPIO_CHIPS 8

// One probed-and-instantiated GPIO chip: which GPIO_CHIP_T it is, its device-tree node and
// physical register address, the /dev/gpiomemN fd (if any) used to map it, and the flat
// GPIO-number range [base, base+numGpios) it occupies in this process's GPIO space.
typedef struct GPIO_CHIP_INSTANCE_
{
    const GPIO_CHIP_T *chipP;
    const char *nameP;
    const char *dtnodeP;
    int memFd;
    void *privP;
    uint64_t physAddr;
    unsigned numGpios;
    uint32_t base;
} GPIO_CHIP_INSTANCE_T;

static unsigned numGpioChips;
static GPIO_CHIP_INSTANCE_T gpioChips[MAX_GPIO_CHIPS];

static unsigned numGpios;
static unsigned firstHdrPin = GPIO_INVALID;
static unsigned lastHdrPin = GPIO_INVALID;
static const char *gpioNames[MAX_GPIO_PINS];
static unsigned hdrGpios[NUM_HDR_PINS + 1];

const char *pull_names[] = { "pn", "pd", "pu", "--" };
const char *drive_names[] = { "dl", "dh", "--" };
const char *fsel_names[] =
{
    "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
    "a8", "??", "??", "??", "??", "??", "??", "??",
    "ip", "op", "gp", "no"
};

void (*verbose_callback)(const char *);

// Instantiates chip (calling its gpio_create_instance() interface method with dtnodeP) and
// records it in the gpioChips[] table at slot numGpioChips. Returns a pointer to the new
// slot, or NULL if the table is already full (assert(0)'d -- MAX_GPIO_CHIPS should always be
// generous enough not to hit this on real hardware) or the chip's own instantiation failed.
static GPIO_CHIP_INSTANCE_T *
gpioCreateInstance(const GPIO_CHIP_T *chipP, uint64_t physAddr, const char *nameP,
    const char *dtnodeP)
{
GPIO_CHIP_INSTANCE_T *instP;

    if( numGpioChips >= MAX_GPIO_CHIPS )
    {
        assert(0);
        return(NULL);
    }

    instP = &gpioChips[numGpioChips];

    instP->chipP = chipP;
    instP->nameP = nameP ? nameP : chipP->name;
    instP->dtnodeP = dtnodeP;
    instP->physAddr = physAddr;
    instP->privP = NULL;
    instP->base = 0;

    instP->privP = chipP->interface->gpio_create_instance(chipP, dtnodeP);
    if( !instP->privP )
    {
        return(NULL);
    }

    numGpioChips++;

    return(instP);
}

// Finds which probed chip instance owns GPIO number 'gpio' and resolves the interface/priv
// pointer plus per-chip offset a gpio_*() call needs to dispatch to it. Returns 0 and fills
// in *ifacePP/*privPP/*offsetP on success, -1 if 'gpio' is not owned by any probed chip
// (*ifacePP is left NULL in that case too, so callers can check either return value).
static int
gpioGetInterface(unsigned gpio, const GPIO_CHIP_INTERFACE_T **ifacePP, void **privPP,
    unsigned *offsetP)
{
unsigned i;
GPIO_CHIP_INSTANCE_T *instP;
const GPIO_CHIP_T *chipP;

    *ifacePP = NULL;

    for( i = 0; i < numGpioChips; i++ )
    {
        instP = &gpioChips[i];
        chipP = instP->chipP;

        if( (gpio >= instP->base) && (gpio < (instP->base + instP->numGpios)) )
        {
            *ifacePP = chipP->interface;
            *privPP = instP->privP;
            *offsetP = gpio - instP->base;
            return(0);
        }
    }

    return(-1);
}

// See gpiolib.h.
int
gpio_num_is_valid(unsigned gpio)
{
    return( (gpio < MAX_GPIO_PINS) && !!gpioNames[gpio] );
}

// See gpiolib.h.
GPIO_DIR_T
gpio_get_dir(unsigned gpio)
{
const GPIO_CHIP_INTERFACE_T *ifaceP;
unsigned gpioOffset;
void *privP;

    ifaceP = NULL;

    if( gpioGetInterface(gpio, &ifaceP, &privP, &gpioOffset) == 0 )
    {
        return( ifaceP->gpio_get_dir(privP, gpioOffset) );
    }

    return(DIR_MAX);
}

// See gpiolib.h. No return value.
void
gpio_set_dir(unsigned gpio, GPIO_DIR_T dir)
{
const GPIO_CHIP_INTERFACE_T *ifaceP;
unsigned gpioOffset;
void *privP;

    ifaceP = NULL;

    if( gpioGetInterface(gpio, &ifaceP, &privP, &gpioOffset) == 0 )
    {
        ifaceP->gpio_set_dir(privP, gpioOffset, dir);
    }
}

// See gpiolib.h.
GPIO_FSEL_T
gpio_get_fsel(unsigned gpio)
{
const GPIO_CHIP_INTERFACE_T *ifaceP;
GPIO_FSEL_T fsel;
unsigned gpioOffset;
void *privP;

    ifaceP = NULL;
    fsel = GPIO_FSEL_MAX;

    if( gpioGetInterface(gpio, &ifaceP, &privP, &gpioOffset) == 0 )
    {
        fsel = ifaceP->gpio_get_fsel(privP, gpioOffset);
    }

    if( fsel == GPIO_FSEL_GPIO )
    {
        if( gpio_get_dir(gpio) == DIR_OUTPUT )
        {
            fsel = GPIO_FSEL_OUTPUT;
        }
        else
        {
            fsel = GPIO_FSEL_INPUT;
        }
    }

    return(fsel);
}

// See gpiolib.h. No return value.
void
gpio_set_fsel(unsigned gpio, const GPIO_FSEL_T func)
{
const GPIO_CHIP_INTERFACE_T *ifaceP;
unsigned gpioOffset;
void *privP;

    ifaceP = NULL;

    if( gpioGetInterface(gpio, &ifaceP, &privP, &gpioOffset) == 0 )
    {
        ifaceP->gpio_set_fsel(privP, gpioOffset, func);
    }
}

// See gpiolib.h. No return value.
void
gpio_set_drive(unsigned gpio, GPIO_DRIVE_T drv)
{
const GPIO_CHIP_INTERFACE_T *ifaceP;
unsigned gpioOffset;
void *privP;

    ifaceP = NULL;

    if( gpioGetInterface(gpio, &ifaceP, &privP, &gpioOffset) == 0 )
    {
        ifaceP->gpio_set_drive(privP, gpioOffset, drv);
    }
}

// See gpiolib.h. No return value.
void
gpio_set(unsigned gpio)
{
const GPIO_CHIP_INTERFACE_T *ifaceP;
unsigned gpioOffset;
void *privP;

    ifaceP = NULL;

    if( gpioGetInterface(gpio, &ifaceP, &privP, &gpioOffset) == 0 )
    {
        ifaceP->gpio_set_drive(privP, gpioOffset, 1);
        ifaceP->gpio_set_dir(privP, gpioOffset, DIR_OUTPUT);
    }
}

// See gpiolib.h. No return value.
void
gpio_clear(unsigned gpio)
{
const GPIO_CHIP_INTERFACE_T *ifaceP;
unsigned gpioOffset;
void *privP;

    ifaceP = NULL;

    if( gpioGetInterface(gpio, &ifaceP, &privP, &gpioOffset) == 0 )
    {
        ifaceP->gpio_set_drive(privP, gpioOffset, 0);
        ifaceP->gpio_set_dir(privP, gpioOffset, DIR_OUTPUT);
    }
}

// See gpiolib.h.
int
gpio_get_level(unsigned gpio)
{
const GPIO_CHIP_INTERFACE_T *ifaceP;
unsigned gpioOffset;
void *privP;

    ifaceP = NULL;

    if( gpioGetInterface(gpio, &ifaceP, &privP, &gpioOffset) == 0 )
    {
        return( ifaceP->gpio_get_level(privP, gpioOffset) );
    }

    return(0);
}

// See gpiolib.h.
GPIO_DRIVE_T
gpio_get_drive(unsigned gpio)
{
const GPIO_CHIP_INTERFACE_T *ifaceP;
unsigned gpioOffset;
void *privP;

    ifaceP = NULL;

    if( gpioGetInterface(gpio, &ifaceP, &privP, &gpioOffset) == 0 )
    {
        return( ifaceP->gpio_get_drive(privP, gpioOffset) );
    }

    return(DRIVE_MAX);
}

// See gpiolib.h.
GPIO_PULL_T
gpio_get_pull(unsigned gpio)
{
const GPIO_CHIP_INTERFACE_T *ifaceP;
unsigned gpioOffset;
void *privP;

    ifaceP = NULL;

    if( gpioGetInterface(gpio, &ifaceP, &privP, &gpioOffset) == 0 )
    {
        return( ifaceP->gpio_get_pull(privP, gpioOffset) );
    }

    return(PULL_MAX);
}

// See gpiolib.h. No return value.
void
gpio_set_pull(unsigned gpio, GPIO_PULL_T pull)
{
const GPIO_CHIP_INTERFACE_T *ifaceP;
unsigned gpioOffset;
void *privP;

    ifaceP = NULL;

    if( gpioGetInterface(gpio, &ifaceP, &privP, &gpioOffset) == 0 )
    {
        ifaceP->gpio_set_pull(privP, gpioOffset, pull);
    }
}

// See gpiolib.h. No return value.
void
gpio_get_pin_range(unsigned *first, unsigned *last)
{
unsigned i;
uint32_t base;

    if( firstHdrPin == GPIO_INVALID )
    {
        for( i = 0; i < numGpioChips; i++ )
        {
            if( !strncmp(gpioChips[i].nameP, "bcm2", 4) || !strcmp(gpioChips[i].nameP, "rp1") )
            {
                // Assume it's the standard RPi 40-pin header layout
                base = gpioChips[i].base;

                hdrGpios[3] = base + 2;
                hdrGpios[5] = base + 3;
                hdrGpios[7] = base + 4;
                hdrGpios[8] = base + 14;
                hdrGpios[10] = base + 15;
                hdrGpios[11] = base + 17;
                hdrGpios[12] = base + 18;
                hdrGpios[13] = base + 27;
                hdrGpios[15] = base + 22;
                hdrGpios[16] = base + 23;
                hdrGpios[18] = base + 24;
                hdrGpios[19] = base + 10;
                hdrGpios[21] = base + 9;
                hdrGpios[18] = base + 24;
                hdrGpios[22] = base + 25;
                hdrGpios[23] = base + 11;
                hdrGpios[24] = base + 8;
                hdrGpios[26] = base + 7;
                hdrGpios[27] = base + 0;
                hdrGpios[28] = base + 1;
                hdrGpios[29] = base + 5;
                hdrGpios[31] = base + 6;
                hdrGpios[32] = base + 12;
                hdrGpios[33] = base + 13;
                hdrGpios[35] = base + 19;
                hdrGpios[36] = base + 16;
                hdrGpios[37] = base + 26;
                hdrGpios[38] = base + 20;
                hdrGpios[40] = base + 21;

                firstHdrPin = 1;
                lastHdrPin = 40;
                break;
            }
        }
    }

    if( first )
    {
        *first = firstHdrPin;
    }

    if( last )
    {
        *last = lastHdrPin;
    }
}

// See gpiolib.h.
unsigned
gpio_for_pin(int pin)
{
    if( (pin >= 1) && (pin <= NUM_HDR_PINS) )
    {
        return(hdrGpios[pin]);
    }

    return(GPIO_INVALID);
}

// See gpiolib.h.
int
gpio_to_pin(unsigned gpio)
{
int i;

    for( i = 1; i <= NUM_HDR_PINS; i++ )
    {
        if( hdrGpios[i] == gpio )
        {
            return(i);
        }
    }

    return(-1);
}

// See gpiolib.h.
unsigned
gpio_get_gpio_by_name(const char *name, int name_len)
{
unsigned gpio;
const char *gpioNameP;
const char *pP;
int len;

    if( !name_len )
    {
        name_len = strlen(name);
    }

    for( gpio = 0; gpio < numGpios; gpio++ )
    {
        gpioNameP = gpioNames[gpio];
        if( !gpioNameP )
        {
            continue;
        }

        // A GPIO's recorded name can be a "primary/alias" pair (see gpiolib_init()'s
        // arch_name-merging logic below); check each '/'-separated component in turn.
        for( pP = gpioNameP; *pP; )
        {
            len = strcspn(pP, "/");
            if( (len == name_len) && (memcmp(name, pP, name_len) == 0) )
            {
                return(gpio);
            }

            pP += len;
            if( *pP == '/' )
            {
                pP++;
            }
        }
    }

    return(GPIO_INVALID);
}

// See gpiolib.h.
const char *
gpio_get_name(unsigned gpio)
{
    if( gpio < numGpios )
    {
        return(gpioNames[gpio]);
    }

    switch( gpio )
    {
    case GPIO_INVALID:
        return("-");

    case GPIO_GND:
        return("gnd");

    case GPIO_5V:
        return("5v");

    case GPIO_3V3:
        return("3v3");

    case GPIO_1V8:
        return("1v8");

    case GPIO_OTHER:
    default:
        return("???");
    }
}

// See gpiolib.h.
const char *
gpio_get_gpio_fsel_name(unsigned gpio, GPIO_FSEL_T fsel)
{
const GPIO_CHIP_INTERFACE_T *ifaceP;
unsigned gpioOffset;
void *privP;

    ifaceP = NULL;

    if( gpioGetInterface(gpio, &ifaceP, &privP, &gpioOffset) == 0 )
    {
        return( ifaceP->gpio_get_fsel_name(privP, gpioOffset, fsel) );
    }

    return(NULL);
}

// See gpiolib.h.
const char *
gpio_get_fsel_name(GPIO_FSEL_T fsel)
{
    if( (unsigned)fsel < ARRAY_SIZE(fsel_names) )
    {
        return(fsel_names[fsel]);
    }

    return(NULL);
}

// See gpiolib.h.
const char *
gpio_get_pull_name(GPIO_PULL_T pull)
{
    if( (unsigned)pull < ARRAY_SIZE(pull_names) )
    {
        return(pull_names[pull]);
    }

    return(NULL);
}

// See gpiolib.h.
const char *
gpio_get_drive_name(GPIO_DRIVE_T drive)
{
    if( (unsigned)drive < ARRAY_SIZE(drive_names) )
    {
        return(drive_names[drive]);
    }

    return(NULL);
}

// Searches every GPIO_CHIP_T in the linker-provided "gpiochips" section (see gpiochip.h)
// for one whose "name" or "compatible" string matches 'name'. Returns a pointer to the
// matching GPIO_CHIP_T, or NULL if 'name' is NULL or no chip matches.
static const GPIO_CHIP_T *
gpioFindChip(const char *name)
{
const GPIO_CHIP_T *chipP;

    for( chipP = &__start_gpiochips; name && (chipP < &__stop_gpiochips); chipP++ )
    {
        if( !strcmp(name, chipP->name) || !strcmp(name, chipP->compatible) )
        {
            return(chipP);
        }
    }

    return(NULL);
}

// See gpiolib.h. Returns the total number of GPIOs found across every probed chip (which
// may be 0 on a system with no recognized GPIO controller), or -1 if a chip was found but
// its address could not be resolved or its instance could not be created, or if the total
// GPIO count exceeds MAX_GPIO_PINS.
int
gpiolib_init(void)
{
const GPIO_CHIP_T *chipP;
GPIO_CHIP_INSTANCE_T *instP;
char pathBuf[FILENAME_MAX];
char gpiomemIdx[4];
const char *dtPath = "/proc/device-tree";
const char *pP;
char *aliasP, *namesP, *endP, *compatibleP;
uint64_t physAddr;
size_t namesLen;
unsigned gpioBase;
unsigned pin, i, gpio;
char nameBuf[32];
const char *nameP, *archNameP;

    aliasP = NULL;

    for( pin = 0; pin <= NUM_HDR_PINS; pin++ )
    {
        hdrGpios[pin] = GPIO_INVALID;
    }

    // There is currently only one header layout
    hdrGpios[1] = GPIO_3V3;
    hdrGpios[17] = GPIO_3V3;
    hdrGpios[2] = GPIO_5V;
    hdrGpios[4] = GPIO_5V;
    hdrGpios[6] = GPIO_GND;
    hdrGpios[9] = GPIO_GND;
    hdrGpios[14] = GPIO_GND;
    hdrGpios[20] = GPIO_GND;
    hdrGpios[25] = GPIO_GND;
    hdrGpios[30] = GPIO_GND;
    hdrGpios[34] = GPIO_GND;
    hdrGpios[39] = GPIO_GND;

    if( verbose_callback )
    {
        (*verbose_callback)("GPIO chips:\n");
    }

    dt_set_path(dtPath);

    for( i = 0; ; i++ )
    {
        sprintf(pathBuf, "gpio%d", i);
        sprintf(gpiomemIdx, "%d", i);

        aliasP = dt_read_prop("/aliases", pathBuf, NULL);
        if( !aliasP && (i == 0) )
        {
            aliasP = dt_read_prop("/aliases", "gpio", NULL);
            gpiomemIdx[0] = 0;
        }

        if( !aliasP )
        {
            break;
        }

        compatibleP = dt_read_prop(aliasP, "compatible", NULL);
        if( !compatibleP )
        {
            sprintf(pathBuf, "%s/..", aliasP);
            compatibleP = dt_read_prop(pathBuf, "compatible", NULL);
        }

        chipP = gpioFindChip(compatibleP);
        dt_free(compatibleP);

        if( !chipP )
        {
            // Skip the unknown gpio chip
            dt_free(aliasP);
            continue;
        }

        physAddr = dt_parse_addr(aliasP);
        if( physAddr == INVALID_ADDRESS )
        {
            dt_free(aliasP);
            return(-1);
        }

        instP = gpioCreateInstance(chipP, physAddr, NULL, aliasP);
        if( !instP )
        {
            dt_free(aliasP);
            return(-1);
        }

        sprintf(pathBuf, "/dev/gpiomem%s", gpiomemIdx);
        instP->memFd = open(pathBuf, O_RDWR | O_SYNC);
    }

    gpioBase = 0;
    numGpios = 0;

    for( i = 0; i < numGpioChips; i++ )
    {
        instP = &gpioChips[i];
        instP->base = gpioBase;
        chipP = instP->chipP;
        instP->numGpios = chipP->interface->gpio_count(instP->privP);

        if( verbose_callback )
        {
            char msgBuf[100];

            snprintf(msgBuf, sizeof(msgBuf), "  %" PRIx64 ": %s (%d gpios)\n",
                instP->physAddr, chipP->name, instP->numGpios);
            (*verbose_callback)(msgBuf);
        }

        if( !instP->numGpios )
        {
            continue;
        }

        numGpios = gpioBase + instP->numGpios;
        gpioBase = ROUND_UP(numGpios, 100);

        if( numGpios > MAX_GPIO_PINS )
        {
            return(-1);
        }

        namesP = dt_read_prop(instP->dtnodeP, "gpio-line-names", &namesLen);
        endP = namesP + namesLen;

        for( gpio = 0, pP = namesP; gpio < instP->numGpios; gpio++ )
        {
            // Fallback mapping for the two ID_SD/ID_SC EEPROM-ID pins, which some
            // device trees name explicitly instead of via a "PINnn" line-name.
            static const char *idNames[] = { "ID_SD", "ID_SC" };
            static const int idPins[] = { 27, 28 };

            nameP = "-";

            if( pP && (pP < endP) )
            {
                nameP = pP;
                pP += strlen(pP) + 1;

                if( (sscanf(nameP, "PIN%u", &pin) != 1) || (pin < 1) || (pin > NUM_HDR_PINS) )
                {
                    pin = 0;
                    for( i = 0; i < ARRAY_SIZE(idNames); i++ )
                    {
                        if( strcmp(nameP, idNames[i]) == 0 )
                        {
                            pin = idPins[i];
                            break;
                        }
                    }
                }

                if( pin >= 1 )
                {
                    hdrGpios[pin] = instP->base + gpio;

                    if( (pin < firstHdrPin) || (firstHdrPin == GPIO_INVALID) )
                    {
                        firstHdrPin = pin;
                    }

                    if( (pin > lastHdrPin) || (lastHdrPin == GPIO_INVALID) )
                    {
                        lastHdrPin = pin;
                    }
                }
            }

            archNameP = chipP->interface->gpio_get_name(instP->privP, gpio);
            if( !nameP[0] || !archNameP )
            {
                gpioNames[instP->base + gpio] = NULL;
                continue;
            }

            if( strcmp(nameP, archNameP) != 0 )
            {
                if( strcmp(nameP, "-") == 0 )
                {
                    nameP = archNameP;
                }
                else
                {
                    if( snprintf(nameBuf, sizeof(nameBuf), "%s/%s", nameP, archNameP) >=
                        (int)sizeof(nameBuf) )
                    {
                        nameBuf[sizeof(nameBuf) - 1] = '\0';
                    }

                    nameP = nameBuf;
                }
            }

            gpioNames[instP->base + gpio] = strdup(nameP);
        }

        dt_free(namesP);
    }

    // On a board with PINs, show pins 1-40
    if( firstHdrPin == 3 )
    {
        firstHdrPin = 1;
    }

    return( (int)numGpios );
}

// See gpiolib.h. Returns the chip's GPIO count on success (may be 0), or 0 if no chip
// named 'name' is registered, or -1 if the chip was found but its instance could not be
// created.
int
gpiolib_init_by_name(const char *name)
{
const GPIO_CHIP_T *chipP;
GPIO_CHIP_INSTANCE_T *instP;
unsigned gpio;
int pin;
const char *nameP;

    for( pin = 0; pin <= NUM_HDR_PINS; pin++ )
    {
        hdrGpios[pin] = GPIO_INVALID;
    }

    if( verbose_callback )
    {
        (*verbose_callback)("GPIO chips:\n");
    }

    chipP = gpioFindChip(name);
    if( !chipP )
    {
        return(0);
    }

    instP = gpioCreateInstance(chipP, 0, NULL, NULL);
    if( !instP )
    {
        return(-1);
    }

    instP->numGpios = chipP->interface->gpio_count(instP->privP);
    numGpios = instP->numGpios;

    for( gpio = 0; gpio < instP->numGpios; gpio++ )
    {
        nameP = chipP->interface->gpio_get_name(instP->privP, gpio);
        if( !nameP )
        {
            gpioNames[instP->base + gpio] = NULL;
            continue;
        }

        gpioNames[gpio] = strdup(nameP);
    }

    if( instP->numGpios && verbose_callback )
    {
        char msgBuf[100];

        snprintf(msgBuf, sizeof(msgBuf), "  %s (%d gpios)\n", chipP->name, instP->numGpios);
        (*verbose_callback)(msgBuf);
    }

    return( (int)numGpios );
}

// See gpiolib.h. Returns 0 on success, or errno (via open()/mmap() failure) or -1 (a chip's
// gpio_probe_instance() failed) otherwise.
int
gpiolib_mmap(void)
{
int pageSize;
int memFd;
unsigned i, align;
GPIO_CHIP_INSTANCE_T *instP;
const GPIO_CHIP_T *chipP;
void *gpioMapP;
void *newPrivP;

    pageSize = getpagesize();
    memFd = -1;

    for( i = 0; i < numGpioChips; i++ )
    {
        instP = &gpioChips[i];
        chipP = instP->chipP;

        align = instP->physAddr & (pageSize - 1);

        if( instP->memFd >= 0 )
        {
            gpioMapP = mmap(
                NULL,                   /* Any address in our space will do */
                chipP->size + align,    /* Map length */
                PROT_READ | PROT_WRITE, /* Enable reading & writing */
                MAP_SHARED,             /* Shared with other processes */
                instP->memFd,           /* File to map */
                0                       /* Offset to GPIO peripheral */
                );
        }
        else
        {
            if( memFd < 0 )
            {
                memFd = open("/dev/mem", O_RDWR | O_SYNC);
                if( memFd < 0 )
                {
                    return(errno);
                }
            }

            gpioMapP = mmap(
                NULL,                     /* Any address in our space will do */
                chipP->size + align,      /* Map length */
                PROT_READ | PROT_WRITE,   /* Enable reading & writing */
                MAP_SHARED,               /* Shared with other processes */
                memFd,                    /* File to map */
                instP->physAddr - align   /* Offset to GPIO peripheral */
                );
        }

        if( gpioMapP == MAP_FAILED )
        {
            return(errno);
        }

        newPrivP = chipP->interface->gpio_probe_instance(instP->privP,
            (void *)((char *)gpioMapP + align));
        if( !newPrivP )
        {
            return(-1);
        }

        instP->privP = newPrivP;
    }

    return(0);
}

// See gpiolib.h. No return value.
void
gpiolib_set_verbose(void (*callback)(const char *))
{
    verbose_callback = callback;
}
