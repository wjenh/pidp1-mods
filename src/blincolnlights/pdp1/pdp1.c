/*
 * This PDP-1 emulator was written originally by Angelo Papenhoff, aap.
 * It has been modified by Bill Ezell, wje, pdp1@quackers.net to:
 * Add new features.
 * Make it readable. :)
 * It uses the One True Formatting Style, keep it.
 * The formatting is based on research done at Stanford many years ago that determined the major causes
 * of coding errors, the formatting reduced that. It works.
 *
 * wje 05-Jan-26 break from original repo, now independent. Initial reformatting.
 *    Prior to this break, dynamic IOTs, new audio, high speed channel support, light pen support,
 *    and other minor changes were done.
 * wje 07-Feb-26 sdb does not draw a dot, only sets position and intensity
 *    Special dpy mod, dpy 3000, sets lightpen enable and the aperture from IO
 * wje 08-Feb-26 rework lightpen, was overthought, now much more accurate
 * wje 10-Feb-26 work around poll() returning data ready when it's not
 * wje 10-Feb-26 style cleanup, remove conditionals for light pen, origin shift, lai, lia
 * wje 18-Feb-26 massive cleanup, add shm support
*/
#include "common.h"
#include "pdp1.h"
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <pthread.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>

//#define DOLOGGING
#include "logger.h"
// Set desired log type to 1 to enable output assuming logging is defined.
#define LOG_LP 0
#define LOG_APERTURE 0
#define LOG_STARTUP 0

#define NOTIOTH
#include "dynamicIots.h"

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

#define APERTURE 6                  // the default, 0.050"
#define PENBUFSIZE  64              // read up to this many commands at once

int penAperture = APERTURE;
int penRadius2 = (APERTURE / 2) * (APERTURE / 2); // radius squared
bool lightpenEnabled = false;       // we always expose these for loadConfig in main.c
bool sdbEnabled = true;
bool dpyShiftEnabled = false;
bool audioEnabled = false;

#define B0 0400000
#define B1 0200000
#define B2 0100000
#define B3 0040000
#define B4 0020000
#define B5 0010000
#define B6 0004000
#define B7 0002000
#define B8 0001000
#define B9 0000400
#define B10 0000200
#define B11 0000100
#define B12 0000040
#define B13 0000020
#define B14 0000010
#define B15 0000004
#define B16 0000002
#define B17 0000001

#define US(us) ((us)*1000 - 1)
#define RDLY US(2500)       // 400/s
#define PDLY US(15873)      // 63/s
#define TYODLY US(100000)   // has to be long enough for MACRO to work

#define RD_CHAN 1
#define PUN_CHAN 6
#define TTI_CHAN 7
#define TTO_CHAN 8

static bool penDown;
static int lastPenX;
static int lastPenY;
static pthread_mutex_t lightpenLock;

int lightpenListener(PDP1P pdp1P);
static char *onOff(bool flag);
static void iot_pulse(PDP1P pdp1P, int pulse, int dev, int nac);
static void iot(PDP1P pdp1P, int pulse);
static bool checkLightPen(PDP1P pdp1P);

// The emulator duplicates all of the original hardware
// subcycles. Impressive.
// TP   length, ns  end tine in main cycle
//  0   200         200
//  1   300         500
//  2   550         1050
//  3   300         1350
//  4   200         1550
//  5   250         1800
//  6   250         2050
//  6a  400         2450
//  7   200         2650
//  8   200         2850
//  9   1200        4050
//  9a  750         4800
//  10  200         5000

enum
{
    TP0_end = 200,
    TP1_end = 500,
    TP2_end = 1050,
    TP3_end = 1350,
    TP4_end = 1550,
    TP5_end = 1800,
    TP6_end = 2050,
    TP6a_end = 2450,
    TP7_end = 2650,
    TP8_end = 2850,
    TP9_end = 4050,
    TP9a_end = 4800,
    TP10_end = 5000,
    TP_unreachable = TP10_end
};

#define TP(n) if(pdp1P->timernd < TP##n##_end) { updatelights(pdp1P, pdp1P->panel); pdp1P->timernd = TP_unreachable; }

static void
readmem(PDP1P pdp1P)
{
    MB |= pdp1P->core[(pdp1P->ema | MA) % MAXMEM];
    pdp1P->core[(pdp1P->ema | MA) % MAXMEM] = 0;
}

static void
writemem(PDP1P pdp1P)
{
    pdp1P->core[(pdp1P->ema | MA) % MAXMEM] = MB;
}

static void mop2379(PDP1P pdp1P)
{
    pdp1P->i = pdp1P->w;
    pdp1P->w = pdp1P->rs;
    pdp1P->rs = pdp1P->r;
    pdp1P->r = !pdp1P->w;   // actually !pdp1P->rs but already clobbered
}

static void inhibit(PDP1P pdp1P)
{
    pdp1P->i = 1;
}

static void memclr(PDP1P pdp1P)
{
    pdp1P->r = 0;
    pdp1P->rs = 0;
    pdp1P->w = 0;
    pdp1P->i = 0;
}

// Initialize everything to random values on power-on.
void
pwrclr(PDP1P pdp1P)
{
    IR = rand() & 077;
    PC = rand() & 07777;
    MA = rand() & 07777;
    MB = rand() & 0777777;
    AC = rand() & 0777777;
    IO = rand() & 0777777;

    pdp1P->cyc = rand() & 1;
    pdp1P->df1 = rand() & 1;
    pdp1P->df2 = rand() & 1;
    pdp1P->bc = rand() & 3;
    pdp1P->ov1 = rand() & 1;
    pdp1P->ov2 = rand() & 1;
    pdp1P->rim = rand() & 1;
    pdp1P->sbm = rand() & 1;
    pdp1P->ioc = rand() & 1;
    pdp1P->ihs = rand() & 1;
    pdp1P->ios = rand() & 1;
    pdp1P->ioh = rand() & 1;
    pdp1P->pf = rand() & 077;
    memclr(pdp1P);

    if(pdp1P->sbs16)
    {
        pdp1P->b4 = rand() & 0177777;
        pdp1P->b3 = rand() & 0177777;
        pdp1P->b2 = rand() & 0177777;
        pdp1P->b1 = rand() & 0177777;
    }
    else
    {
        pdp1P->b4 = rand() & 1;
        pdp1P->b3 = rand() & 1;
        pdp1P->b2 = rand() & 1;
        pdp1P->b1 = rand() & 1;
    }

    pdp1P->req = 0;

    pdp1P->emc = rand() & 1;
    pdp1P->exd = rand() & 1;
    pdp1P->ema = rand() & EXTMASK;
    pdp1P->epc = rand() & EXTMASK;

    pdp1P->rc = 0;
    pdp1P->rby = 0;
    pdp1P->rcl = 0;
    pdp1P->r_time = NEVER;
    pdp1P->rim_return = 0;
    pdp1P->rim_cycle = 0;

    pdp1P->punon = 0;
    pdp1P->p_time = NEVER;
    pdp1P->feed_time = 0;

    pdp1P->tbs = 0;
    pdp1P->tbb = 0;
    pdp1P->tyo = 0;
    pdp1P->typ_time = NEVER;
    pdp1P->tyi_wait = 0;

    pdp1P->dpy_defl_time = NEVER;
    pdp1P->dpy_time = NEVER;

    logger(LOG_STARTUP, "Sdb is %s, dpy shift is %s, sbs16 is %s, audio is %s\n",
           onOff(sdbEnabled), onOff(dpyShiftEnabled), onOff(pdp1P->sbs16), onOff(audioEnabled));
    logger(LOG_STARTUP, "Lightpen is %s, aperture is %d, radius^2 is %d\n", onOff(lightpenEnabled), penAperture,
           penRadius2);
}

static char *
onOff(bool flag)
{
    return((flag) ? "on" : "off");
}

static void
mul_shift(PDP1P pdp1P)
{
    int ac;

    ac = AC >> 1;
    IO = (AC & B17) << 17 | IO >> 1;
    AC = ac;
}

static void
div_shift(PDP1P pdp1P)
{
    int ac;

    ac = (AC&~B0) << 1 | (IO & B0) >> 17;
    IO = (IO&~B0) << 1 | (~AC & B0) >> 17;
    AC = ac;
}

static void
carry(PDP1P pdp1P)
{
    AC += (~AC & MB) << 1;

    if(AC & 01000000)
    {
        AC++;
    }

    AC &= WORDMASK;
}

static void
inc_ac(PDP1P pdp1P)
{
    if(AC == 0777776)
    {
        AC = 0;
    }
    else if(AC == 0777777)
    {
        AC = 1;
    }
    else
    {
        AC++;
    }
}
static void
pc_to_ac(PDP1P pdp1P)
{
    AC |= pdp1P->ov1 << 17 | pdp1P->exd << 16 | pdp1P->epc | PC;
}

static void
clr_ma(PDP1P pdp1P)
{
    MA = 0;
    pdp1P->ema = 0;
}

static void
mb_to_ma(PDP1P pdp1P)
{
    MA |= MB & ADDRMASK;

    if(pdp1P->emc)
    {
        pdp1P->ema |= MB & EXTMASK;
    }
    else
    {
        pdp1P->ema |= pdp1P->epc;
    }
}

static void
pc_to_ma(PDP1P pdp1P)
{
    MA |= PC;
    pdp1P->ema |= pdp1P->epc;
}


static void
clr_pc(PDP1P pdp1P)
{
    PC = 0;
    pdp1P->epc = 0;
}

static void
mb_to_pc(PDP1P pdp1P)
{
    PC |= MB & ADDRMASK;

    if(pdp1P->emc)
    {
        pdp1P->epc |= MB & EXTMASK;
    }
    else
    {
        pdp1P->epc |= pdp1P->ema;
    }
}

static void
ma_to_pc(PDP1P pdp1P)
{
    PC |= MA;
    pdp1P->epc |= pdp1P->ema;
}

static void
pc_inc(PDP1P pdp1P)
{
    PC = (PC + 1) & ADDRMASK;
}

static void
inst_cancel(PDP1P pdp1P)
{
    PC = (PC - 1) & ADDRMASK;
    pdp1P->df1 = 0;
    pdp1P->df2 = 0;
}

static void
sbs_calc_req(PDP1P pdp1P)
{
    pdp1P->req = (pdp1P->b3 & ~pdp1P->b3 + 1) & (pdp1P->b4 & ~pdp1P->b4 + 1) - 1;
}

static void
clr_sbs(PDP1P pdp1P)
{
    pdp1P->b4 = 0;
    pdp1P->b3 = 0;

    if(pdp1P->sbs16)
    {
        pdp1P->b2 = 0;
    }

    sbs_calc_req(pdp1P);
}

static void
hold_break(PDP1P pdp1P)
{
    pdp1P->b4 |= pdp1P->req;

    if(!pdp1P->sbs16)
    {
        pdp1P->b2 = 0;
    }

    sbs_calc_req(pdp1P);
}

static void
sbs_sync(PDP1P pdp1P)
{
    pdp1P->b3 |= pdp1P->b2;
    sbs_calc_req(pdp1P);
}

static void
sbs_reset_sync(PDP1P pdp1P)
{
    if(pdp1P->sbs16)
    {
        pdp1P->b2 &= ~pdp1P->b3;
    }
}

static void
sc(PDP1P pdp1P)
{
    pdp1P->df1 = 0;
    pdp1P->df2 = 0;
    pdp1P->bc = 0;
    pdp1P->ov1 = 0;
    pdp1P->ov2 = 0;
    pdp1P->ihs = 0;
    pdp1P->ioc = 1;
    pdp1P->ios = 0;
    pdp1P->ioh = 0;
    pdp1P->ir = 0;
    clr_pc(pdp1P);
    pdp1P->sbm = pdp1P->sbm_start_sw;
    clr_sbs(pdp1P);
    pdp1P->b2 = 0;

    pdp1P->lai = 0;
    pdp1P->lia = 0;
    pdp1P->emc = 0;
    pdp1P->exd = 0;
    pdp1P->tyo = 0;
}

void
spec(PDP1P pdp1P)
{
    // PB
    pdp1P->run = 0;

    if(pdp1P->start_sw || pdp1P->deposit_sw || pdp1P->examine_sw)
    {
        pdp1P->rim = 0;
    }

    // SP1
    pdp1P->run_enable = 1;
    clr_ma(pdp1P);

    if(pdp1P->start_sw)
    {
        pdp1P->cyc = 0;
    }

    if(pdp1P->deposit_sw)
    {
        pdp1P->ac = 0;
    }

    if(pdp1P->start_sw || pdp1P->deposit_sw || pdp1P->examine_sw)
    {
        sc(pdp1P);
    }

    // SP2
    if(pdp1P->start_sw || pdp1P->deposit_sw || pdp1P->examine_sw)
    {
        PC |= pdp1P->ta;
        pdp1P->epc |= pdp1P->eta;
    }

    if(pdp1P->deposit_sw || pdp1P->examine_sw || pdp1P->rim)
    {
        pdp1P->cyc = 1;
    }

    if(pdp1P->examine_sw)
    {
        pdp1P->ir |= 020 >> 1;
    }

    if(pdp1P->deposit_sw)
    {
        pdp1P->ir |= 024 >> 1;
        AC |= pdp1P->tw;
    }

    // SP3
    if(pdp1P->deposit_sw || pdp1P->examine_sw)
    {
        pc_to_ma(pdp1P);
        pdp1P->cychack = 1;
    }

    if(pdp1P->start_sw && pdp1P->extend_sw)
    {
        pdp1P->exd = 1;
    }

    // SP4
    if(pdp1P->start_sw || pdp1P->continue_sw)
    {
        pdp1P->run = 1;
    }
}

void
start_readin(PDP1P pdp1P)
{
    // PB
    pdp1P->run = 0;
    pdp1P->rim = 1;
    pdp1P->sbm = 0;

    pdp1P->rim_cycle = 1;
}

void
readin1(PDP1P pdp1P)
{
    pdp1P->rim_cycle = 0;

    // SP1
    pdp1P->run_enable = 1;
    clr_ma(pdp1P);

    if(pdp1P->rim)
        // guaranteed
    {
        MB = 0;
        sc(pdp1P);
        iot_pulse(pdp1P, 1, 2, 0);
    }
}

void
readin2(PDP1P pdp1P)
{
    // SP2
    pdp1P->cyc = 1;
    MB |= IO;
    pdp1P->epc |= pdp1P->eta;

    // SP3
    IR |= MB >> 13;
    mb_to_ma(pdp1P);

    if(pdp1P->extend_sw)
    {
        pdp1P->exd = 1;
    }

    // SP4
    if(IR_DIO)
    {
        iot_pulse(pdp1P, 1, 2, 0);
    }

    if(IR_JMP)
    {
        pdp1P->run = 1;
        pdp1P->cyc = 0;
        pdp1P->rim = 0;
        mb_to_pc(pdp1P);
        pdp1P->cychack = 1;   // actually TP0 should work too
    }
}

static void
clrmd(PDP1P pdp1P)
{
    pdp1P->scr = 0;
    pdp1P->smb = 0;
    pdp1P->srm = 0;
}

static void
multiply(PDP1P pdp1P)
{
    int lastlong;

    pdp1P->simtime += 150;
    goto start;

    do
    {
        if(lastlong = (IO & B17))
        {
            // MDP-3
            AC ^= MB;
            pdp1P->simtime += 200;

            // MDP-4
            carry(pdp1P);
            pdp1P->simtime += 500;
        }

        // MDP-5
        pdp1P->scr = (pdp1P->scr + 1) & 037;
        mul_shift(pdp1P);
        pdp1P->simtime += 150;

start:
        // MDP-6
        // if(scr==021) MD_RESTART
        // but we run synchronously
        ;
    }
    while(pdp1P->scr != 022);

    // last cycle is asynchronous
    pdp1P->simtime -= lastlong * (200 + 500) + 150;

    // MDP-11
    if(pdp1P->srm != pdp1P->smb && !(AC == 0 && IO == 0))
    {
        AC ^= WORDMASK;
        IO ^= WORDMASK;
    }
}

static void
divide(PDP1P pdp1P)
{
    int t;
    int done;

    pdp1P->simtime += 150;
    goto start;

    do
    {
        // MDP-1
        if(!(AC & B0))
        {
            MB ^= WORDMASK;
        }

        div_shift(pdp1P);
        pdp1P->simtime += 150;

        if(!(IO & B17))
        {
            // MDP-2
            inc_ac(pdp1P);
            pdp1P->simtime += 500;
        }

start:
        // MDP-3
        AC ^= MB;
        pdp1P->simtime += 200;

        // MDP-4
        if(AC == 0777777)
        {
            AC ^= WORDMASK;
        }
        else
        {
            carry(pdp1P);
        }

        // delayed 0.05us
        if(MB & B0)
        {
            MB ^= WORDMASK;
        }

        pdp1P->simtime += 500;

        // MDP-5
        done = pdp1P->scr == 022 || pdp1P->scr == 0 && !(AC & B0);
        pdp1P->scr = (pdp1P->scr + 1) & 037;
    }
    while(!done);

    // MDP-7
    AC ^= MB;

    if(pdp1P->scr & 2)
    {
        pc_inc(pdp1P);
    }

    pdp1P->simtime += 150;

    // MDP-8
    if(AC == 0777777)
    {
        AC ^= WORDMASK;
    }
    else
    {
        carry(pdp1P);
    }

    pdp1P->simtime += 500;

    // MDP-9
    if(pdp1P->scr & 020)
    {
        AC >>= 1;
    }

    // MD_RESTART, but we run synchronously
    // so don't add to simtime from now on

    // MDP-10
    if(!(pdp1P->scr & 020) && pdp1P->srm ||
            (pdp1P->scr & 020) && IO != 0 && pdp1P->srm != pdp1P->smb)
    {
        IO ^= WORDMASK;
    }

    if(AC != 0 && pdp1P->srm)
    {
        AC ^= WORDMASK;
    }

    MB = 0;

    // swap AC and IO
    if(pdp1P->scr & 020)
    {
        // MDP-12
        MB |= IO;

        // MDP-13
        t = MB;
        MB = AC;
        AC = t;
        IO = 0;

        // MDP-14
        IO |= MB;
    }
}

static int
decflg(int n)
{
    switch(n & 7)
    {
    case 1:
        return 040;

    case 2:
        return 020;

    case 3:
        return 010;

    case 4:
        return 004;

    case 5:
        return 002;

    case 6:
        return 001;

    case 7:
        return 077;
    }

    return 0;
}

static void
shro(PDP1P pdp1P)
{
    int ac, io;

    ac = AC;
    io = IO;

    switch((MB >> 9) & 017)
    {
    case 001:   // RAL
        ac = (AC&~B0) << 1 | (AC & B0) >> 17;
        break;

    case 002:   // RIL
        io = (IO&~B0) << 1 | (IO & B0) >> 17;
        break;

    case 003:   // RCL
        ac = (AC&~B0) << 1 | (IO & B0) >> 17;
        io = (IO&~B0) << 1 | (AC & B0) >> 17;
        break;

    case 005:   // SAL
        ac = (AC & B0) | (AC&~(B0 | B1)) << 1 | (AC & B0) >> 17;
        break;

    case 006:   // SIL
        io = (IO & B0) | (IO&~(B0 | B1)) << 1 | (IO & B0) >> 17;
        break;

    case 007:   // SCL
        ac = (AC & B0) | (AC&~(B0 | B1)) << 1 | (IO & B0) >> 17;
        io = (IO&~B0) << 1 | (AC & B0) >> 17;
        break;

    case 011:   // RAR
        ac = (AC & B17) << 17 | AC >> 1;
        break;

    case 012:   // RIR
        io = (IO & B17) << 17 | IO >> 1;
        break;

    case 013:   // RCR
        ac = (IO & B17) << 17 | AC >> 1;
        io = (AC & B17) << 17 | IO >> 1;
        break;

    case 015:   // SAR
        ac = (AC & B0) | AC >> 1;
        break;

    case 016:   // SIR
        io = (IO & B0) | IO >> 1;
        break;

    case 017:   // SCR
        ac = (AC & B0) | AC >> 1;
        io = (AC & B17) << 17 | IO >> 1;
        break;
    }

    AC = ac;
    IO = io;
}

#define CY0_INST_DONE (!pdp1P->df1 && pdp1P->ir >= 030)
#define CY0_MIDBRK_PERMIT (pdp1P->ir < 030)
#define DF_INST_DONE (!pdp1P->df2 && pdp1P->ir >= 030)
#define DF_MIDBRK_PERMIT (pdp1P->ir < 030 || (IR_JMP || IR_JSP) && pdp1P->df2)

static void
syncov(PDP1P pdp1P)
{
    if(pdp1P->ov2)
    {
        pdp1P->ov1 = 1;
    }

    pdp1P->ov2 = 0;
}

static void
cycle0(PDP1P pdp1P)
{
    int hack;

    hack = pdp1P->cychack;
    pdp1P->cychack = 0;

    switch(hack)
    {
    default
            :

        // TP0
        if(IR_SHRO && (MB & B12))
        {
            shro(pdp1P);
        }

        if(pdp1P->lai)
        {
            MB |= IO;
        }

        pc_to_ma(pdp1P);
        TP(0)

    case 1:

        // TP1
        if(IR_SHRO && (MB & B11))
        {
            shro(pdp1P);
        }

        if(pdp1P->lai && pdp1P->lia)
        {
            int t = MB;
            MB = AC;
            AC = t;
            IO = 0;
        }
        else
        {
            if(pdp1P->lia)
            {
                MB = AC;
                IO = 0;
            }

            if(pdp1P->lai)
            {
                AC = MB;
            }
        }

        pdp1P->emc = 0;

        TP(1)

        // TP2
        mop2379(pdp1P);

        if(IR_SHRO && (MB & B10))
        {
            shro(pdp1P);
        }

        pc_inc(pdp1P);

        if(IR_IOT)
        {
            pdp1P->ioc = !pdp1P->ioh && !pdp1P->ihs;
        }

        pdp1P->ihs = 0;

        if(pdp1P->lia)
        {
            IO |= MB;
        }

        TP(2)

        // TP3
        mop2379(pdp1P);

        if(IR_SHRO && (MB & B9))
        {
            shro(pdp1P);
        }

        MB = 0;
        TP(3)

    case 4:
        // TP4
        sbs_sync(pdp1P);
        readmem(pdp1P);
        IR = 0;
        TP(4)

        // TP5
        IR |= MB >> 13;
        pdp1P->lai = 0;
        pdp1P->lia = 0;
        TP(5)

        // TP6
        if((MB & B5) && !IR_SHRO && !IR_SKIP &&
                !IR_LAW && !IR_OPR && !IR_IOT && !IR_CALJDA)
        {
            pdp1P->df1 = 1;
        }

        TP(6)

        // TP6a
        if(IR_IOT && !(MB & B5) && pdp1P->ioh)
        {
            pdp1P->ioc = 1;
            pdp1P->ihs = 1;
            pdp1P->ioh = 0;
        }

        TP(6a)

        // TP7
        mop2379(pdp1P);

        if(IR_SHRO && (MB & B17))
        {
            shro(pdp1P);
        }

        if(IR_JSP && !pdp1P->df1 || IR_LAW || IR_OPR && (MB & B10))
        {
            AC = 0;
        }

        if(IR_OPR && (MB & B6))
        {
            IO = 0;
        }

        if(IR_IOT)
        {
            if((MB & B5) && !pdp1P->ioh && !pdp1P->ihs)
            {
                pdp1P->ioh = 1;
            }

            if(pdp1P->ioc)
            {
                iot(pdp1P, 0);
            }
        }

        TP(7)

        // TP8
        inhibit(pdp1P);

        if(!pdp1P->df1)
        {
            if(IR_JSP)
            {
                pc_to_ac(pdp1P), clr_pc(pdp1P);
            }

            if(IR_JMP)
            {
                clr_pc(pdp1P);
            }
        }

        if(IR_SKIP)
        {
            int skip = 0;

            if((MB & B6) && IO)
            {
                skip = 1;    // wje - pdp1P-1D sni, skip on nonzero IO
            }

            if((MB & B7) && !(IO & B0))
            {
                skip = 1;
            }

            if((MB & B8) && !pdp1P->ov1)
            {
                skip = 1;
            }

            if((MB & B9) && (AC & B0))
            {
                skip = 1;
            }

            if((MB & B10) && !(AC & B0))
            {
                skip = 1;
            }

            if((MB & B11) && AC == 0)
            {
                skip = 1;
            }

            if((MB & 070) && !(pdp1P->ss & decflg(MB >> 3)))
            {
                skip = 1;
            }

            if((MB & 007) && !(pdp1P->pf & decflg(MB)))
            {
                skip = 1;
            }

            if(MB & B5)
            {
                skip = !skip;
            }

            if(skip)
            {
                pc_inc(pdp1P);
            }
        }

        if(IR_SHRO && (MB & B16))
        {
            shro(pdp1P);
        }

        if(IR_LAW)
        {
            AC |= MB & 0007777;
        }

        if(IR_OPR)
        {
            if(MB & B5)
            {
                IO = ~IO;    // wje - pdp1P-1D cmi, complement IO
            }

            if(MB & B7)
            {
                AC |= pdp1P->tw;
            }

            if(MB & B11)
            {
                pc_to_ac(pdp1P);
            }

            if(MB & B12)
            {
                pdp1P->lai = 1;
            }

            if(MB & B13)
            {
                pdp1P->lia = 1;
            }

            if(MB & B14)
            {
                pdp1P->pf |= decflg(MB);
            }
            else
            {
                pdp1P->pf &= ~decflg(MB);
            }
        }

        TP(8)

        // TP9
        mop2379(pdp1P);
        writemem(pdp1P);      // approximate

        if(!pdp1P->df1 && (IR_JMP || IR_JSP))
        {
            mb_to_pc(pdp1P);
        }

        if(IR_SKIP && (MB & B8))
        {
            pdp1P->ov1 = 0;
        }

        if(IR_SHRO && (MB & B15))
        {
            shro(pdp1P);
        }

        if(IR_OPR && (MB & B8) || IR_LAW && (MB & B5))
        {
            AC ^= WORDMASK;
        }

        if(IR_IOT && !pdp1P->ihs && pdp1P->ios)
        {
            pdp1P->ioh = 0;
        }

        if(IR_OPR && (MB & B9) ||
                IR_INCORR ||
                pdp1P->single_cyc_sw ||
                pdp1P->single_inst_sw && CY0_INST_DONE ||
                !pdp1P->run_enable)
        {
            pdp1P->run = 0;
        }

        clrmd(pdp1P);
        TP(9)

        // TP9A
        if(IR_SHRO && (MB & B14))
        {
            shro(pdp1P);
        }

        if(IR_IOT && pdp1P->ioh)
        {
            inst_cancel(pdp1P);
        }

        TP(9a)

        // TP10
        sbs_reset_sync(pdp1P);
        memclr(pdp1P);
        syncov(pdp1P);

        if(pdp1P->run)
        {
            clr_ma(pdp1P);
        }

        if(pdp1P->df1 || pdp1P->ir < 030)
        {
            pdp1P->cyc = 1;
        }

        if(pdp1P->sbm && pdp1P->req)
        {
            if(CY0_INST_DONE || CY0_MIDBRK_PERMIT)
            {
                pdp1P->cyc = 1;
                pdp1P->bc |= 1;

                if(CY0_MIDBRK_PERMIT)
                {
                    inst_cancel(pdp1P);
                }

                assert(pdp1P->cyc && pdp1P->bc == 1 && !pdp1P->df1 && !pdp1P->df2);
            }
        }

        if(IR_SHRO && (MB & B13))
        {
            shro(pdp1P);
        }

        if(IR_IOT)
        {
            if(pdp1P->ihs)
            {
                pdp1P->ioh = 1;
            }
            else if(!pdp1P->ioh)
            {
                pdp1P->ios = 0;
            }

            if(pdp1P->ioc)
            {
                iot(pdp1P, 1);
            }
        }

        if(MB & B0)
        {
            pdp1P->smb = 1;
        }

        if(pdp1P->lai)
        {
            MB = 0;
        }

        TP(10)
    }
}

static void
defer(PDP1P pdp1P)
{
    int sbs_restore = 0;
    int mask = 0;

    // TP0
    mb_to_ma(pdp1P);
    TP(0)

    // TP1
    pdp1P->emc = 0;
    TP(1)

    // TP2
    mop2379(pdp1P);

    if(pdp1P->sbm && IR_JMP && pdp1P->epc == 0)
    {
        if(pdp1P->sbs16)
        {
            if((MB & 07703) == 1)
            {
                mask = ~(1 << ((MB & 074) >> 2));
                pdp1P->b4 &= mask;
                pdp1P->b3 &= mask;   // wje fix sbs16 not clearing
                pdp1P->exd = 1;
                sbs_restore = 1;
            }
        }
        else
        {
            if((MB & 07777) == 1)
            {
                pdp1P->b3 = 0;
                pdp1P->b4 = 0;
                pdp1P->exd = 1;
                sbs_restore = 1;
            }
        }

        sbs_calc_req(pdp1P);
    }

    TP(2)

    // TP3
    mop2379(pdp1P);
    MB = 0;
    TP(3)

    // TP4
    sbs_sync(pdp1P);
    readmem(pdp1P);
    TP(4)

    // TP5
    if(pdp1P->exd)
    {
        pdp1P->emc = 1;
    }

    TP(5)

    if(MB & B5 && !pdp1P->exd)
    {
        // TP6
        pdp1P->df2 = 1;
        TP(6)
        TP(6a)
        mop2379(pdp1P);
        TP(7)
        inhibit(pdp1P);
        TP(8)
    }
    else
    {
        TP(6)
        TP(6a)

        // TP7
        if(IR_JSP)
        {
            AC = 0;
        }

        mop2379(pdp1P);
        TP(7)

        // TP8
        inhibit(pdp1P);

        if(IR_JSP)
        {
            pc_to_ac(pdp1P), clr_pc(pdp1P);
        }

        if(IR_JMP)
        {
            clr_pc(pdp1P);
        }

        TP(8)

        // TP9
        if(IR_JSP || IR_JMP)
        {
            mb_to_pc(pdp1P);
        }

        clrmd(pdp1P);
    }

    // TP9
    mop2379(pdp1P);
    writemem(pdp1P);      // approximate

    if(IR_INCORR ||
            pdp1P->single_cyc_sw ||
            pdp1P->single_inst_sw && DF_INST_DONE ||
            !pdp1P->run_enable)
    {
        pdp1P->run = 0;
    }

    TP(9)

    // 3.5us after TP2, shortly before TP9A
    if(sbs_restore)
    {
        pdp1P->ov1 = !!(MB & B0);
        pdp1P->exd = !!(MB & B1);
    }

    TP(9a)

    // TP10
    sbs_reset_sync(pdp1P);
    memclr(pdp1P);
    syncov(pdp1P);

    if(pdp1P->run)
    {
        clr_ma(pdp1P);
    }

    if(!pdp1P->df2)
    {
        pdp1P->df1 = 0;

        if(pdp1P->ir >= 030)
        {
            pdp1P->cyc = 0;
        }
    }

    if(pdp1P->sbm && pdp1P->req)
    {
        if(DF_INST_DONE || DF_MIDBRK_PERMIT)
        {
            pdp1P->cyc = 1;
            pdp1P->bc |= 1;

            if(DF_MIDBRK_PERMIT)
            {
                inst_cancel(pdp1P);
            }

            assert(pdp1P->cyc && pdp1P->bc == 1 && !pdp1P->df1 && !pdp1P->df2);
        }
    }

    pdp1P->df2 = 0;

    if(MB & B0)
    {
        pdp1P->smb = 1;
    }

    TP(10)
}

static void
cycle1(PDP1P pdp1P)
{
    int hack;

    hack = pdp1P->cychack;
    pdp1P->cychack = 0;

    switch(hack)
    {
    default
            :

        // TP0
        if(IR_CALJDA && !(MB & B5))
        {
            MA |= 0100;

            if(!pdp1P->exd)
            {
                pdp1P->ema |= pdp1P->epc;
            }
        }
        else
        {
            mb_to_ma(pdp1P);
        }

        // EMA stuff
        if(IR_DIS)
        {
            div_shift(pdp1P);
        }

        TP(0)

    case 1:
        // TP1
        pdp1P->emc = 0;
        TP(1)

        // TP2
        mop2379(pdp1P);

        if(IR_DIS && !(IO & B17))
        {
            if(AC == 0777777)
            {
                AC = 1;
            }
            else
            {
                AC++;
            }
        }

        if(IR_MUL)
        {
            MB = AC;
            IO = 0;
        }

        TP(2)

        // TP3
        mop2379(pdp1P);

        if(IR_MUL)
        {
            IO |= MB;
        }

        MB = 0;

        if(IR_XCT)
        {
            pdp1P->cyc = 0;
            pdp1P->cychack = 4;
            cycle0(pdp1P);
            return;
        }

        TP(3)

        // TP4
        sbs_sync(pdp1P);
        readmem(pdp1P);

        if(IR_SUB || IR_DIS && (IO & B17))
        {
            AC ^= WORDMASK;
        }

        if(IR_LIO)
        {
            IO = 0;
        }

        TP(4)

        // TP5
        if(IR_AND)
        {
            AC &= MB;
        }

        if(IR_IOR)
        {
            AC |= MB;
        }

        if(IR_IDX || IR_ISP || IR_LAC)
        {
            AC = MB;
        }

        if(IR_DIO || IR_DZM)
        {
            MB = 0;
        }

        if(IR_LIO)
        {
            IO |= MB;
        }

        if((IR_ADD || IR_SUB) && (AC & B0) == (MB & B0))
        {
            pdp1P->ov2 = 1;
        }

        if(IR_XOR || IR_ADD || IR_SUB || IR_SAD || IR_SAS ||
                IR_DIS || IR_MUS && (IO & B17))
        {
            AC ^= MB;
        }

        TP(5)

        // TP6
        if(IR_ADD || IR_SUB || IR_DIS || IR_MUS && (IO & B17))
        {
            carry(pdp1P);
        }

        if(IR_IDX || IR_ISP)
        {
            inc_ac(pdp1P);
        }

        TP(6)
        TP(6a)

        // TP7
        mop2379(pdp1P);

        if(IR_DAC || IR_IDX || IR_ISP)
        {
            MB = AC;
        }

        if(IR_CALJDA)
        {
            MB = AC, AC = 0;
        }

        if(IR_DAP)
        {
            MB = MB & 0770000 | AC & 0007777;
        }

        if(IR_DIP)
        {
            MB = MB & 0007777 | AC & 0770000;
        }

        if(IR_DIO)
        {
            MB = IO;
        }

        TP(7)

        // TP8
        inhibit(pdp1P);

        if(IR_MUS)
        {
            mul_shift(pdp1P);
        }

        if(IR_CALJDA)
        {
            pc_to_ac(pdp1P), clr_pc(pdp1P);
        }

        if(IR_SAS && AC == 0 || IR_SAD && AC != 0 || IR_ISP && !(AC & B0))
        {
            pc_inc(pdp1P);
        }

        TP(8)

        // TP9
        mop2379(pdp1P);
        writemem(pdp1P);      // approximate

        if(IR_CALJDA)
        {
            ma_to_pc(pdp1P);
        }

        if((IR_ADD || IR_SUB) && (AC & B0) == (MB & B0))
        {
            pdp1P->ov2 = 0;
        }

        if(IR_SUB || IR_DIS && (IO & B17))
        {
            AC ^= WORDMASK;
        }

        if(IR_SAD || IR_SAS)
        {
            AC ^= MB;
        }

        if(IR_INCORR ||
                pdp1P->single_cyc_sw ||
                pdp1P->single_inst_sw ||
                !pdp1P->run_enable)
        {
            pdp1P->run = 0;
        }

        clrmd(pdp1P);
        TP(9)

        // TP9A
        if((IR_ADD || IR_DIS) && AC == 0777777)
        {
            AC = 0;
        }

        if(IR_CALJDA)
        {
            pc_inc(pdp1P);
        }

        TP(9a)

        // TP10
        sbs_reset_sync(pdp1P);
        memclr(pdp1P);
        syncov(pdp1P);
        pdp1P->cyc = 0;

        if(pdp1P->run)
        {
            clr_ma(pdp1P);
        }

        if(MB & B0)
        {
            pdp1P->smb = 1;
        }

        if(IR_MUL)
        {
            if(MB & B0)
            {
                MB ^= WORDMASK;
            }

            if(IO & B0)
            {
                IO ^= WORDMASK;
                pdp1P->srm = 1;
            }

            pdp1P->scr |= 1;
            AC = 0;
            multiply(pdp1P);
            // without delay to TP0
            pdp1P->simtime -= 200;
        }

        if(IR_DIV)
        {
            if(!(MB & B0))
            {
                MB ^= WORDMASK;
            }

            if(AC & B0)
            {
                AC ^= WORDMASK;
                IO ^= WORDMASK;
                pdp1P->srm = 1;
            }

            divide(pdp1P);
            // without delay to TP0
            pdp1P->simtime -= 200;
        }

        TP(10)
    }
}

static void
brkcycle(PDP1P pdp1P)
{
    int be;
    int r;

    // TP0
    if(IR_SHRO && (MB & B12))
    {
        shro(pdp1P);
    }

    if(pdp1P->bc == 1 && pdp1P->sbs16)
    {
        be = 0;

        for(r = pdp1P->req; !(r & 1); r >>= 1)
        {
            be++;
        }

        MA |= be << 2;
    }

    if(pdp1P->bc == 2 || pdp1P->bc == 3)
    {
        pc_to_ma(pdp1P);
    }

    TP(0)

    // TP1
    if(IR_SHRO && (MB & B11))
    {
        shro(pdp1P);
    }

    pdp1P->emc = 0;
    TP(1)

    // TP2
    mop2379(pdp1P);

    if(IR_SHRO && (MB & B10))
    {
        shro(pdp1P);
    }

    if(IR_IOT)
    {
        pdp1P->ioc = !pdp1P->ioh && !pdp1P->ihs;
    }

    pdp1P->ihs = 0;
    TP(2)

    // TP3
    mop2379(pdp1P);

    if(IR_SHRO && (MB & B9))
    {
        shro(pdp1P);
    }

    MB = 0;
    TP(3)

    // TP4
    if(pdp1P->bc == 1)
    {
        hold_break(pdp1P);
    }

    sbs_sync(pdp1P);
    readmem(pdp1P);

    if(pdp1P->bc == 1)
    {
        IR = 0;
    }

    TP(4)

    // TP5
    if(pdp1P->bc == 3)
    {
        MB = 0;
    }

    TP(5)

    // TP6
    TP(6)
    TP(6a)

    // TP7
    mop2379(pdp1P);

    if(pdp1P->bc == 1)
    {
        MB = AC, AC = 0;
    }

    if(pdp1P->bc == 2)
    {
        MB = AC;
    }

    if(pdp1P->bc == 3)
    {
        MB |= IO;
    }

    TP(7)

    // TP8
    inhibit(pdp1P);

    if(pdp1P->bc == 1)
    {
        pc_to_ac(pdp1P), clr_pc(pdp1P);
    }

    TP(8)

    // TP9
    mop2379(pdp1P);
    writemem(pdp1P);      // approximate

    if(pdp1P->bc == 1)
    {
        ma_to_pc(pdp1P);
    }

    if(pdp1P->single_cyc_sw ||
            !pdp1P->run_enable)
    {
        pdp1P->run = 0;
    }

    clrmd(pdp1P);
    TP(9)

    // TP9A
    pc_inc(pdp1P);
    TP(9a)

    // TP10
    sbs_reset_sync(pdp1P);
    memclr(pdp1P);

    if(pdp1P->run)
    {
        clr_ma(pdp1P);
    }

    if(MB & B0)
    {
        pdp1P->smb = 1;
    }

    if(pdp1P->bc == 3)
    {
        pdp1P->cyc = 0;
    }

    pdp1P->bc = (pdp1P->bc + 1) & 3;
    TP(10)
}

void
cycle(PDP1P pdp1P)
{
// TODO: these can be wrong after power on
// how do we handle that?
//  assert(pdp1P->cyc || pdp1P->bc==0);
//  assert(!pdp1P->df1 || pdp1P->bc==0);

    pdp1P->timernd = rand() % TP_unreachable;

    // a cycle takes 5 usecs
    if(pdp1P->bc)
    {
        brkcycle(pdp1P);
    }
    else if(!pdp1P->cyc)
    {
        cycle0(pdp1P);
    }
    else if(pdp1P->df1)
    {
        defer(pdp1P);
    }
    else
    {
        cycle1(pdp1P);
    }

    // update any IOTs regardless of cycle type
    dynamicIotProcessorDoPoll(pdp1P);             // wje - handle pseudo-async IOTs
}

// Spin until the 5usec cycle time reached
void
throttle(PDP1P pdp1P)
{
    while(pdp1P->realtime < pdp1P->simtime)
    {
        usleep(1000);
        pdp1P->realtime = gettime();
    }
}

// pulse=0: TP7
// pulse=1: TP10
static void
iot_pulse(PDP1P pdp1P, int pulse, int dev, int nac)
{
    int ch;

    ch = (MB >> 6) & 077;

    switch(dev)
    {
    case 000:
        break;

    case 001:   // rpa
    case 002:   // rpb
        if(pulse)
        {
            pdp1P->rcp = nac;

            if(dev == 00001)
            {
                pdp1P->rby = 0;
                pdp1P->rc = 3;
                pdp1P->rcl ^= 1;
            }
            else
            {
                pdp1P->rby = 1;
                pdp1P->rc = 1;
                pdp1P->rcl = 1;
            }

            pdp1P->r_time = pdp1P->simtime + RDLY;
            pdp1P->rb = 0;
        }

        break;

    case 003:   // tyo
        if(!pulse)
        {
            if(!pdp1P->tyo)
            {
                pdp1P->tb = 0;
            }
        }
        else
        {
            pdp1P->tcp = nac;

            if(!pdp1P->tyo)
            {
                pdp1P->tyo = 1;
                pdp1P->tb |= IO & 077;
                pdp1P->typ_time = pdp1P->simtime + TYODLY;
            }
        }

        break;

    case 004:   // tyi
        if(!pulse)
        {
            IO = 0;
        }
        else
        {
            pdp1P->tbs = 0;
            pdp1P->io |= pdp1P->tb;
        }

        break;

    case 005:   // ppa
    case 006:   // ppb
        if(!pulse)
        {
            pdp1P->pb = 0;
            pdp1P->punon = 1;
            pdp1P->p_time = pdp1P->simtime + PDLY;
        }
        else
        {
            pdp1P->pcp = nac;

            if(dev == 00005)
            {
                pdp1P->pb |= IO & 0377;
            }
            else
            {
                pdp1P->pb |= 0200 | (IO >> 12) & 077;
            }
        }

        break;

    case 007:   // dpy
        if(!pulse)
        {
            pdp1P->dbx = 0;
            pdp1P->dby = 0;
            pdp1P->dint = 0;

            if(lightpenEnabled)
            {
                pdp1P->cksflags &= ~0400000;  // wje, set by the last dpy completion if lp hit
            }
        }
        else if((ch & 030) == 030)       // wje, set lightpen aperture
        {
            pdp1P->dpy_defl_time = NEVER;     // just set aperture
            pdp1P->dpy_time = NEVER;
            pdp1P->dcp = 0;                   // be sure its not set
            // The aperture is the diameter in pixels, allow 6 to 63
            // Each pixel corresponds to the original 0.009"
            penAperture = IO & 077;

            if(penAperture < 6)
            {
                penAperture = 6;
            }

            penRadius2 = (penAperture / 2) * (penAperture / 2); // radius squared
            logger(LOG_APERTURE, "Aperture %d, radius squared %d\n", penAperture, penRadius2);
        }
        else
        {
            pdp1P->dbx |= AC >> 8;
            pdp1P->dby |= IO >> 8;

            // Emulate the origin shift that was implemented in some systems
            // It conflicts with sdb, sdb takes priority
            if(dpyShiftEnabled && !sdbEnabled)
            {
                if(ch & 010)        // origin at bottom
                {
                    pdp1P->dby ^= 01000;
                }

                if(ch & 020)        // origin at left
                {
                    pdp1P->dbx ^= 01000;
                }
            }

            pdp1P->dpy_defl_time = pdp1P->simtime + US(35);
            pdp1P->dpy_time = pdp1P->dpy_defl_time + US(15);
            pdp1P->dint |= (MB >> 6) & 7;
            pdp1P->dcp = nac;

            if(sdbEnabled && ((ch & 030) == 020))    // sdb is a reposition without drawing a dot
            {
                // This is documented as taking 30 usecs because it doesn't
                // need the addtional time to draw the dot.
                // But, there is no real reason to do so, so just complete immediately.
                // Yes, not historically accurate, but neither is using a mouse for a lightpen.
                // All it does is set the intensity and reposition x,y, does not honor completion.
                pdp1P->dpy_defl_time = NEVER;
                pdp1P->dpy_time = NEVER;
                pdp1P->dcp = 0;
            }
        }

        break;

    case 011:   // spacewar controllers

        // simple but stupid version for now
        if(pulse)
        {
            // LRTF
            IO |= pdp1P->spcwar1 << 14 | pdp1P->spcwar2;
        }

        break;

    case 030:   // rrb
        if(pulse)
        {
            IO |= pdp1P->rb;
            pdp1P->rbs = 0;
        }

        break;

    case 033:   // cks
        if(pulse)
        {
            // TODO: LP (wje - just use a dynamic IOT)
            IO |= pdp1P->rbs << 16;
            IO |= !pdp1P->tyo << 15;
            IO |= pdp1P->tbs << 14;
            IO |= !pdp1P->punon << 13;
            // ..
            IO |= pdp1P->sbm << 11;
            IO |= pdp1P->cksflags;        // wje - needed to generalize use, many devices use it
        }

        break;

    case 050:   // dsc
        if(!pulse)
        {
            if(pdp1P->sbs16 && ch < 16)
            {
                pdp1P->b1 &= ~(1 << ch);
            }
        }

        break;

    case 051:   // asc
        if(!pulse)
        {
            if(pdp1P->sbs16 && ch < 16)
            {
                pdp1P->b1 |= (1 << ch);
            }
        }

        break;

    case 052:   // isb
        if(!pulse)
        {
            if(pdp1P->sbs16 && ch < 16)
            {
                pdp1P->b2 |= (1 << ch);
            }
        }

        break;

    case 053:   // cac
        if(!pulse)
        {
            if(pdp1P->sbs16)
            {
                pdp1P->b1 = 0;
            }
        }

        break;

    case 054:   // lsm
        if(!pulse)
        {
            pdp1P->sbm = 0;
        }

        break;

    case 055:   // esm
        if(!pulse)
        {
            pdp1P->sbm = 1;
        }

        break;

    case 056:   // cbs
        if(!pulse)
        {
            clr_sbs(pdp1P);
        }

        break;

    case 074:   // lem/eem
        if(pulse)
        {
            pdp1P->exd = !!(MB & B6);
        }

        break;

    default
            :
        if(!dynamicIotProcessor(pdp1P, dev, pulse, nac))          // wje - see if there is a dynamic IOT to handle this
        {
            printf("unknown IOT %06o\n", MB);
        }

        break;
    }
}

static void
iot(PDP1P pdp1P, int pulse)
{
    int nac;
    int dev;

    nac = (MB & (B5 | B6)) == B5 || (MB & (B5 | B6)) == B6;
    dev = MB & 077;

    // 0 -> IO ON IOT also available for other devices
    if(!pulse && ((dev & 070) == 030))
    {
        IO = 0;
    }

    iot_pulse(pdp1P, pulse, dev, nac);
}

static void
req(PDP1P pdp1P, int chan)
{
    if(pdp1P->sbs16)
    {
        pdp1P->b2 |= pdp1P->b1 & (1 << chan);
    }
    else
    {
        pdp1P->b2 = 1;
    }
}

// Let the dynamic IOT code trigger a break
void
dynamicReq(PDP1P pdp1P, int chan)
{
    req(pdp1P, chan);             // wje - because req() is private
}

void
flushdpy(DispCon *d)
{
    int sz;
    int n;

    sz = d->ncmds * sizeof(d->cmdbuf[0]);
    n = write(d->fd, d->cmdbuf, sz);
    d->ncmds = 0;

    if(n < sz)
    {
        close(d->fd);
        d->fd = -1;
    }
}

// Although the display semulator, p7sim, sends 'lightpen' updates, there
// was no code here originally to handle them.
int
cvtDpyToSigned(int dpy)             // convert a 10 bit dpy coordinate to a 2's complement signed int
{
    // For coordinates, the 1's complement -0 value, 01777, is legitimate, map negative numbers
    // to the range -1 to -512.
    if(dpy & 01000)                 // high bit set means neg 1's cmpl number
    {
        return(-(~dpy & 0777) - 1);
    }
    else
    {
        return(dpy);
    }
}


// See if there is a light pen hit.
// If so, return 1.
// If no hit, return 0.
// The client sends x,y coordinates whenever the mouse is moved with mouse button 1 down.
// When the button is releasde, a 'pen lifted' notice is sent.
// The physical light pen aperture is simulated by having any match within the dot location
// plus the aperture setting.
//
// The algorithm is:
// If a pen-lifted event is seen, set penDwon to false, no hit cheking will be done.
// If a coordnate event is see, set penDown to true and save the most recent x, y update.
// Whenever a dpy completion occurs, check the status and if the pen is down, see if the
// dpy coordinates match the current position within the aperture boundaries and if so,
// set the appropriate flags.
//
// A command from the client is a 32 bit word:
// FFpccccc where:
// p = 0x1 if the pen is up, 0x0 if down
// c is the packed x, y 10 bit 1's complement coordinates (x << 10) | y

#define CMDBITS 0xFF000000
#define LPCMD   0xFF000000
#define PENBITS 0x00F00000
#define LPUP    0x00100000

// This is the reader thread for the lightpen.
// See if there is data from the client, update lp status
int
lightpenListener(PDP1P pdp1P)
{
    int count;
    int flag = 1;
    uint32_t cmdBuf[PENBUFSIZE];
    uint32_t cmd;
    DispCon *dpyP;

    dpyP = &(pdp1P->dpy[0]);

    // Loop reading all pending commands.
    // Only the last will be significant.
    // Return if the fd is closed.
    for(;;)
    {
        if((count = read(dpyP->fd, cmdBuf, sizeof(cmdBuf))) < sizeof(uint32_t))
        {
            count = 0;                          // some problem, probably fd closed
            penDown = false;
            return(0);
        }

        // Turn on fast ack to minimize delays.
        // This might or might not improve lightpen performance.
        setsockopt(dpyP->fd, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag));

        count /= sizeof(uint32_t);                  // convert to index

        if(count > 0)
        {
            cmd = cmdBuf[count - 1];                // last one

            if((cmd & CMDBITS) == LPCMD)    // light pen, just to be sure
            {
                if((cmd & PENBITS) == LPUP)     // pen up, done
                {
                    penDown = false;
                    logger(LOG_LP, "Pen up\n", lastPenX, lastPenY);
                }
                else
                {
                    penDown = true;
                    // We convert to a signed 2's complement integer
                    // Use a mutex to be sure main thead gets a constent pair, maybe overkill.
                    pthread_mutex_lock(&lightpenLock);
                    lastPenX = cvtDpyToSigned((cmd >> 10) & 0x3FF);
                    lastPenY = cvtDpyToSigned(cmd & 0x3FF);
                    pthread_mutex_unlock(&lightpenLock);
                    logger(LOG_LP, "LP received x %d, y %d\n", lastPenX, lastPenY);
                }
            }
        }
    }
}

// Check for a lightpen hit, return true if so, else false.
bool
checkLightPen(PDP1P pdp1P)
{
    int i, sawOne, dpyx, dpyy;
    int delx, dely;
    int lpX, lpY;
    DispCon *dpyP;

    if(!penDown)
    {
        return(false);
    }

    dpyP = &(pdp1P->dpy[0]);
    dpyx = cvtDpyToSigned(pdp1P->dbx);
    dpyy = cvtDpyToSigned(pdp1P->dby);

    // Use a mutex to be sure main thead gets a constent pair, maybe overkill.
    pthread_mutex_lock(&lightpenLock);
    lpX = lastPenX;
    lpY = lastPenY;
    pthread_mutex_unlock(&lightpenLock);

    // Both coordinate pairs have been converted from 10 bit 1's complement to full signed 2's complement.
    // We have to take edge wrapping into account, do nothing if it wrapped.
    // Just compare bits outside the range, neg will have the bit set, pos won't
    if(!((dpyx ^ lpX) & 0x200) && !((dpyy ^ lpY) & 0x200))
    {
        // Use the distance equation for a circle to simulate an actual circular aperture
        delx = lpX - dpyx;               // Find squared magnitudes of hit offset
        dely = lpY - dpyy;

        if(((delx * delx) + (dely * dely)) < penRadius2)
        {
            logger(LOG_LP, "LP x %d, y %d hit at x %d, y %d aperture %d\n",
                   lpX, lpY, dpyx, dpyy, penAperture);
            return(true);
        }
    }

    return(false);
}

void
dpycmd(PDP1P pdp1P, int i, u32 cmd)
{
    DispCon *d = &pdp1P->dpy[i];
    d->cmdbuf[d->ncmds++] = cmd;

    if(d->ncmds == nelem(d->cmdbuf))
    {
        flushdpy(d);
    }
}

void
agedisplay(PDP1P pdp1P, int i)
{
    int ival;

    DispCon *d = &pdp1P->dpy[i];

    if(d->fd < 0)
    {
        return;
    }

    ival = d->agetime;
    assert(d->last <= pdp1P->simtime);
    u64 dt = (pdp1P->simtime - d->last) / 1000;

    if(dt >= ival)
    {
        dpycmd(pdp1P, i, 511 << 23);
        // TODO? theoretically dt could be huge,
        // but if it is you have other problems
        dpycmd(pdp1P, i, dt);
        d->last = pdp1P->simtime;
        flushdpy(d);

        // increase interval during fade out
        // to reduce number of age-commands
        if(d->agetime < 1000 * 1000)
        {
            d->agetime += d->agetime / 6;
        }
    }
}

void
display(PDP1P pdp1P, int screenNo)
{
    int x, y;
    int dt;
    int cmd;
    int twoscreens;
    int intensity;

    if((screenNo < 0) || (screenNo > 1))
    {
        return;     // only 2 screens
    }

    // need to make sure dt field doesn't overflow cmd
    pdp1P->dpy[screenNo].agetime = 510;
    agedisplay(pdp1P, screenNo);
    // reset age interval for every point shown
    pdp1P->dpy[screenNo].agetime = 50 * 1000;

    if(pdp1P->dpy[screenNo].fd < 0)
    {
        return;
    }

    x = pdp1P->dbx;
    y = pdp1P->dby;
    dt = (pdp1P->simtime - pdp1P->dpy[screenNo].last) / 1000;

    if(x & 01000)
    {
        x++;
    }

    if(y & 01000)
    {
        y++;
    }

    x = (x + 01000) & 01777;
    y = (y + 01000) & 01777;
    cmd = x | (y << 10) | (dt << 23);
    intensity = pdp1P->dint;
    // checking fd's is a bit of a hack of course.
    // this is really a hardware configuration
    twoscreens = (pdp1P->dpy[0].fd >= 0) && (pdp1P->dpy[1].fd >= 0);

    if(twoscreens)
    {
        if((pdp1P->dint & 4) && !screenNo)
        {
            return;         // dpy said second screen, call said first screen
        }

        // unclear what's happening here exactly
        // spacewar 4.4 uses only intensity 0/4
        intensity &= 3;
    }

    pdp1P->dpy[screenNo].last = pdp1P->simtime;

    // The real hardware used intensity 4 for a brightness that was only
    // visible to the lightpen.
    // Simulate that by just not drawing a point.
    if(intensity != 4)
    {
        cmd |= ((intensity + 4) & 7) << 20;
        dpycmd(pdp1P, screenNo, cmd);
    }
}

void
handleio(PDP1P pdp1P)
{
    /* Reader */
    if(pdp1P->rcl && pdp1P->r_time < pdp1P->simtime && pdp1P->r_fd >= 0)
    {
        u8 c;
        pdp1P->r_time = pdp1P->simtime + RDLY;

        if(read(pdp1P->r_fd, &c, 1) <= 0)
        {
            close(pdp1P->r_fd);
            pdp1P->r_fd = -1;
            return;
        }

        // write back in case this is over a socket
        // and we need to synchronize
        write(pdp1P->r_fd, &c, 1);

        if(pdp1P->rc && (!pdp1P->rby || c & 0200))
        {
            // STROBE PETR
            pdp1P->rcl = 0;
            pdp1P->rb |= c & (pdp1P->rby ? 077 : 0377);

            // SHIFT RB
            if(pdp1P->rc != 3)
            {
                pdp1P->rb = (pdp1P->rb << 6) & WORDMASK;
                pdp1P->rcl = 1;
            }

            // CLR IO
            if(pdp1P->rc == 3 && (pdp1P->rcp || pdp1P->rim))
            {
                IO = 0;
            }

            // -----
            // +1 RC
            if(pdp1P->rc == 3)
            {
                // READER RETURN
                if(pdp1P->rcp)
                {
                    pdp1P->ios = 1;
                }
                else
                {
                    pdp1P->rbs = 1;
                }

                if(pdp1P->rcp || pdp1P->rim)
                {
                    IO |= pdp1P->rb;
                    pdp1P->rbs = 0;

                    if(pdp1P->rim)
                    {
                        pdp1P->rim_return = 2;
                    }
                }

                // not sure about this, but seems annoying
                if(!pdp1P->rim)
                {
                    req(pdp1P, RD_CHAN);
                }
            }

            pdp1P->rc = (pdp1P->rc + 1) & 3;
        }
    }

    /* Punch */
    if(pdp1P->punon && pdp1P->p_time < pdp1P->simtime)
    {
        pdp1P->p_time = NEVER;

        if(pdp1P->p_fd >= 0)
        {
            char c = pdp1P->pb;
            write(pdp1P->p_fd, &c, 1);
        }

        if(pdp1P->pcp)
        {
            pdp1P->ios = 1;
        }

        req(pdp1P, PUN_CHAN);
    }
    else if(pdp1P->tape_feed && pdp1P->feed_time < pdp1P->simtime)
    {
        pdp1P->feed_time = pdp1P->simtime + PDLY;

        if(pdp1P->p_fd >= 0)
        {
            char c = 0;
            write(pdp1P->p_fd, &c, 1);
        }
    }

    /* Typewriter */
    if(pdp1P->typ_time < pdp1P->simtime)
    {
        // wrong timing
        pdp1P->typ_time = NEVER;

        if((pdp1P->tb & 076) == 034)
        {
            pdp1P->tbb = pdp1P->tb & 1;

            // hack to synchronize input
            if(pdp1P->typ_fd.fd >= 0)
            {
                char c = (pdp1P->tbb << 6) | 060;
                write(pdp1P->typ_fd.fd, &c, 1);
            }
        }
        else if(pdp1P->typ_fd.fd >= 0)
        {
            char c = (pdp1P->tbb << 6) | pdp1P->tb;
            write(pdp1P->typ_fd.fd, &c, 1);
        }

        // this is really much more complicated
        // and overlaps with the type-in logic
        pdp1P->tyo = 0;

        if(pdp1P->tcp)
        {
            pdp1P->ios = 1;
        }

        req(pdp1P, TTO_CHAN);
    }

    // stall input while we're outputting stuff
    if(pdp1P->typ_time != NEVER)
    {
        pdp1P->tyi_wait = pdp1P->simtime + US(25000);
    }

    if(pdp1P->tyi_wait < pdp1P->simtime && pdp1P->typ_fd.ready)
    {
        char c;

        if(read(pdp1P->typ_fd.fd, &c, 1) <= 0)
        {
            closefd(&pdp1P->typ_fd);
            pdp1P->typ_fd.fd = -1;
            return;
        }

        waitfd(&pdp1P->typ_fd);

        if(pdp1P->pf & 040)
        {
            printf("	char missed <%o>\n", pdp1P->tb);
        }

        pdp1P->tb = 0;
        // STROBE TYPE
        pdp1P->tb |= c & 077;
        //
        pdp1P->tbs = 1;
        // TYPE SYNC
        pdp1P->pf |= 040;
        req(pdp1P, TTI_CHAN);

        // PDP-1 has to keep up, so avoid clobbering TB
        // not sure what a good timeout here is
        pdp1P->tyi_wait = pdp1P->simtime + US(25000);
    }

    /* Display */
    if(pdp1P->dpy_defl_time < pdp1P->simtime)
    {
        pdp1P->dpy_defl_time = NEVER;
        display(pdp1P, 0);
        display(pdp1P, 1);
    }

    if(pdp1P->dpy_time < pdp1P->simtime)
    {
        pdp1P->dpy_time = NEVER;

        if(pdp1P->dcp)
        {
            // If there was a light pen hit, cks bit 0 is set, and pf3 is set.
            if(lightpenEnabled && checkLightPen(pdp1P))
            {
                pdp1P->cksflags |= 0400000;               // cleared by next dpy
                pdp1P->pf |= decflg(3);
            }

            pdp1P->ios = 1;
        }
    }
}

int
getwrd(int fd)
{
    u8 c;
    int w, n;

    w = 0;
    n = 3;

    while(n--)
    {
        do
        {
            if(read(fd, &c, 1) <= 0)
            {
                return -1;
            }
        }
        while((c & 0200) == 0);

        w = w << 6 | (c & 077);
    }

    return(w);
}

void
readrim(PDP1P pdp1P, int fd)
{
    int inst, wd;

    if(fd < 0)
    {
        fprintf(stderr, "no tape\n");
        return;
    }

    // clear memory just to be safe
    for(wd = 0; wd < MAXMEM; wd++)
    {
        pdp1P->core[wd] = 0;
    }

    for(;;)
    {
        inst = getwrd(fd);

        if((inst & 0760000) == 0320000)
        {
            wd = getwrd(fd);
            pdp1P->core[inst & 07777] = wd;
        }
        else if((inst & 0760000) == 0600000)
        {
            printf("start: %04o\n", inst & 07777);
            return;
        }
        else
        {
            printf("rim botch: %06o\n", inst);
            return;
        }
    }
}

void
cli(PDP1P pdp1P)
{
    int n;
    static int timer = 0;

    if(timer++ != 10000)
    {
        return;
    }

    timer = 0;

    if(!hasinput(0))
    {
        return;
    }

    char line[1024], *p;

    n = read(0, line, sizeof(line));

    if(n > 0 && n < sizeof(line))
    {
        line[n] = '\0';

        char *resp = handlecmd(pdp1P, line);
        printf("%s\n", resp);
    }
}

char*
handlecmd(PDP1P pdp1P, char *line)
{
    int n;
    int fd;
    char *p;

    static const char *host = "localhost";
    static int port = 3400;
    static char *rimfile = nil;
    static char resp[1024];

    if((p = strchr(line, '\r')), p)
    {
        *p = '\0';
    }

    if(p = strchr(line, '\n'), p)
    {
        *p = '\0';
    }

    char **args = split(line, &n);

    strcpy(resp, "ok");

    if(n > 0)
    {
        // reader
        if(strcmp(args[0], "r") == 0)
        {
            close(pdp1P->r_fd);
            pdp1P->r_fd = -1;

            if(args[1])
            {
                pdp1P->r_fd = open(args[1], O_RDONLY);

                if(pdp1P->r_fd < 0)
                {
                    sprintf(resp, "couldn't open %s", args[1]);
                }
            }
        }
        // punch
        else if(strcmp(args[0], "p") == 0)
            {
                close(pdp1P->p_fd);
                pdp1P->p_fd = -1;

                if(args[1])
                {
                    pdp1P->p_fd = open(args[1], O_CREAT | O_WRONLY | O_TRUNC, 0644);

                    if(pdp1P->p_fd < 0)
                    {
                        sprintf(resp, "couldn't open %s", args[1]);
                    }
                }
            }
        // load
            else if(strcmp(args[0], "l") == 0)
                {
                    if(args[1])
                    {
                        free(rimfile);
                        rimfile = strdup(args[1]);
                    }

                    if(rimfile)
                    {
                        fd = open(rimfile, O_RDONLY);

                        if(fd < 0)
                        {
                            sprintf(resp, "couldn't open %s", rimfile);
                        }
                        else
                        {
                            readrim(pdp1P, fd);
                            close(fd);
                        }
                    }
                    else
                    {
                        sprintf(resp, "no filename");
                    }
                }
        // display
                else if(strcmp(args[0], "d") == 0)
                    {
                        if(args[1])
                        {
                            host = args[1];
                        }

                        if(args[2])
                        {
                            port = atoi(args[2]);
                        }

                        if(pdp1P->dpy[0].fd >= 0)
                        {
                            close(pdp1P->dpy[0].fd);
                        }

                        pdp1P->dpy[0].last = pdp1P->simtime;
                        pdp1P->dpy[0].fd = dial(host, port);

                        if(pdp1P->dpy[0].fd < 0)
                        {
                            strcpy(resp, "can't open display");
                        }
                        else
                        {
                            nodelay(pdp1P->dpy[0].fd);
                        }
                    }
                    else if(strcmp(args[0], "?") == 0 || strcmp(args[0], "help") == 0)
                        {
                            p = resp;
                            p += sprintf(p, "r                     unmount tape from reader\n");
                            p += sprintf(p, "r filename            mount tape in reader\n");
                            p += sprintf(p, "p                     unmount tape from punch\n");
                            p += sprintf(p, "p filename            mount tape in punch\n");
                            p += sprintf(p, "l filename            load memory from RIM-file\n");
                            p += sprintf(p, "d [host] [port]       connect to display program\n");
                            p += sprintf(p, "muldiv [on/off]       set/toggle type 10 mul-div option");
                            p += sprintf(p, "audio [on/off]        set/toggle audio output");
                        }
                        else if(strcmp(args[0], "muldiv") == 0)
                            {
                                if(args[1])
                                {
                                    if(strcmp(args[1], "on") == 0 ||
                                            strcmp(args[1], "1") == 0)
                                    {
                                        pdp1P->muldiv_sw = 1;
                                    }
                                    else
                                        if(strcmp(args[1], "off") == 0 ||
                                                strcmp(args[1], "0") == 0)
                                        {
                                            pdp1P->muldiv_sw = 0;
                                        }

                                    resp[0] = '\0';
                                }
                                else
                                {
                                    pdp1P->muldiv_sw = !pdp1P->muldiv_sw;
                                }

                                sprintf(resp, "mul-div now %s", pdp1P->muldiv_sw ? "on" : "off");
                            }
                            else if(strcmp(args[0], "audio") == 0)
                            {
                                resp[0] = '\0';

                                if(args[1])
                                {
                                    if(strcmp(args[1], "on") == 0 || strcmp(args[1], "1") == 0)
                                    {
                                        audioEnabled = 1;
                                    }
                                    else if(strcmp(args[1], "off") == 0 || strcmp(args[1], "0") == 0)
                                    {
                                        audioEnabled = 0;
                                    }
                                    else if(strcmp(args[1], "query") == 0)
                                    {
                                        sprintf(resp,
                                            "Audio %s, current alpha %f, current gain %f, current tuning %f\n",
                                            audioEnabled ? "on" : "off", getFilterAlpha(),
                                            getMixerGain(), getAudioTuning());
                                    }
                                    else if(strcmp(args[1], "alpha") == 0)
                                    {
                                        setFilterAlpha(atof(args[2]));
                                    }
                                    else if(strcmp(args[1], "gain") == 0)
                                    {
                                        setMixerGain(atof(args[2]));
                                    }
                                    else if(strcmp(args[1], "tuning") == 0)
                                    {
                                        setAudioTuning(atof(args[2]));
                                    }
                                }
                                else
                                {
                                    audioEnabled = !audioEnabled;
                                }

                                // wje, the new audio support adds digital filtering
                                if(audioEnabled)
                                {
                                    if(isAudioInitialized())
                                    {
                                        continueaudio();
                                    }
                                    else
                                    {
                                        initaudio();
                                        startaudio();
                                    }
                                }
                                else
                                {
                                    stopaudio();
                                }

                                if(!resp[0])
                                {
                                    sprintf(resp, "audio now %s", audioEnabled ? "on" : "off");
                                }
                            }
    }

    free(args[0]);
    free(args);

    return resp;
}
