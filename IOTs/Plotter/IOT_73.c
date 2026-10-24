/*
 * IOT_73.c -- a CalComp incremental drum plotter on device 73, drawing to a PNG file.
 *
 * DEC sold a "Graph Plotter and Control" for the PDP-1 with the CalComp 560, 563 and 565 (PDP-1
 * price list, June 1964), but no reliable document giving its IOTs has been found.
 * The command set here is DEC's Type 350 Incremental Plotter Control for the PDP-7
 * (PDP-7 User Handbook F-75, page 106) as one IOT.
 * Bits 7-8 of the IOT select the Type 350's pulse group and its device codes 24, 25 and 26.
 * Bits 9-11 are that group's three pulses.
 * Pulses in one group combine,one IOT can step diagonally.
 * The plotter's done flag is a cks bit, 1 when it is ready for a command.
 * Docs/UsingThePlotter.md has the IOT bits table.
 *
 * Timing is the plotter's authentic timing: a step takes 5 ms on the 560 and 563 (12,000 steps a minute) and
 * 3.33 ms on the 565, a diagonal the same (IBM 1627 manual, A26-5710),
 * a pen change 100 ms (CalComp 565 manual, "5 up and 5 down per second").
 * The plotter cannot detect a pulse that comes too soon, it loses it (CalComp 565 manual: its one-shot does not
 * accept another step signal), so a command while the plotter is busy is ignored.
 *
 * The paper is a bitmap of one pixel per step, 0.01 inch.
 * The carriage axis is the paper's width and stops at its edges.
 * The drum axis is a roll, unbounded both ways, kept in bands allocated as the pen reaches them.
 * The sheet is written as a PNG after the plotter has been idle for a configurable time, 5 seconds by default,
 * at every halt, a hlt or the STOP switch, and at power clear.
 * A new sheet starts at the first plotter IOT after the power is on.
 * The PNG is encoded here and written to a .tmp file by a write-behind thread
 * then renamed over the old one.t
 * The emulator thread never waits on the disk and a viewer never sees half a file.
 *
 * Execution model: everything runs on the emulator thread except the end of each file write,
 * writeDone(), which runs on the write-behind thread and touches only what it was handed.
 *
 * 22-Oct-2026 Claude initial version.
 * 22-Oct-2026 wje cleanup
 * 24-Oct-2026 wje now uses zlib to create a compressed png, much smaller
 */

#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "iotHandler.h"
#include "configuration.h"
#include "writeBehind.h"
#include "pngWrite.h"

//#define DOLOGGING
#include "iotLogger.h"
#ifndef LOG_START
#define LOG_START 0             // start, stop and power clear
#endif
#ifndef LOG_COMMAND
#define LOG_COMMAND 0           // each command taken
#endif
#ifndef LOG_SHEET
#define LOG_SHEET 0             // a sheet started or written
#endif
#ifndef LOG_BUSY
#define LOG_BUSY 1              // a command while busy, ignored
#endif
#ifndef LOG_CONFIG
#define LOG_CONFIG 1            // a plotter setting out of range, ignored
#endif
#ifndef LOG_WRITE_ERROR
#define LOG_WRITE_ERROR 1       // a sheet that could not be encoded or written
#endif

// The command field of the IOT: bits 7-8 the pulse group, bits 9-11 its pulses.
#define GROUP(mb) (((mb) >> 9) & 03)
#define PULSES(mb) (((mb) >> 6) & 07)
#define PULSE_1 01
#define PULSE_2 02
#define PULSE_4 04

#define PEN_NONE 0
#define PEN_DOWN 1
#define PEN_UP 2

// 100 steps an inch, as pixels a meter for the PNG's pHYs chunk.
#define PIXELS_PER_METER 3937

// Rows of drum travel above and below the drawing in the PNG.
#define MARGIN_ROWS 10

#define BAND_ROWS 256

#define MAX_PEN_WIDTH 50
#define MAX_BRUSH (MAX_PEN_WIDTH * MAX_PEN_WIDTH)

#define DEFAULT_MODEL 560
#define DEFAULT_PEN_NS MSTONS(100)
#define DEFAULT_IDLE_NS MSTONS(5000)
#define DEFAULT_CKS_BIT 7
#define DEFAULT_FILE "plotter.png"

// Queued file bytes before a write waits for room: a few sheets of ten feet of the 563's paper.
#define WBMAXBYTES (16 * 1024 * 1024)

// What one IOT asks the plotter to do: a carriage step (+1 right, -1 left), a drum step (+1 up,
// -1 down) and a pen action.
typedef struct
{
    int carriage;
    int drum;
    int pen;
} Command;

// A model's step time and paper width in steps.
typedef struct
{
    int model;
    uint64_t stepNs;
    int width;
} Model;

// What writeDone() needs, handed to the write-behind thread with each write.
typedef struct
{
    char *tmpNameP;
    char *nameP;
} WriteJob;

static const Model models[] =
{
    { 560, 5000000ULL, 1100 },      // 11 inches of plotting width, 12,000 steps a minute
    { 563, 5000000ULL, 2950 },      // 29.5 inches
    { 565, 3333333ULL, 1100 },      // 18,000 steps a minute
};

// Settings, from pidp1.config.
static const Model *modelP = &models[0];
static uint64_t stepNs;
static uint64_t penDownNs;
static uint64_t penUpNs;
static uint64_t idleNs;
static int penWidth;
static int sbsChan;                 // -1 for no break
static int cksMask;
static int staleCksMask;            // a cks bit given up by a reconfiguration, cleared at the next poll
static int origin;                  // the carriage's start, steps from the left edge
static char fileName[256];

// The plotter.
static bool busy;                   // an operation is timing; the cks flag is 0
static bool completionPending;      // the busy operation's IOT asked for a completion
static bool idleArmed;              // the deadline is the idle rewrite, not an operation's end
static bool penDown;
static int penX;                    // carriage position, 0 to sheetWidth - 1
static int64_t penY;                // drum position; up is +

// The sheet.
static bool sheetOpen;
static int sheetNumber;
static int sheetWidth;
static size_t rowBytes;
static uint8_t **bandsP;            // bandCount bands from band number bandLow; NULL where never drawn
static int64_t bandLow;
static size_t bandCount;
static bool anyInk;
static int64_t inkTop;              // the lowest and highest drum rows inked
static int64_t inkBottom;
static bool dirty;                  // inked since the last write
static bool everWritten;            // this sheet has been written at least once

// The pen's footprint: offsets from the pen's position.
static int brushCount;
static int brushDx[MAX_BRUSH];
static int brushDy[MAX_BRUSH];

static bool started;
static bool wasRunning;             // the machine ran on the last I/O poll
static WbQueueP wbP;                // NULL if it could not be made; writes are then done in place

static Command decode(int mb);
static bool isEmpty(Command cmd);
static void startCommand(PDP1 *pdp1P, Command cmd, int completion);
static void openSheet(void);
static void closeSheet(void);
static void mark(int x, int64_t y);
static uint8_t *bandFor(int64_t y);
static int64_t bandOf(int64_t y);
static void sheetRow(void *ctxP, uint32_t row, uint8_t *rowP);
static void writeSheet(void);
static void writeDone(void *argP, int fd, bool ok);
static void makeName(char *bufP, size_t len);
static void armIdle(void);
static void makeBrush(void);
static void configure(void);

// Called for both pulses of each plotter IOT; the command is carried out at the second, as the
// interface documentation asks. Returns 1: every value of the command bits is a plotter IOT.
int
iotHandler(PDP1 *pdp1P, int device, int pulse, int completion)
{
Command cmd;

    (void)device;
    if( !pulse )
    {
        return(1);
    }

    if( !sheetOpen )
    {
        openSheet();
    }

    cmd = decode(MB(pdp1P));

    // The plotter would lose the pulses: nothing moves and the operation under way goes on. A
    // completion is given at once, so a program that does not wait loses steps but never hangs.
    if( busy )
    {
        iotCondLog(LOG_BUSY, "IOT 73: plotter busy, command %06o ignored\n", MB(pdp1P));
        IOCOMPLETE_IFNEEDED(pdp1P, completion);
        return(1);
    }

    if( isEmpty(cmd) )
    {
        IOCOMPLETE_IFNEEDED(pdp1P, completion);
        return(1);
    }

    startCommand(pdp1P, cmd, completion);
    return(1);
}

// The deadline: either the operation under way has ended, or the plotter has been idle long
// enough after drawing that the sheet is written.
void
iotDeadline(PDP1 *pdp1P)
{
    if( busy )
    {
        busy = false;
        CKS(pdp1P) |= cksMask;
        if( completionPending )
        {
            completionPending = false;
            IOCOMPLETE(pdp1P);
        }

        if( sbsChan >= 0 )
        {
            initiateBreak(sbsChan);
        }

        armIdle();
    }
    else if( idleArmed )
    {
        idleArmed = false;
        if( dirty )
        {
            writeSheet();
        }
    }
}

// Every main-loop pass while the power is on: the cks flag follows the plotter. Power clear
// cannot set it (it has no PDP1), and a reconfiguration may have moved it to another bit.
// A halt writes the sheet: iotStop() comes only with the STOP switch, not with a hlt.
void
iotIOPoll(PDP1 *pdp1P)
{
    if( pdp1P->run )
    {
        wasRunning = true;
    }
    else if( wasRunning )
    {
        wasRunning = false;
        if( sheetOpen && (dirty || !everWritten) )
        {
            writeSheet();
        }
    }

    if( staleCksMask )
    {
        CKS(pdp1P) &= ~staleCksMask;
        staleCksMask = 0;
    }

    if( busy )
    {
        CKS(pdp1P) &= ~cksMask;
    }
    else
    {
        CKS(pdp1P) |= cksMask;
    }
}

// Each run starts here; the settings are read and the write-behind queue made only the first time.
void
iotStart(void)
{
    if( !started )
    {
        configure();
        wbP = wbCreate(WBMAXBYTES);
        started = true;
    }

    iotCondLog(LOG_START, "IOT 73 started\n");
}

// A STOP leaves the plotter as it is, an operation under way included; the sheet is written so
// far, so a halted program's drawing can be looked at. A hlt is caught in iotIOPoll().
void
iotStop(void)
{
    if( sheetOpen && (dirty || !everWritten) )
    {
        writeSheet();
    }

    iotCondLog(LOG_START, "IOT 73 stopped\n");
}

// Power clear: the operation under way is dropped (its deadline is already disarmed) and the pen
// lifts. The sheet is written and ended; the next plotter IOT starts a new one.
void
iotPowerClear(void)
{
    busy = false;
    completionPending = false;
    idleArmed = false;
    penDown = false;
    if( sheetOpen )
    {
        if( dirty || !everWritten )
        {
            writeSheet();
        }

        closeSheet();
    }

    iotCondLog(LOG_START, "IOT 73 power clear\n");
}

// SIGHUP: the settings are read again.
void
iotUpdate(void)
{
    configure();
}

// Returns the command an IOT's MB asks for. Drum up and drum down together cancel; the carriage
// still steps.
static Command
decode(int mb)
{
Command cmd;
int pulses;

    cmd.carriage = 0;
    cmd.drum = 0;
    cmd.pen = PEN_NONE;
    pulses = PULSES(mb);

    switch( GROUP(mb) )
    {
    case 1:
        // The Type 350's skip (01) and clear flag (02) have no place here: the flag is in cks.
        if( pulses & PULSE_4 )
        {
            cmd.pen = PEN_UP;
        }
        break;

    case 2:
        if( pulses & PULSE_1 )
        {
            cmd.carriage = 1;
        }
        if( pulses & PULSE_2 )
        {
            cmd.drum += 1;
        }
        if( pulses & PULSE_4 )
        {
            cmd.drum -= 1;
        }
        break;

    case 3:
        if( pulses & PULSE_1 )
        {
            cmd.carriage = -1;
        }
        if( pulses & PULSE_2 )
        {
            cmd.drum = 1;
        }
        if( pulses & PULSE_4 )
        {
            cmd.pen = PEN_DOWN;
        }
        break;

    default:
        break;
    }

    return(cmd);
}

// Returns true if cmd does nothing at all.
static bool
isEmpty(Command cmd)
{
    return( (cmd.carriage == 0) && (cmd.drum == 0) && (cmd.pen == PEN_NONE) );
}

// Carries out cmd and starts timing it. The pen moves before the step and the times add (a pen
// command takes its full time even when the pen is already there: the control cannot know). The
// drawing is done now; only the flag, the completion and the break wait for the deadline.
static void
startCommand(PDP1 *pdp1P, Command cmd, int completion)
{
uint64_t ns;
int newX;

    ns = 0;
    if( cmd.pen != PEN_NONE )
    {
        penDown = (cmd.pen == PEN_DOWN);
        ns += (penDown ? penDownNs : penUpNs);
        if( penDown )
        {
            mark(penX, penY);
        }
    }

    // The carriage's limit switch stops it at the paper's edge; the step still takes its time.
    if( (cmd.carriage != 0) || (cmd.drum != 0) )
    {
        newX = penX + cmd.carriage;
        if( (newX >= 0) && (newX < sheetWidth) )
        {
            penX = newX;
        }

        penY += cmd.drum;
        ns += stepNs;
        if( penDown )
        {
            mark(penX, penY);
        }
    }

    iotCondLog(LOG_COMMAND, "IOT 73: %06o carriage %d drum %d pen %d, at %d,%lld, %llu ns\n",
        MB(pdp1P), cmd.carriage, cmd.drum, cmd.pen, penX, (long long)penY, (unsigned long long)ns);

    busy = true;
    idleArmed = false;
    completionPending = (completion != 0);
    CKS(pdp1P) &= ~cksMask;
    iotPollAt(IOT_TIME_DEVICE, (iotTime(IOT_TIME_DEVICE) + ns));
}

// Starts a new sheet of the configured model's width, the pen up at the operator's start
// position.
static void
openSheet(void)
{
    sheetWidth = modelP->width;
    rowBytes = ((size_t)sheetWidth + 7) / 8;
    bandsP = NULL;
    bandLow = 0;
    bandCount = 0;
    anyInk = false;
    dirty = false;
    everWritten = false;
    penX = (origin < sheetWidth) ? origin : (sheetWidth - 1);
    penY = 0;
    penDown = false;
    ++sheetNumber;
    sheetOpen = true;
    iotCondLog(LOG_SHEET, "IOT 73: sheet %d started, %d steps wide\n", sheetNumber, sheetWidth);
}

// Ends the sheet and frees its bands.
static void
closeSheet(void)
{
size_t i;

    for( i = 0; i < bandCount; ++i )
    {
        free(bandsP[i]);
    }

    free(bandsP);
    bandsP = NULL;
    bandCount = 0;
    sheetOpen = false;
}

// Inks the pen's footprint at (x, y). The paper ends at the carriage's travel; ink off it is lost.
static void
mark(int x, int64_t y)
{
int i;
int px;
int64_t py;
uint8_t *bandP;

    for( i = 0; i < brushCount; ++i )
    {
        px = x + brushDx[i];
        py = y + brushDy[i];
        if( (px < 0) || (px >= sheetWidth) )
        {
            continue;
        }

        if( !(bandP = bandFor(py)) )
        {
            iotCondLog(LOG_WRITE_ERROR, "IOT 73: out of memory for the sheet at drum row %lld\n", (long long)py);
            continue;
        }

        bandP[((size_t)(py - (bandOf(py) * BAND_ROWS)) * rowBytes) + ((size_t)px >> 3)] |= (uint8_t)(0x80 >> (px & 7));
        if( !anyInk || (py < inkTop) )
        {
            inkTop = py;
        }
        if( !anyInk || (py > inkBottom) )
        {
            inkBottom = py;
        }

        anyInk = true;
        dirty = true;
    }
}

// Returns the band number holding drum row y: the floor of y / BAND_ROWS, for negative rows too.
static int64_t
bandOf(int64_t y)
{
    return( (y >= 0) ? (y / BAND_ROWS) : (-((-y + BAND_ROWS - 1) / BAND_ROWS)) );
}

// Returns the band holding drum row y, making it if need be, or NULL if memory runs out. The
// band table grows at whichever end the pen went past.
static uint8_t *
bandFor(int64_t y)
{
int64_t band;
int64_t newLow;
size_t newCount;
size_t shift;
uint8_t **newP;

    band = bandOf(y);
    if( (bandCount == 0) || (band < bandLow) || (band >= (bandLow + (int64_t)bandCount)) )
    {
        if( bandCount == 0 )
        {
            newLow = band;
            newCount = 1;
        }
        else
        {
            newLow = (band < bandLow) ? band : bandLow;
            newCount = (size_t)(((band >= (bandLow + (int64_t)bandCount)) ? (band + 1) : (bandLow + (int64_t)bandCount)) - newLow);
        }

        if( !(newP = calloc(newCount, sizeof(*newP))) )
        {
            return(NULL);
        }

        shift = (size_t)(bandLow - newLow);
        if( bandCount )
        {
            memcpy(newP + shift, bandsP, bandCount * sizeof(*newP));
        }

        free(bandsP);
        bandsP = newP;
        bandLow = newLow;
        bandCount = newCount;
    }

    if( !bandsP[band - bandLow] )
    {
        bandsP[band - bandLow] = calloc(BAND_ROWS, rowBytes);
    }

    return(bandsP[band - bandLow]);
}

// pngEncode()'s row callback: image row 0 is MARGIN_ROWS above the topmost ink, and drum up
// runs down the picture. Ink is black (0); the padding bits past the paper's edge are white.
static void
sheetRow(void *ctxP, uint32_t row, uint8_t *rowP)
{
int64_t y;
int64_t band;
uint8_t *srcP;
size_t i;

    y = *(int64_t *)ctxP + row;
    band = bandOf(y);
    if( (bandCount == 0) || (band < bandLow) || (band >= (bandLow + (int64_t)bandCount))
        || !(srcP = bandsP[band - bandLow]) )
    {
        memset(rowP, 0xff, rowBytes);
        return;
    }

    srcP += (size_t)(y - (band * BAND_ROWS)) * rowBytes;
    for( i = 0; i < rowBytes; ++i )
    {
        rowP[i] = (uint8_t)~srcP[i];
    }

    if( sheetWidth & 7 )
    {
        rowP[rowBytes - 1] |= (uint8_t)(0xff >> (sheetWidth & 7));
    }
}

// Encodes the sheet and writes it to the file through a .tmp and a rename. A sheet with no ink
// is one blank row, so a run that used the plotter always leaves a file.
static void
writeSheet(void)
{
int64_t top;
uint64_t height;
uint8_t *pngP;
size_t size;
char name[sizeof(fileName) + 16];
char tmpName[sizeof(name) + 4];
WriteJob *jobP;
int fd;
bool ok;

    if( anyInk )
    {
        top = inkTop - MARGIN_ROWS;
        height = (uint64_t)(inkBottom - inkTop) + 1 + (2 * MARGIN_ROWS);
    }
    else
    {
        top = 0;
        height = 1;
    }

    dirty = false;
    everWritten = true;

    pngP = NULL;
    if( height <= UINT32_MAX )
    {
        pngP = pngEncode((uint32_t)sheetWidth, (uint32_t)height, PIXELS_PER_METER, sheetRow, &top, &size);
    }

    if( !pngP )
    {
        iotCondLog(LOG_WRITE_ERROR, "IOT 73: sheet %d, %llu rows, too large to write\n", sheetNumber, (unsigned long long)height);
        return;
    }

    makeName(name, sizeof(name));
    snprintf(tmpName, sizeof(tmpName), "%s.tmp", name);

    // The last write must be done with the .tmp before it is truncated again.
    if( wbP )
    {
        wbDrain(wbP);
    }

    if( (fd = open(tmpName, (O_WRONLY | O_CREAT | O_TRUNC), 0666)) < 0 )
    {
        iotCondLog(LOG_WRITE_ERROR, "IOT 73: cannot open %s\n", tmpName);
        free(pngP);
        return;
    }

    jobP = malloc(sizeof(*jobP));
    if( jobP )
    {
        jobP->tmpNameP = strdup(tmpName);
        jobP->nameP = strdup(name);
    }

    if( !jobP || !jobP->tmpNameP || !jobP->nameP )
    {
        iotCondLog(LOG_WRITE_ERROR, "IOT 73: out of memory writing %s\n", name);
        if( jobP )
        {
            free(jobP->tmpNameP);
            free(jobP->nameP);
            free(jobP);
        }
        close(fd);
        free(pngP);
        return;
    }

    iotCondLog(LOG_SHEET, "IOT 73: sheet %d written to %s, %zu bytes\n", sheetNumber, name, size);
    if( wbP )
    {
        wbWrite(wbP, fd, pngP, size, 0, writeDone, jobP);
        wbClose(wbP, fd);
    }
    else
    {
        ok = (write(fd, pngP, size) == (ssize_t)size);
        writeDone(jobP, fd, ok);
        close(fd);
    }

    free(pngP);
}

// The end of a write, on the write-behind thread: a complete .tmp replaces the file at once, so
// a viewer sees either the old picture or the new one.
static void
writeDone(void *argP, int fd, bool ok)
{
WriteJob *jobP;

    (void)fd;
    jobP = argP;
    if( !ok )
    {
        iotCondLog(LOG_WRITE_ERROR, "IOT 73: writing %s failed\n", jobP->tmpNameP);
    }
    else if( rename(jobP->tmpNameP, jobP->nameP) != 0 )
    {
        iotCondLog(LOG_WRITE_ERROR, "IOT 73: cannot rename %s to %s\n", jobP->tmpNameP, jobP->nameP);
    }

    free(jobP->tmpNameP);
    free(jobP->nameP);
    free(jobP);
}

// Puts the sheet's file name in bufP: plotterfile with its first %d, if any, as the sheet number.
static void
makeName(char *bufP, size_t len)
{
char *atP;

    if( (atP = strstr(fileName, "%d")) )
    {
        snprintf(bufP, len, "%.*s%d%s", (int)(atP - fileName), fileName, sheetNumber, (atP + 2));
    }
    else
    {
        snprintf(bufP, len, "%s", fileName);
    }
}

// After an operation: if it inked the sheet, the sheet is written once the plotter has been idle
// idleNs, unless another command comes first.
static void
armIdle(void)
{
    if( dirty && (idleNs > 0) )
    {
        idleArmed = true;
        iotPollAt(IOT_TIME_DEVICE, (iotTime(IOT_TIME_DEVICE) + idleNs));
    }
}

// The pen's footprint for penWidth steps: the pixels whose centers fall in a circle of that
// diameter centered on the pen, which for 1 is the pen's own pixel.
static void
makeBrush(void)
{
int i;
int j;
int a;
int b;

    brushCount = 0;
    for( i = 0; i < penWidth; ++i )
    {
        for( j = 0; j < penWidth; ++j )
        {
            // Twice each pixel's offset from the circle's center, to stay in integers.
            a = (2 * i) - (penWidth - 1);
            b = (2 * j) - (penWidth - 1);
            if( ((a * a) + (b * b)) <= (penWidth * penWidth) )
            {
                brushDx[brushCount] = i - ((penWidth - 1) / 2);
                brushDy[brushCount] = j - ((penWidth - 1) / 2);
                ++brushCount;
            }
        }
    }
}

// Reads the plotter's settings from pidp1.config, each back to its default when it is absent. A
// new model's paper width applies from the next sheet; everything else at once.
static void
configure(void)
{
ConfigurationSettingP settingP;
ConfigurationP configP;
size_t i;
int newCksMask;

    configP = getConfiguration();

    modelP = &models[0];
    if( (settingP = findConfigurationSetting(configP, "plottermodel")) )
    {
        for( i = 0; i < (sizeof(models) / sizeof(models[0])); ++i )
        {
            if( models[i].model == settingP->ivalue )
            {
                modelP = &models[i];
            }
        }

        if( modelP->model != settingP->ivalue )
        {
            iotCondLog(LOG_CONFIG, "plottermodel must be 560, 563 or 565; using %d\n", modelP->model);
        }
    }

    // Microseconds, a fraction allowed (the 565's step is 3333.3).
    stepNs = modelP->stepNs;
    if( (settingP = findConfigurationSetting(configP, "plotterstep")) )
    {
        if( !isnan(settingP->fvalue) && (settingP->fvalue > 0) )
        {
            stepNs = (uint64_t)((settingP->fvalue * 1000.0) + 0.5);
        }
        else if( (settingP->ivalue > 0) && !settingP->strvalueP )
        {
            stepNs = USTONS(settingP->ivalue);
        }
        else
        {
            iotCondLog(LOG_CONFIG, "plotterstep must be a time in microseconds\n");
        }
    }

    penDownNs = DEFAULT_PEN_NS;
    if( (settingP = findConfigurationSetting(configP, "plotterpendown")) )
    {
        if( (settingP->ivalue > 0) && !settingP->strvalueP )
        {
            penDownNs = MSTONS(settingP->ivalue);
        }
        else
        {
            iotCondLog(LOG_CONFIG, "plotterpendown must be a time in milliseconds\n");
        }
    }

    penUpNs = DEFAULT_PEN_NS;
    if( (settingP = findConfigurationSetting(configP, "plotterpenup")) )
    {
        if( (settingP->ivalue > 0) && !settingP->strvalueP )
        {
            penUpNs = MSTONS(settingP->ivalue);
        }
        else
        {
            iotCondLog(LOG_CONFIG, "plotterpenup must be a time in milliseconds\n");
        }
    }

    // The configuration reader keeps a value without spaces, at most 63 characters.
    snprintf(fileName, sizeof(fileName), "%s", DEFAULT_FILE);
    if( (settingP = findConfigurationSetting(configP, "plotterfile")) )
    {
        if( settingP->strvalueP )
        {
            snprintf(fileName, sizeof(fileName), "%s", settingP->strvalueP);
        }
        else
        {
            iotCondLog(LOG_CONFIG, "plotterfile must be a file name\n");
        }
    }

    idleNs = DEFAULT_IDLE_NS;
    if( (settingP = findConfigurationSetting(configP, "plotteridle")) )
    {
        if( (settingP->ivalue >= 0) && !settingP->strvalueP )
        {
            idleNs = MSTONS((uint64_t)settingP->ivalue * 1000);
        }
        else
        {
            iotCondLog(LOG_CONFIG, "plotteridle must be a number of seconds, 0 for none\n");
        }
    }

    penWidth = 1;
    if( (settingP = findConfigurationSetting(configP, "plotterpen")) )
    {
        if( (settingP->ivalue >= 1) && (settingP->ivalue <= MAX_PEN_WIDTH) && !settingP->strvalueP )
        {
            penWidth = settingP->ivalue;
        }
        else
        {
            iotCondLog(LOG_CONFIG, "plotterpen must be a width of 1 to %d steps\n", MAX_PEN_WIDTH);
        }
    }

    makeBrush();

    // The configuration reader stores "off" as it stores 0, so 0 is no break, not channel 0.
    sbsChan = -1;
    if( (settingP = findConfigurationSetting(configP, "plottersbs")) )
    {
        if( (settingP->ivalue >= 1) && (settingP->ivalue <= 15) && !settingP->strvalueP )
        {
            sbsChan = settingP->ivalue;
        }
        else if( (settingP->ivalue != 0) || settingP->strvalueP || settingP->onOff )
        {
            iotCondLog(LOG_CONFIG, "plottersbs must be a channel 1-15, or off; no break\n");
        }
    }

    newCksMask = 1 << (17 - DEFAULT_CKS_BIT);
    if( (settingP = findConfigurationSetting(configP, "plottercks")) )
    {
        if( (settingP->ivalue >= 0) && (settingP->ivalue <= 17) && !settingP->strvalueP && !settingP->onOff )
        {
            newCksMask = 1 << (17 - settingP->ivalue);
        }
        else
        {
            iotCondLog(LOG_CONFIG, "plottercks must be a bit number 0-17; using %d\n", DEFAULT_CKS_BIT);
        }
    }

    if( cksMask && (newCksMask != cksMask) )
    {
        staleCksMask = cksMask;
    }

    cksMask = newCksMask;

    origin = 0;
    if( (settingP = findConfigurationSetting(configP, "plotterorigin")) )
    {
        if( (settingP->ivalue >= 0) && !settingP->strvalueP && !settingP->onOff )
        {
            origin = settingP->ivalue;
        }
        else
        {
            iotCondLog(LOG_CONFIG, "plotterorigin must be a step count from the left edge\n");
        }
    }

    iotCondLog(LOG_START, "IOT 73: model %d, step %llu ns, pen %llu/%llu ns, file %s, idle %llu ns, pen %d, sbs %d, cks %06o, origin %d\n",
        modelP->model, (unsigned long long)stepNs, (unsigned long long)penDownNs, (unsigned long long)penUpNs,
        fileName, (unsigned long long)idleNs, penWidth, sbsChan, cksMask, origin);
}
