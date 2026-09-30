/**
 * Dynamic IOT for tyo (device 3) -- typewriter output.
 *
 * tyo is now a real IOT, removed from the pdp1.c emulator where it didn't belong.
 * tyi and tyo are independent input/output streams that can be in
 * flight simultaneously and need different polling mechanisms, so they're two
 * separate IOTs that happen to share the same underlying pdp1P fields (tb/tbs/tbb/tyo/tcp/tyi_wait/typ_fd/pf)
 * and fd (typ_fd), the way the original built-in code did.
 *
 * This is am accurate port of pdp1.c's tyo logic:
 *   - the arm/load logic from iot_pulse()'s case 003, now in iotHandler()
 *   - the output-completion logic from handleio()'s Typewriter block
 *     is now in iotDeadline() below
 *
 * Like the original, the delay is timed on simtime, through a deadline on the device time
 * base (iotPollAt() with IOT_TIME_DEVICE), which also runs in a halt.
 *
 * 19-Jun-2026 wje initial version.
 * 20-Jun-2026 wje stop conflating case and ribbon-color; forward tb raw.
 * 23-Jun-2026 wje add tyo fast mode via the config file, sick of waiting for that slooow output.
 * 23-Sep-2026 Claude write() blocked forever with nothing draining the typtelnet
 *   socketpair with no client on port 1041, freezing the whole emulator thread, now non-blocking
 * 28-Sep-2026 Claude the output delay is a simtime deadline, not a count of executed cycles.
 */
#include <sys/socket.h>
#include "iotHandler.h"
#include "configuration.h"

// pdp1.c keeps these as private #defines not exposed via pdp1.h to plugins.
// Keep in sync with pdp1.c if those values ever change.
#define TTO_CHAN 8

// pdp1.c's B5/B6 wait/complete IOT flag bits.
#define B5 010000
#define B6 004000

// pdp1.c's TYODLY, 10 cps.
#define TYO_DELAY_NS USTONS(100000)

// And fast mode, 200 cps.
#define TYO_FAST_DELAY_NS USTONS(5000)

static bool fastTyo = false;
static bool configDone = false;
static void configure();

int
iotHandler(PDP1 *pdp1P, int device, int pulse, int completion)
{

    if( !configDone )
    {
        configure();
        configDone = true;
    }

    if(!pulse)
    {
        if(!pdp1P->tyo)
        {
            pdp1P->tb = 0;
        }
    }
    else
    {
        pdp1P->tcp = !!(MB(pdp1P) & (B5 | B6));

        if(!pdp1P->tyo)
        {
            pdp1P->tyo = 1;
            pdp1P->tb |= IO(pdp1P) & 077;
            iotPollAt(IOT_TIME_DEVICE, (iotTime(IOT_TIME_DEVICE) + ((fastTyo)?TYO_FAST_DELAY_NS:TYO_DELAY_NS)));
        }
    }

    return(1);
}

// Called once the output delay has passed. The deadline is one-shot; the next tyo IOT arms it.
void
iotDeadline(PDP1 *pdp1P)
{
    if(pdp1P->tb == 072 || pdp1P->tb == 074)
    {
        pdp1P->tbb = (pdp1P->tb == 074);
    }

    // MSG_DONTWAIT: iotDeadline() runs inline in the main loop, so a blocking write()
    // here would stall the entire emulator.
    // Drop the character on backpressure instead; real hardware has no flow control from tyo
    // back to the CPU either, so this is authentic.
    if(pdp1P->typ_fd.fd >= 0)
    {
        char c = pdp1P->tb;
        send(pdp1P->typ_fd.fd, &c, 1, MSG_DONTWAIT);
    }

    pdp1P->tyo = 0;

    if(pdp1P->tcp)
    {
        IOCOMPLETE(pdp1P);
    }

    initiateBreak(TTO_CHAN);
}

// Load any config settings.
void
configure()
{
ConfigurationSettingP settingP;

    if( (settingP = findConfigurationSetting(getConfiguration(), "fasttyo")) )
    {
        fastTyo = settingP->onOff;
    }
}
