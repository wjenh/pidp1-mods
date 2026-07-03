// pinctrl.c -- Standalone CLI tool (upstream raspberrypi-utils "pinctrl") for inspecting and
// driving GPIO pins directly through gpiolib.h, plus (from the "int cols[12]" block onward) an
// ad hoc PiDP-1 panel LED/switch matrix scan added by a prior developer. Entry point is
// xmain(), not main() -- nothing else in this tree references xmain, xprogram_name, or any
// other symbol here (confirmed by grep), so this file does not currently link into a runnable
// executable via this directory's Makefile (which only builds .o files, no link step).
// Formatting/naming here follows the project standard for everything local to this file;
// xmain()/xprogram_name are left as-is since their external ("x"-prefixed, non-static)
// linkage looks like a deliberate convention for whatever wrapper eventually supplies a real
// main() and calls xmain(argc, argv) from it.
//
// FIX (03-Jul-26): the PiDP-1 panel scan block below (starting at "int cols[12]") used to run
// unconditionally and end in a "do { ... } while(1)" loop with no break, which made the
// pinctrl get/set/poll/funcs dispatch immediately after gpiolib_mmap() -- including the
// "if (poll) doGpioPoll();" line at the very end -- permanently unreachable. It is now gated
// behind its own explicit "leds" subcommand (see the 'leds' variable in xmain()), so it still
// runs forever exactly as before when a caller asks for it (Ctrl-C to stop, as it always
// required), but no longer swallows every other invocation of the tool. get/set/poll/funcs
// dispatch is unchanged from upstream otherwise.

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/time.h>

#include "gpiolib.h"

#define ARRAY_SIZE(_a) (sizeof(_a) / sizeof(_a[0]))

const char *xprogram_name = "pinctrl";

static int pinMode = 0;
static int verboseMode = 0;
static unsigned numGpios;

typedef struct
{
    unsigned int num;
    unsigned int gpio;
    const char *nameP;
    int level;
} PollGpioState, *PollGpioStateP;

int numPollGpios;
PollGpioStateP pollGpiosP;

// Prints one line of "funcs"-style output for a single GPIO/pin: its number, name (if any),
// and every alternate function name it supports (terminated by the first NULL
// gpio_get_gpio_fsel_name() returns). No return value; does nothing if the pin/gpio resolves
// to an invalid GPIO.
static void
printGpioAltsInfo(unsigned gpio)
{
const char *nameP;
unsigned int num;
int fsel;

    num = gpio;

    if( pinMode )
    {
        gpio = gpio_for_pin(num);
    }

    if( !gpio_num_is_valid(gpio) )
    {
        return;
    }

    nameP = gpio_get_name(gpio);
    if( pinMode && strchr(nameP, '/') )
    {
        nameP = strchr(nameP, '/') + 1;
    }

    printf("%d", num);
    if( nameP[0] )
    {
        printf(", %s", nameP);
    }

    for( fsel = GPIO_FSEL_FUNC0; ; fsel++ )
    {
        nameP = gpio_get_gpio_fsel_name(gpio, fsel);
        if( !nameP )
        {
            break;
        }

        printf(", %s", nameP);
    }

    printf("\n");
}

// Prints the tool's usage/help text to stdout. No return value.
static void
usage()
{
const char *nameP;

    nameP = xprogram_name;

    printf("\n");
    printf("WARNING! %s set writes directly to the GPIO control registers\n", nameP);
    printf("ignoring whatever else may be using them (such as Linux drivers) -\n");
    printf("it is designed as a debug tool, only use it if you know what you\n");
    printf("are doing and at your own risk!\n");
    printf("\n");
    printf("Running %s with the help argument prints this help.\n", nameP);
    printf("%s can get and print the state of a GPIO (or all GPIOs)\n", nameP);
    printf("and can be used to set the function, pulls and value of a GPIO.\n");
    printf("%s must be run as root.\n", nameP);
    printf("Use:\n");
    printf("  %s [-p] [-v] get [GPIO]\n", nameP);
    printf("OR\n");
    printf("  %s [-p] [-v] set <GPIO> [options]\n", nameP);
    printf("OR\n");
    printf("  %s [-p] [-v] poll [GPIO]\n", nameP);
    printf("OR\n");
    printf("  %s [-p] [-v] funcs [GPIO]\n", nameP);
    printf("OR\n");
    printf("  %s -c <chip> [funcs] [GPIO]\n", nameP);
    printf("\n");
    printf("GPIO is a comma-separated list of GPIO names, numbers or ranges (without\n");
    printf("spaces), e.g. 4 or 18-21 or BT_ON,9-11\n");
    printf("\n");
    printf("Note that omitting [GPIO] from \"%s get\" prints all GPIOs.\n", nameP);
    printf("If the -p option is given, GPIO numbers are replaced by pin numbers on the\n");
    printf("40-way header. If the -v option is given, the output is more verbose.\n");
    printf("%s funcs will dump all the possible GPIO alt funcions in CSV format\n", nameP);
    printf("or if [GPIO] is specified the alternate funcs just for that specific GPIO.\n");
    printf("The -c option allows the alt functions (and only the alt function) for a named\n");
    printf("chip to be displayed, even if that chip is not present in the current system.\n");
    printf("\n");
    printf("Valid [options] for %s set are:\n", nameP);
    printf("  ip      set GPIO as input\n");
    printf("  op      set GPIO as output\n");
    printf("  a1-a7   set GPIO to fsel in the range 1-7\n");
    printf("  no      set GPIO to no function (NONE)\n");
    printf("  pu      set GPIO in-pad pull up\n");
    printf("  pd      set GPIO pin-pad pull down\n");
    printf("  pn      set GPIO pull none (no pull)\n");
    printf("  dh      set GPIO to drive high (1) level (only valid if set to be an output)\n");
    printf("  dl      set GPIO to drive low (0) level (only valid if set to be an output)\n");
    printf("Examples:\n");
    printf("  %s get              Prints state of all GPIOs one per line\n", nameP);
    printf("  %s get 10           Prints state of GPIO10\n", nameP);
    printf("  %s get 10,11        Prints state of GPIO10 and GPIO11\n", nameP);
    printf("  %s set 10 a2        Set GPIO10 to fsel 2 function (nand_wen_clk)\n", nameP);
    printf("  %s set 10 pu        Enable GPIO10 ~50k in-pad pull up\n", nameP);
    printf("  %s set 10 pd        Enable GPIO10 ~50k in-pad pull down\n", nameP);
    printf("  %s set 10 op        Set GPIO10 to be an output\n", nameP);
    printf("  %s set 10 dl        Set GPIO10 to output low/zero (must already be set as an output)\n", nameP);
    printf("  %s set 10 ip pd     Set GPIO10 to input with pull down\n", nameP);
    printf("  %s set 35 a1 pu     Set GPIO35 to fsel 1 (jtag_2_clk) with pull up\n", nameP);
    printf("  %s set 20 op pn dh  Set GPIO20 to output with no pull and driving high\n", nameP);
    printf("  %s -c bcm2835 9-11  Display the alt functions for GPIOs 9-11 on bcm2835\n", nameP);
}

// Implements "pinctrl get <gpio>": prints the GPIO's (or pin's) current fsel/drive/pull/
// level/name. Returns 0 on success, 1 if the resolved GPIO is invalid (a non-GPIO pin name
// is printed and 0 is returned instead, matching the original tool's behavior).
static int
doGpioGet(unsigned int gpio)
{
unsigned int num;
const char *nameP;
int fsel;
int level;

    num = gpio;

    if( pinMode )
    {
        gpio = gpio_for_pin(num);
        switch( gpio )
        {
        case GPIO_INVALID:
        case GPIO_GND:
        case GPIO_5V:
        case GPIO_3V3:
        case GPIO_1V8:
        case GPIO_OTHER:
            nameP = gpio_get_name(gpio);
            printf("%2d: %s\n", num, nameP);
            return(0);
        }
    }

    if( !gpio_num_is_valid(gpio) )
    {
        return(1);
    }

    fsel = gpio_get_fsel(gpio);
    printf("%2d: %2s ", num, gpio_get_fsel_name(fsel));
    if( fsel == GPIO_FSEL_OUTPUT )
    {
        printf("%s", gpio_get_drive_name(gpio_get_drive(gpio)));
    }
    else
    {
        printf("  ");
    }

    nameP = gpio_get_name(gpio);
    if( pinMode && strchr(nameP, '/') )
    {
        nameP = strchr(nameP, '/') + 1;
    }

    level = gpio_get_level(gpio);

    printf(" %s | %s // %s%s%s\n",
        gpio_get_pull_name(gpio_get_pull(gpio)),
        (level == 1) ? "hi" : (level == 0) ? "lo" : "--",
        nameP ? nameP : "",
        nameP ? " = " : "",
        gpio_get_gpio_fsel_name(gpio, fsel));

    return(0);
}

// Implements "pinctrl set <gpio> [options]": applies fsparam/drive/pull (each either a real
// value or the corresponding _MAX "leave alone" sentinel) to the given GPIO/pin. Returns 0
// on success, 1 if the GPIO/pin is invalid, or if a drive level was requested on a GPIO not
// currently set as an output.
static int
doGpioSet(unsigned int gpio, int fsparam, int drive, int pull)
{
unsigned int num;

    num = gpio;

    if( pinMode )
    {
        gpio = gpio_for_pin(num);
        if( gpio >= numGpios )
        {
            printf("Pin %d cannot be set\n", num);
            return(1);
        }
    }

    if( !gpio_num_is_valid(gpio) )
    {
        return(1);
    }

    if( fsparam != GPIO_FSEL_MAX )
    {
        gpio_set_fsel(gpio, fsparam);
    }
    else
    {
        fsparam = gpio_get_fsel(gpio);
    }

    if( drive != DRIVE_MAX )
    {
        if( fsparam == GPIO_FSEL_OUTPUT )
        {
            gpio_set_drive(gpio, drive);
        }
        else
        {
            printf("Can't set pin value, not an output\n");
            return(1);
        }
    }

    if( pull != PULL_MAX )
    {
        gpio_set_pull(gpio, pull);
    }

    return(0);
}

// Implements "pinctrl poll <gpio>"'s setup phase: adds one more GPIO/pin to the pollGpiosP
// array that doGpioPoll() will watch. Returns 0 on success, 1 if the GPIO/pin is invalid.
static int
doGpioPollAdd(unsigned int gpio)
{
PollGpioStateP newGpioP;
unsigned int num;

    num = gpio;

    if( pinMode )
    {
        gpio = gpio_for_pin(num);
    }

    if( !gpio_num_is_valid(gpio) )
    {
        return(1);
    }

    pollGpiosP = reallocarray(pollGpiosP, numPollGpios + 1, sizeof(*pollGpiosP));
    newGpioP = &pollGpiosP[numPollGpios];
    newGpioP->num = num;
    newGpioP->gpio = gpio;
    newGpioP->nameP = gpio_get_name(gpio);
    newGpioP->level = -1; /* Unknown */
    numPollGpios++;

    return(0);
}

// Implements "pinctrl poll"'s run phase: busy-polls every registered GPIO forever, printing
// a line each time one changes level (with the idle interval since the last change), until
// killed. No return value (never returns while numPollGpios > 0).
static void
doGpioPoll(void)
{
unsigned int idleCount;
struct timeval idleStart;
int i, changed, level;
PollGpioStateP stateP;
struct timeval now;
uint64_t intervalUs;

    idleCount = 0;

    while( numPollGpios )
    {
        changed = 0;

        for( i = 0; i < numPollGpios; i++ )
        {
            stateP = &pollGpiosP[i];
            level = gpio_get_level(stateP->gpio);

            if( level != stateP->level )
            {
                if( idleCount )
                {
                    gettimeofday(&now, NULL);
                    intervalUs = (uint64_t)(now.tv_sec - idleStart.tv_sec) * 1000000 +
                        (now.tv_usec - idleStart.tv_usec);
                    printf("+%" PRIu64 "us\n", intervalUs);
                    idleCount = 0;
                }

                printf("%2d: %s // %s\n", stateP->num, level ? "hi" : "lo", stateP->nameP);
                stateP->level = level;
                changed = 1;
            }
        }

        if( !changed )
        {
            if( !idleCount )
            {
                gettimeofday(&idleStart, NULL);
            }

            idleCount++;
        }
    }
}

// Verbose-mode callback registered with gpiolib_set_verbose(): just echoes the message to
// stdout. No return value.
static void
verboseCallback(const char *msg)
{
    printf("%s", msg);
}

int
xmain(int argc, char *argv[])
{
int ret;
const char *namedChipP;
int set, get, poll, funcs, leds;
int pull;
int inferCmd;
int fsparam;
int drive;
uint32_t gpiomask[(MAX_GPIO_PINS + 31) / 32] = { 0 };
unsigned startPin, endPin, pin;
int i;
const char *argP;
const char *cmdP;
char *pP;
unsigned gpio, gpio2;
int len, len2, pinNum, tmpVal;
int ic, ir, is;
int cols[12] = { 26, 27, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
int rows[6] = { 20, 21, 22, 23, 24, 25 };
int srows[3] = { 16, 17, 18 };

    namedChipP = NULL;
    set = 0;
    get = 0;
    poll = 0;
    funcs = 0;
    leds = 0;
    pull = PULL_MAX;
    inferCmd = 0;
    fsparam = GPIO_FSEL_MAX;
    drive = DRIVE_MAX;
    startPin = GPIO_INVALID;

    /* arg parsing */

    argv++;
    argc--;

    while( argc && (argv[0][0] == '-') )
    {
        argP = *(argv++);
        argc--;

        if( strcmp(argP, "-h") == 0 )
        {
            usage();
            return(0);
        }
        else if( strcmp(argP, "-p") == 0 )
        {
            pinMode = 1;
        }
        else if( strcmp(argP, "-v") == 0 )
        {
            verboseMode = 1;
        }
        else if( strcmp(argP, "-c") == 0 )
        {
            if( !argc )
            {
                printf("* chip name expected - use 'pinctrl -h' for help\n");
                return(-1);
            }

            namedChipP = *(argv++);
            argc--;
        }
        else
        {
            printf("Unknown option '%s' - try \"%s help\"\n", argP, xprogram_name);
            exit(1);
        }
    }

    if( verboseMode )
    {
        gpiolib_set_verbose(&verboseCallback);
    }

    if( namedChipP )
    {
        ret = gpiolib_init_by_name(namedChipP);
    }
    else
    {
        ret = gpiolib_init();
    }

    if( ret < 0 )
    {
        printf("Failed to initialise gpiolib - %d\n", ret);
        return(-1);
    }

    numGpios = ret;
    if( !numGpios )
    {
        printf("No GPIO chips found\n");
        return(-1);
    }

    if( argc )
    {
        cmdP = *(argv++);
        argc--;

        if( strcmp(cmdP, "help") == 0 )
        {
            usage();
            return(0);
        }

        get = strcmp(cmdP, "get") == 0;
        set = strcmp(cmdP, "set") == 0;
        poll = strcmp(cmdP, "poll") == 0;
        funcs = strcmp(cmdP, "funcs") == 0;
        leds = strcmp(cmdP, "leds") == 0;

        if( !set && !get && !poll && !funcs && !leds )
        {
            /* Back up in case we can decode this as a pin */
            argv--;
            argc++;
            inferCmd = 1;
        }
    }
    else if( namedChipP )
    {
        funcs = 1;
    }
    else
    {
        get = 1;
    }

    if( pinMode )
    {
        gpio_get_pin_range(&startPin, &endPin);
        if( startPin == GPIO_INVALID )
        {
            printf("No PIN numbers declared in DT - pin mode disabled\n");
            pinMode = 0;
        }
    }

    if( startPin == GPIO_INVALID )
    {
        startPin = 0;
        endPin = numGpios - 1;
    }

    if( argc ) /* expect pin number/name(s) next */
    {
        pP = *(argv++);
        argc--;

        while( pP )
        {
            len = strcspn(pP, "-,");
            ret = sscanf(pP, "%u%n", &gpio, &len2);
            if( (ret == 1) && (len == len2) && (gpio >= numGpios) )
            {
                break;
            }
            else if( (ret != 1) || (len != len2) )
            {
                gpio = gpio_get_gpio_by_name(pP, len);
                if( gpio == GPIO_INVALID )
                {
                    break;
                }

                if( pinMode )
                {
                    pinNum = gpio_to_pin(gpio);
                    if( pinNum < 0 )
                    {
                        printf("Signal \"%*s\" is not on a header pin\n", len, pP);
                        return(1);
                    }

                    gpio = (unsigned)pinNum;
                }
            }

            pP += len;

            if( (*pP == '\0') && argc && ((argv[0][0] == '-') || (argv[0][0] == ',')) )
            {
                pP = *(argv++);
                argc--;
            }

            if( *pP == '-' )
            {
                pP++;
                len = strcspn(pP, "-,");
                ret = sscanf(pP, "%u%n", &gpio2, &len2);
                if( (ret == 1) && (len == len2) && (gpio2 >= numGpios) )
                {
                    break;
                }
                else if( (ret != 1) || (len != len2) )
                {
                    gpio2 = gpio_get_gpio_by_name(pP, len);
                    if( gpio2 == GPIO_INVALID )
                    {
                        break;
                    }

                    if( pinMode )
                    {
                        pinNum = gpio_to_pin(gpio2);
                        if( pinNum < 0 )
                        {
                            printf("Signal \"%*s\" is not on a header pin\n", len, pP);
                            return(1);
                        }

                        gpio2 = (unsigned)pinNum;
                    }
                }

                if( gpio2 < gpio )
                {
                    tmpVal = gpio2;
                    gpio2 = gpio;
                    gpio = tmpVal;
                }

                pP += len;
            }
            else
            {
                gpio2 = gpio;
            }

            while( gpio <= gpio2 )
            {
                gpiomask[gpio / 32] |= (1 << (gpio % 32));
                gpio++;
            }

            if( (*pP == '\0') && argc && (argv[0][0] == ',') )
            {
                pP = *(argv++);
                argc--;
            }

            if( *pP == '\0' )
            {
                pP = NULL;
            }
            else
            {
                if( *pP != ',' )
                {
                    break;
                }

                pP++;
            }
        }

        if( pP )
        {
            if( inferCmd && (pP == argv[-1]) )
            {
                printf("Unknown command \"%s\"\n", pP);
            }
            else
            {
                printf("Unknown GPIO \"%s\"\n", pP);
            }

            return(1);
        }
    }
    else if( set )
    {
        printf("Need GPIO number to set\n");
        return(1);
    }
    else if( poll )
    {
        printf("Need GPIO number to poll\n");
        return(1);
    }

    if( set && !argc )
    {
        printf("Need a function or pull to set\n");
        return(1);
    }

    if( (get || funcs) && argc )
    {
        printf("Too many arguments\n");
        return(1);
    }

    if( inferCmd )
    {
        if( namedChipP )
        {
            funcs = 1;
        }
        else if( argc )
        {
            set = 1;
        }
        else
        {
            get = 1;
        }
    }

    /* parse remaining args */
    while( argc )
    {
        argP = *(argv++);
        argc--;

        if( strcmp(argP, "dh") == 0 )
        {
            drive = DRIVE_HIGH;
        }
        else if( strcmp(argP, "dl") == 0 )
        {
            drive = DRIVE_LOW;
        }
        else if( strcmp(argP, "gp") == 0 )
        {
            fsparam = GPIO_FSEL_GPIO;
        }
        else if( strcmp(argP, "ip") == 0 )
        {
            fsparam = GPIO_FSEL_INPUT;
        }
        else if( strcmp(argP, "op") == 0 )
        {
            fsparam = GPIO_FSEL_OUTPUT;
        }
        else if( strcmp(argP, "no") == 0 )
        {
            fsparam = GPIO_FSEL_NONE;
        }
        else if( strcmp(argP, "a0") == 0 )
        {
            fsparam = GPIO_FSEL_FUNC0;
        }
        else if( strcmp(argP, "a1") == 0 )
        {
            fsparam = GPIO_FSEL_FUNC1;
        }
        else if( strcmp(argP, "a2") == 0 )
        {
            fsparam = GPIO_FSEL_FUNC2;
        }
        else if( strcmp(argP, "a3") == 0 )
        {
            fsparam = GPIO_FSEL_FUNC3;
        }
        else if( strcmp(argP, "a4") == 0 )
        {
            fsparam = GPIO_FSEL_FUNC4;
        }
        else if( strcmp(argP, "a5") == 0 )
        {
            fsparam = GPIO_FSEL_FUNC5;
        }
        else if( strcmp(argP, "a6") == 0 )
        {
            fsparam = GPIO_FSEL_FUNC6;
        }
        else if( strcmp(argP, "a7") == 0 )
        {
            fsparam = GPIO_FSEL_FUNC7;
        }
        else if( strcmp(argP, "a8") == 0 )
        {
            fsparam = GPIO_FSEL_FUNC8;
        }
        else if( strcmp(argP, "pu") == 0 )
        {
            pull = PULL_UP;
        }
        else if( strcmp(argP, "pd") == 0 )
        {
            pull = PULL_DOWN;
        }
        else if( strcmp(argP, "pn") == 0 )
        {
            pull = PULL_NONE;
        }
        else
        {
            printf("Unknown argument \"%s\"\n", argP);
            return(1);
        }
    }

    for( i = ARRAY_SIZE(gpiomask) - 1; i >= 0; i-- )
    {
        if( gpiomask[i] )
        {
            break;
        }
    }

    if( i < 0 )
    {
        memset(gpiomask, 0xff, sizeof(gpiomask));
    }

    if( !funcs )
    {
        ret = gpiolib_mmap();
        if( ret )
        {
            if( (ret == EACCES) && geteuid() )
            {
                printf("Must be root\n");
            }
            else
            {
                printf("Failed to mmap gpiolib - %s\n", strerror(ret));
            }

            return(-1);
        }
    }

    // ------------------------------------------------------------------------------------
    // Below this point: an ad hoc PiDP-1 panel LED/switch matrix scan added by a prior
    // developer. Formerly ran unconditionally and never returned, permanently hiding the
    // get/set/poll/funcs dispatch below it; now gated behind the explicit "leds" subcommand
    // (see the FIX note in the file header) so it only runs when asked for, and still never
    // returns on its own when it does (Ctrl-C to stop, as it always required).
    // ------------------------------------------------------------------------------------

    if( leds )
    {
        printf("Hello\n");

        // init
        for( ir = 0; ir < 6; ir++ )
        {
            gpio_set_fsel(rows[ir], GPIO_FSEL_OUTPUT);
            gpio_set_dir(rows[ir], DIR_OUTPUT);
            gpio_set_drive(rows[ir], DRIVE_LOW);
        }

        for( ic = 0; ic < 12; ic++ )
        {
            gpio_set_fsel(cols[ic], GPIO_FSEL_OUTPUT);
            gpio_set_pull(cols[ic], PULL_UP);
            gpio_set_dir(cols[ic], DIR_OUTPUT);
            gpio_set_drive(cols[ic], DRIVE_LOW);
        }

        for( is = 0; is < 3; is++ )
        {
            gpio_set_fsel(srows[is], GPIO_FSEL_INPUT);
            //gpio_set_dir(srows[is], DIR_INPUT);
            gpio_set_pull(srows[is], PULL_UP);
        }

        // main loop
        do
        {
            // do 6 led rows
            for( ir = 0; ir < 6; ir++ )
            {
                if( fsparam == DRIVE_HIGH )
                {
                    fsparam = DRIVE_LOW;
                }
                else
                {
                    fsparam = DRIVE_HIGH;
                }

                // for each led row, do 12 leds
                for( ic = 0; ic < 12; ic++ )
                {
                    pin = cols[ic];
                    if( !(gpiomask[pin / 32] & (1 << (pin % 32))) )
                    {
                        printf("err1\n");
                        continue;
                    }

                    gpio_set_dir(cols[ic], DIR_OUTPUT);
                    gpio_set_drive(pin, fsparam);
                }

                // light up this row with selected leds for 500 usec
                gpio_set_drive(rows[ir], DRIVE_HIGH);
                usleep(500);
                // close down this row, and give 1 usec to avoid ghosting
                gpio_set_drive(rows[ir], DRIVE_LOW);
                usleep(1);

                // prepare for reading switches by setting cols to input
                for( ic = 0; ic < 12; ic++ )
                {
                    gpio_set_dir(cols[ic], DIR_INPUT);
                }

                // now read 3 switch rows
                for( is = 0; is < 3; is++ )
                {
                    // set one row to OUTPUT LOW
                    //gpio_set_fsel(srows[is], GPIO_FSEL_INPUT);
                    gpio_set_dir(srows[is], DIR_OUTPUT);
                    gpio_set_drive(srows[is], DRIVE_LOW);

                    // read 12 switches
                    for( ic = 0; ic < 12; ic++ )
                    {
                        printf("-%d", gpio_get_level(cols[ic]));
                    }

                    printf("\n");
                    // set the switch row to INPUT again
                    gpio_set_dir(srows[is], DIR_INPUT);
                }

                // prepare for lighting LEDs again by setting cols to OUTPUT again
                for( ic = 0; ic < 12; ic++ )
                {
                    gpio_set_dir(cols[ic], DIR_OUTPUT);
                }
            }
        } while( 1 );
    }

    if( poll )
    {
        doGpioPoll();
    }

    return(0);
}
