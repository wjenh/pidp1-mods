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
 *     is now in iotPoll() below
 *
 * Unlike the original, this uses iotPoll() instead of simtime comparisons.
 *
 * 19-Jun-2026 wje initial version.
 * 20-Jun-2026 wje stop conflating case and ribbon-color; forward tb raw.
 * 23-Jun-2026 wje add tyo fast mode via the config file, sick of waiting for that slooow output.
 */
#include <unistd.h>
#include "iotHandler.h"
#include "configuration.h"

// pdp1.c keeps these as private #defines not exposed via pdp1.h to plugins.
// Keep in sync with pdp1.c if those values ever change.
#define TTO_CHAN 8

// pdp1.c's B5/B6 wait/complete IOT flag bits.
#define B5 010000
#define B6 004000

// Cycle-count equivalent of pdp1.c's TYODLY, 10 cps.
#define TYO_POLL_CYCLES USTOCYCLES(100000)

// And fast mode, 200 cps.
#define TYO_POLL_FASTCYCLES USTOCYCLES(5000)

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
            enablePolling((fastTyo)?TYO_POLL_FASTCYCLES:TYO_POLL_CYCLES);
        }
    }

    return(1);
}

// Called by dynamicIotProcessorDoPoll() once TYO_POLL_CYCLES cycles have elapsed.
// Polling is disabled again immediately after firing, the next tyo IOT re-arms it.
void
iotPoll(PDP1 *pdp1P)
{
    if(pdp1P->tb == 072 || pdp1P->tb == 074)
    {
        pdp1P->tbb = (pdp1P->tb == 074);
    }

    if(pdp1P->typ_fd.fd >= 0)
    {
        char c = pdp1P->tb;
        write(pdp1P->typ_fd.fd, &c, 1);
    }

    pdp1P->tyo = 0;

    if(pdp1P->tcp)
    {
        IOCOMPLETE(pdp1P);
    }

    initiateBreak(TTO_CHAN);
    enablePolling(0);
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
