/*
 * This is an implementation of the PDP-1 Type 340 display.
 * It is a complex beast!
 * It's unclear what IOTs were really assigned, the documentation has conflicting values.
 * But, one shows IOT numbers that overlap the symbol generator, so using this set which was also mentioned.
 *
 * Note that unlike the Type 30 display and the Type 33 symbol generator, the coordinate system is
 * always positve; the lower left corner is 0,0, upper right 1023,1023.
 *
 * The actual hardware implementation and timing is quite complex.
 * This is accomodated by having the actual emulator in its own thread, the IOTs just send it commands and
 * get responses.
 *
 * 20-Apr-2026 wje initial implementation
 * 6-May-2026 wje a stop or start from the pidp-1 stops the 340 and reverts to param mode
 */

#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>

#include "common.h"
#include "pdp1.h"
#include "iotHandler.h"
#include "configuration.h"
#include "type340emu.h"

//#define DOLOGGING
#include "iotLogger.h"
#define LOG_IOT 0
#define LOG_START 0
#define LOG_STOP 0
#define LOG_CONFIG 0
#define LOG_ERR 0

// Make the actual iot numbers configurable for easy changes.
#define IOT1 015
#define IOT2 016
#define IOT3 017

#define MAX_WAIT 6  // max 5us cycles we will wait for a stop to be responded to

static void configure(void);

int
iotHandler(PDP1P pdp1P, int dev, int pulse, int completion)
{
int cmd;
int tmp;
int x, y;
int flags;
bool needSkip;
bool needClear;
bool needCompletion;
EmuControlP ctlP;

    if( completion )
    {
        needCompletion = true;
    }

    if( !pulse )
    {
        return(1);
    }

    // Ok to call this multiple times
    emuInitialize(pdp1P);
    ctlP = getEmuControlP();

    // get the command from the iot instruction
    cmd = (pdp1P->mb >> 6) & 077;

    iotCondLog(LOG_IOT, "iot %o cmd %o\n", dev, cmd);

    switch( dev )
    {
    case IOT1:
        flags = 0;

        if( cmd & 02 )
        {
            emuClearFlags();
        }
        if( cmd & 01 )
        {
            // drs, display resume sequence
            ctlP->command = EMU_CMD_RESUME;
            iotCondLog(LOG_IOT, "drs%s\n", (flags)?" and clear flags":"");
        }
        else
        {
            // dla, display load address
            // This also resets flags via the RUN in the display emulator
            // Get rid of any dangling response, lock if necessary
            // Note that get340Respose sets the lock and leaves it set unless the resonse is NONE.
            ctlP->address = pdp1P->io;            // This is a full 16 bit address
            ctlP->command = EMU_CMD_RUN;
            iotCondLog(LOG_IOT, "dla %o%s\n", ctlP->address, (flags)?" and clear flags":"");
        }

        emuCommandSet(ctlP);
        break;

    case IOT2:
        switch( cmd )
        {
        case 0:         // dra, display read address counter
            pdp1P->io = emuGetAddress();
            break;

        case 1:         // drc, display read coordinates
            // Note that per documentation, the lsb is lost
            emuGetXY(&x, &y);
            pdp1P->io = ((x & 01776) << 8) | ((y >> 1) & 0777);
            break;

        default:
            iotCondLog(LOG_ERR,"Iot %o got invalid command %o\n", dev, cmd);
            break;
        }
        break;

    case IOT3:
        needSkip = false;
        flags = emuGetFlags();

        // Thse can be combined in one instruction.
        if( (cmd & 01) && (flags & FLAG_LP) )
        {
            // dsp, display skip on light pen flag
            needSkip = true;
        }

        if( (cmd & 02) && (flags & FLAG_STOP) )
        {
            // dss, display skip on stop interrupt
            needSkip = true;
        }

        if( (cmd & 04) && (flags & FLAG_VEDGE) )
        {
            // dsv, display skip on vertical edgs violation
            needSkip = true;
        }

        if( (cmd & 010) && (flags & FLAG_HEDGE) )
        {
            // dsh, display skip on horizontal edge violation
            needSkip = true;
        }

        if( needSkip )
        {
            pdp1P->pc = (++(pdp1P->pc) & 0777777);       // constrain to 0-4095
        }
        break;
    }

    if( needCompletion )
    {
        needCompletion = false;
        IOCOMPLETE(pdp1P);                  // no completion supported, ignore it1
    }

    return(1);
}

void
iotStart()
{
EmuControlP ctlP;

    iotCondLog(LOG_START, "IOT %o started\n", IOT1);
    configure();

    if( emuIsInitialized() )
    {
        ctlP = getEmuControlP();
        ctlP->command = EMU_CMD_STOP;
        emuCommandSet(ctlP);
        iotCondLog(LOG_START, "Issuing stop\n", IOT1);
    }
}

void
iotStop()
{
EmuControlP ctlP;

    iotCondLog(LOG_STOP, "IOT %o stopped\n", IOT1);
    if( emuIsInitialized() )
    {
        ctlP = getEmuControlP();
        ctlP->command = EMU_CMD_STOP;
        emuCommandSet(ctlP);
        iotCondLog(LOG_START, "Issuing stop\n", IOT1);
    }

    iotCloseLog();
}

// Get our configurations settings, can be called more than once.
// We have none, placeholder
void
configure()
{
ConfigurationSettingP settingP;
}
