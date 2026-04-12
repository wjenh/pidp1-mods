/*
 * This is an implementation of the PDP-1 Type 30 display.
 * It has been moved from inside the emulator, where it really doesn't belong, here.
 *
 * According to the DEC documentation, it takes approximately 35 microseconds to draw a character,
 * 30 usecs to position the dot, then 5 usecs of intensification.
 *
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>

#include "common.h"
#include "pdp1.h"
#include "iotHandler.h"
#include "configuration.h"

//#define DOLOGGING
#include "iotLogger.h"
#define LOG_START 0
#define LOG_DPYSHIFT 0
#define LOG_SDB 0
#define LOG_CONFIG 0
#define LOG_APERTURE 0
#define LOG_POLL 0
#define LOG_LIGHTPEN 0

#define MOVEDELAY 30                // beam move delay, usecs
#define DRAWDELAY 5                 // intinsification delay,usecs
#define IDLEDELAY 100               // if not drawing, repeat time for display aging, usecs
#define APERTURE 6                  // the default, 0.050"

// The light pen came with 6 different aperture masks ranging from 0.05 to 0.30 inches.
// The setting of penAperture, see readLightpen() below, emulates these by defining the distance from the
// last dpy coordinates to the light pen coordinates such that if the pen coordinates are within
// penAperture pixels it is considered a hit.
// The setting of penAperture is used to compute penRadius2, which is what is actually compared against.
// This simulates a circular aperture.
// THe nonstandard dpy 3000 extension allows changing the aperture.
// IO contains the aperture size in pixels, 6-61.
// Each pixel corresponds to 1/1024th of the display, or 0.009" on the original Type 30 display.
// APERTURE sets the default value.
// The emulator didn't seem to actually implement a second display for this.
//
// Aperture Setting  Size with 0.009" pixels
// 0.05     6        0.054
// 0.10     11       0.111
// 0.15     17       0.153
// 0.20     22       0.198
// 0.25     28       0.252
// 0.30     33       0.297
//
static int penAperture = APERTURE;
static int penRadius2 = (APERTURE/2) * ( APERTURE/2);  // radius squared

static bool lightpenEnabled;
static bool dpyShiftEnabled;
static bool sdbEnabled;
static bool needCompletion;
static bool twoscreensEnabled;

static enum {IDLE, DEFLECTION, DRAW} pollState;

static void configure();

extern void display(PDP1P pdp1P, int screenNo, int x, int y, int intensity);
extern bool checkLightpen(PDP1P pdp1P, int x, int y);

// Some machines had a variation where the intensity could be set using the
// xx03xx bits. Snowflake uses it.
// Some had a variant where the display origin could be shifted setting the x origin
// to left edge, center, right edge, the y origin to top, center, bottom using the
// xx30x bits.
// The symbol generator added the sdb variant, invisibly move display point using the value
// xx20xx, which conflicts with the origin-shifting.
// The aperture variant, added to support the pseudo-lightpen, uses
// xx34xx.
// This theoretically could conflict with some intensity setting, but no cases of an intensity
// of intensity 4 have been found.
int
iotHandler(PDP1 *pdp1P, int dev, int pulse, int completion)
{
int i;
int delayTime;
bool noWait;

    noWait = false;
    delayTime = 0;

    if( completion )
    {
        needCompletion = true;
    }

    if( !pulse )
    {
        pdp1P->curDispX = 0;
        pdp1P->curDispY = 0;
        pdp1P->curDispIntensity = 0;
        pollState = DRAW;

        // Be sure it's set to the current value
        pdp1P->dpy[0].lpRadius2 = penRadius2;
        if( lightpenEnabled )
        {
            pdp1P->cksflags &= ~0400000;  // set by the last dpy completion if lp hit
        }

        return(1);
    }
    else if( lightpenEnabled && (pdp1P->mb & 003777) == 003407 )       // set lightpen aperture
    {
        // The aperture is the diameter in pixels, allow 6 to 63
        // Each pixel corresponds to the original 0.009"
        i = penAperture;                // current value
        penAperture = pdp1P->io & 077; 

        if( penAperture < 6 )
        {
            penAperture = 6;
        }

        // We keep a copy in the dpy struct so checkLightpen can find it
        penRadius2 = (penAperture/2) * (penAperture/2);  // radius squared
        pdp1P->dpy[0].lpRadius2 = penRadius2;

        noWait = true;
        pollState = IDLE;

        iotCondLog(LOG_APERTURE,"Aperture was %d, now %d, new radius squared %d\n", i,
            penAperture, pdp1P->dpy[0].lpRadius2);
    }
    else
    {
        pdp1P->curDispX = pdp1P->ac >> 8;
        pdp1P->curDispY = pdp1P->io >> 8;
        pdp1P->curDispIntensity = (pdp1P->mb >> 6) & 7;

        // Emulate the origin shift that was implemented in some systems
        // It conflicts with sdb, the following test is done.
        // It checks to see if i or C is set to distiguish it from a program that's using
        // sdb, will fail if a prog just does a bare dpy with shift and sdb will be assumed.
        if( dpyShiftEnabled && (pdp1P->mb & 03000) )
        {
            if(pdp1P->mb & 01000)        // origin at bottom
            {
                pdp1P->curDispY ^= 01000;
            }

            if(pdp1P->mb & 02000)        // origin at left
            {
                pdp1P->curDispX ^= 01000;
            }

            iotCondLog(LOG_DPYSHIFT,"Dpy shift pdp1P->mb %06o\n", pdp1P->mb);
        }

        if( sdbEnabled && ((pdp1P->mb & 017000) == 02000) )  // sdb is a reposition without drawing a dot
        {
            // This is documented as taking 30 usecs because it doesn't
            // need the addtional time to draw the dot.
            // All it does is set the intensity and reposition x,y, does not honor completion.
            // This means code just had to know that 30 usecs had elapsed, there was no way to check.
            // Just complete immediately.
            // Yes, not historically accurate, but neither is using a mouse for a lightpen.
            noWait = true;
            pollState = IDLE;
            iotCondLog(LOG_SDB,"Sdb pdp1P->mb %06o, completion %d\n", pdp1P->mb, completion);
        }
        else
        {
            delayTime = MOVEDELAY;
            pollState = DEFLECTION;
        }
    }

    if( noWait && needCompletion )
    {
        needCompletion = false;
        IOCOMPLETE(pdp1P);                  // no completion pulse if noWait
    }

    if( delayTime )
    {
        enablePolling( USTOCYCLES(delayTime) );
    }
    else
    {
        enablePolling(0);
    }

    return(1);
}

void
iotStart()
{
    iotCondLog(LOG_START, "IOT 27 started\n");
    configure();
}

void
iotStop()
{
    iotCloseLog();
}

// Actually put out our dots
void
iotPoll(PDP1 *pdp1P)
{
    iotCondLog(LOG_POLL, "IOT 7 poll x %d y %d intensity %d\n",
        pdp1P->curDispX, pdp1P->curDispY, pdp1P->curDispIntensity);

    if( pollState == DEFLECTION )
    {
        pollState = DRAW;
        enablePolling(USTOCYCLES(DRAWDELAY));           // a bit silly, but this is the actual timing
        return;
    }

    // This is when the dot is actually sent.
    display(pdp1P, 0, pdp1P->curDispX, pdp1P->curDispY, pdp1P->curDispIntensity);

    // There was a 2 screen implementation somewhere that used a hack with the intensity
    // to select the second screen.
    // This of course breaks a lot of other stuff, doubtful it even works correctly.
    if( twoscreensEnabled && (pdp1P->curDispIntensity & 4) )
    {
        // unclear what's happening here exactly
        // spacewar 4.4 uses only intensity 0/4
        pdp1P->curDispIntensity &= 3;
        display(pdp1P, 1, pdp1P->curDispX, pdp1P->curDispY, pdp1P->curDispIntensity);
    }

    if( lightpenEnabled && checkLightpen(pdp1P, pdp1P->curDispX, pdp1P->curDispY) )
    {
        iotCondLog(LOG_LIGHTPEN, "Lightpen hit at x %d y %d\n", pdp1P->curDispX, pdp1P->curDispY);
    }

    if( needCompletion )
    {
        needCompletion = false;
        IOCOMPLETE(pdp1P);
    }

    pollState = IDLE;
    enablePolling(0); 
}

// Get our configurations settings, can be called more than once.
void
configure()
{
ConfigurationP confP;
ConfigurationSettingP settingP;

    iotCondLog(LOG_CONFIG, "IOT 7 checking configuration\n");
    lightpenEnabled = sdbEnabled = dpyShiftEnabled = twoscreensEnabled = false;

    if( (settingP = findConfigurationSetting(getConfiguration(), "lightpen")) )
    {
        iotCondLog(LOG_CONFIG, "IOT 7 lightpen is enabled\n");
        lightpenEnabled = settingP->onOff;
    }

    if( (settingP = findConfigurationSetting(getConfiguration(), "sdb")) )
    {
        iotCondLog(LOG_CONFIG, "IOT 7 sdb is enabled\n");
        sdbEnabled = settingP->onOff;
    }

    // sdb takes priority
    if( !sdbEnabled && (settingP = findConfigurationSetting(getConfiguration(), "dpyshift")) )
    {
        iotCondLog(LOG_CONFIG, "IOT 7 dpy shift is enabled\n");
        dpyShiftEnabled = settingP->onOff;
    }

    if( (settingP = findConfigurationSetting(getConfiguration(), "aperture")) )
    {
        penAperture = settingP->ivalue;
        penRadius2 = (penAperture/2) * (penAperture/2);  // radius squared
        iotCondLog(LOG_CONFIG, "IOT 7 aperture %d\n", settingP->ivalue);
    }

    if( (settingP = findConfigurationSetting(getConfiguration(), "twoscreens")) )
    {
        iotCondLog(LOG_CONFIG, "IOT 7 second screen is enabled\n");
        twoscreensEnabled = settingP->onOff;
    }
}
