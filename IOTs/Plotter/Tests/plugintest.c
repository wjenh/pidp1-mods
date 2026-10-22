/*
 * plugintest.c -- host tests for the plotter plugin (../IOT_73.c), with no emulator.
 *
 * The plugin's source is included here so its state (pen, sheet, flags) can be read directly,
 * and it is driven through its own entry points as the emulator would drive it.
 * This program stands in for the emulator: the PDP1 struct, simtime on the device base, the one-shot deadline,
 * the break hook and the configuration.
 * It checks:
 *   T2  decoding: all 32 values of command bits 7-11, with each wait/completion combination and
 *       with junk in AC and IO, against the instruction table; control: a table with two
 *       motions swapped must disagree;
 *   T3  timing: step, diagonal, pen, pen plus step and unchanged-pen times for each model, the
 *       settings that override them, completion only when exactly one of bits 5 and 6 is set,
 *       the cks flag (0 from the IOT to the deadline, 1 after, after a power clear, on another
 *       bit when configured, no other cks bit touched), the break; control: a step time one
 *       microsecond off must be caught;
 *   T4  busy: a command during an operation is ignored, completes at once if it asked, leaves
 *       the deadline alone and is logged; control: a check expecting it queued must fail;
 *   T5  paper: the carriage stops at both edges, the drum runs 100,000 steps each way, bands
 *       are made only where ink is; control: a check expecting a step past the edge to move
 *       the pen must fail;
 *   T6  writing: the idle rewrite, the write at STOP, at a hlt (none during an in-out wait)
 *       and at power clear, the .tmp and rename,
 *       %d numbering, a new sheet after power clear, the picture's orientation. The pictures
 *       are written with their expected bitmaps to plugintest.d/manifest.txt for checkpng.py,
 *       with a control that a blank sheet does not match a drawn one.
 *
 * Build and run: make plugintest (in this directory).
 * Built with DOLOGGING, the IOT's reports go to /tmp/iot.dbg, which T4 reads.
 * Prints PASS or FAIL for each group and every failed check; exit status 0 if everything passed.
 *
 */

#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>

#include "../IOT_73.c"

#define DIR "plugintest.d"
#define LOG_FILE "/tmp/iot.dbg"
#define MS 1000000ULL

// The instructions, as the documentation names them.
#define PLPU 0721473
#define PLPR 0722173
#define PLDU 0722273
#define PLDD 0722473
#define PLPL 0723173
#define PLUD 0723273
#define PLPD 0723473
#define PLNOP 0720073
#define BIT_I 0010000
#define BIT_C 0004000

#define MAX_SETTINGS 16
#define FLAG_BIT7 0002000

typedef struct
{
    int carriage;
    int drum;
    int pen;
} Expect;

// An expected picture, in drum coordinates: ink at (x, y) for y from yLow.
typedef struct
{
    int width;
    int64_t yLow;
    int64_t rows;
    uint8_t *inkP;
} Picture;

static PDP1 pdp;
static uint64_t simNow;                     // the device base, ns
static bool deadlineArmed;
static uint64_t deadlineAt;
static int breakCounts[16];
static Configuration config;
static ConfigurationSetting settings[MAX_SETTINGS];
static int settingCount;
static FILE *manifestP;

static const char *testNameP;
static int checkCount;
static int failCount;

// The emulator's side.

// The break hook initiateBreak() calls. Counts the request.
void
dynamicIotProcessBreak(int chan)
{
    if( (chan >= 0) && (chan < 16) )
    {
        ++breakCounts[chan];
    }
}

// Returns the device base's time; the run base is not used by the plotter.
uint64_t
dynamicIotTime(int base)
{
    (void)base;
    return(simNow);
}

// Arms the one-shot deadline, replacing any armed one, as dynamicIots.c does.
void
dynamicIotSetDeadline(IotEntryP entryP, int base, uint64_t deadline)
{
    (void)entryP;
    if( base != IOT_TIME_DEVICE )
    {
        printf("    FAIL [%s] a deadline on base %d\n", testNameP, base);
        ++failCount;
    }

    deadlineArmed = true;
    deadlineAt = deadline;
}

void
dynamicIotCancelDeadline(IotEntryP entryP)
{
    (void)entryP;
    deadlineArmed = false;
}

// Returns the test's configuration.
ConfigurationP
getConfiguration(void)
{
    return(&config);
}

// Returns the setting called nameP, or NULL.
ConfigurationSettingP
findConfigurationSetting(ConfigurationP configP, char *nameP)
{
ConfigurationSettingP sP;

    for( sP = configP->settingsP; sP; sP = sP->nextP )
    {
        if( strcmp(sP->nameP, nameP) == 0 )
        {
            return(sP);
        }
    }

    return(NULL);
}

// Reporting.

// Counts one check; on failure prints the test name and the message.
static void
check(bool ok, const char *fmtP, ...)
{
va_list args;

    ++checkCount;
    if( ok )
    {
        return;
    }

    ++failCount;
    printf("    FAIL [%s] ", testNameP);
    va_start(args, fmtP);
    vprintf(fmtP, args);
    va_end(args);
    printf("\n");
}

// Runs one test group and prints PASS or FAIL for it.
static void
runTest(void (*fnP)(void), const char *nameP)
{
int before;

    testNameP = nameP;
    before = failCount;
    fnP();
    printf("%s  %s\n", ((failCount == before) ? "PASS" : "FAIL"), nameP);
}

// Helpers.

// Sets a setting as configuration.c would read "name = value": a yes/no word sets onOff, digits
// set ivalue, digits and a point fvalue and ivalue, anything else strvalueP. NULL removes it.
static void
setSetting(const char *nameP, const char *valueP)
{
int i;
ConfigurationSettingP sP;

    sP = NULL;
    for( i = 0; i < settingCount; ++i )
    {
        if( strcmp(settings[i].nameP, nameP) == 0 )
        {
            sP = &settings[i];
        }
    }

    if( !sP )
    {
        sP = &settings[settingCount++];
        sP->nameP = strdup(nameP);
    }

    free(sP->strvalueP);
    sP->strvalueP = NULL;
    sP->onOff = false;
    sP->ivalue = 0;
    sP->fvalue = NAN;

    if( !valueP )
    {
        free(sP->nameP);
        *sP = settings[--settingCount];
        memset(&settings[settingCount], 0, sizeof(settings[0]));
    }
    else if( !strcmp(valueP, "on") || !strcmp(valueP, "yes") || !strcmp(valueP, "true") || !strcmp(valueP, "y") )
    {
        sP->onOff = true;
    }
    else if( !strcmp(valueP, "off") || !strcmp(valueP, "no") || !strcmp(valueP, "false") || !strcmp(valueP, "n") )
    {
        // onOff false, ivalue 0, no string: the reader's "off".
    }
    else if( strspn(valueP, "-0123456789") == strlen(valueP) )
    {
        sP->ivalue = atoi(valueP);
    }
    else if( strspn(valueP, "-0123456789.") == strlen(valueP) )
    {
        sP->fvalue = atof(valueP);
        sP->ivalue = atoi(valueP);
    }
    else
    {
        sP->strvalueP = strdup(valueP);
    }

    config.settingsP = NULL;
    for( i = 0; i < settingCount; ++i )
    {
        settings[i].nextP = config.settingsP;
        config.settingsP = &settings[i];
    }
}

// Removes every setting.
static void
clearSettings(void)
{
    while( settingCount )
    {
        setSetting(settings[0].nameP, NULL);
    }
}

// Executes one IOT as dynamicIotProcessor() does: MB holds inst, completion is 1 when exactly one
// of bits 5 and 6 is set, and the handler is called for both pulses. ios is cleared first, so a
// completion is seen. No I/O poll follows: the next instruction may read cks before one runs, so
// the handler itself must leave the flag right.
static void
iot(Word inst)
{
int completion;

    pdp.mb = inst;
    pdp.ios = 0;
    completion = (((inst & 014000) == 010000) || ((inst & 014000) == 004000));
    iotHandler(&pdp, 073, 0, completion);
    iotHandler(&pdp, 073, 1, completion);
}

// Advances the device base by ns, calling the deadline when it comes (it may arm another).
static void
advance(uint64_t ns)
{
uint64_t end;

    end = simNow + ns;
    while( deadlineArmed && (deadlineAt <= end) )
    {
        simNow = deadlineAt;
        deadlineArmed = false;
        iotDeadline(&pdp);
        iotIOPoll(&pdp);
    }

    simNow = end;
    iotIOPoll(&pdp);
}

// Runs until the plotter is idle (its operation's deadline has come), not past the idle rewrite.
static void
finish(void)
{
    if( busy && deadlineArmed )
    {
        advance(deadlineAt - simNow);
    }
}

// Issues inst and waits out its operation.
static void
plot(Word inst)
{
    iot(inst);
    finish();
}

// A power cycle and a fresh start with the current settings: the sheet in progress is written
// (and waited for), the plotter reset, the settings read.
static void
powerCycle(void)
{
    deadlineArmed = false;
    iotPowerClear();
    iotStop();
    if( wbP )
    {
        wbDrain(wbP);
    }

    iotUpdate();
    iotIOPoll(&pdp);
}

// Returns the cks flag bit as the program would read it.
static bool
flag(void)
{
    return( (CKS(&pdp) & cksMask) != 0 );
}

// Pictures.

// Starts an expected picture of the given width covering drum rows yLow..yHigh.
static Picture
newPicture(int width, int64_t yLow, int64_t yHigh)
{
Picture p;

    p.width = width;
    p.yLow = yLow;
    p.rows = (yHigh - yLow) + 1;
    p.inkP = calloc((size_t)(p.width * p.rows), 1);
    return(p);
}

// Inks (x, y) in the expected picture.
static void
ink(Picture *pP, int x, int64_t y)
{
    pP->inkP[((size_t)(y - pP->yLow) * pP->width) + x] = 1;
}

// Writes the expected picture as NAME.pbm, framed as the plugin frames a sheet (MARGIN_ROWS
// above the first ink and below the last), and lists png and pbm in the manifest; want is
// "pass", or "fail" for a control.
static void
expectPicture(const char *pngP, const char *pbmP, Picture *pP, const char *wantP)
{
char path[256];
FILE *fP;
int64_t y;
int x;
int64_t first;
int64_t last;
int64_t row;

    // The plugin frames the ink, so the expected frame is found the same way.
    first = -1;
    last = -1;
    for( y = 0; y < pP->rows; ++y )
    {
        for( x = 0; x < pP->width; ++x )
        {
            if( pP->inkP[((size_t)y * pP->width) + x] )
            {
                if( first < 0 )
                {
                    first = y;
                }
                last = y;
            }
        }
    }

    snprintf(path, sizeof(path), "%s/%s", DIR, pbmP);
    fP = fopen(path, "w");
    if( first < 0 )
    {
        fprintf(fP, "P1\n%d 1\n", pP->width);
        for( x = 0; x < pP->width; ++x )
        {
            fputc('0', fP);
        }
        fputc('\n', fP);
    }
    else
    {
        fprintf(fP, "P1\n%d %lld\n", pP->width, (long long)((last - first) + 1 + (2 * MARGIN_ROWS)));
        for( row = first - MARGIN_ROWS; row <= (last + MARGIN_ROWS); ++row )
        {
            for( x = 0; x < pP->width; ++x )
            {
                fputc(((row >= 0) && (row < pP->rows) && pP->inkP[((size_t)row * pP->width) + x]) ? '1' : '0', fP);
            }
            fputc('\n', fP);
        }
    }

    fclose(fP);
    fprintf(manifestP, "%s %s %d %s\n", pngP, pbmP, PIXELS_PER_METER, wantP);
}

// Returns true if path exists.
static bool
exists(const char *pathP)
{
struct stat st;

    return( stat(pathP, &st) == 0 );
}

// Copies a written sheet aside, so a later write to the same name leaves it for checkpng.py.
static void
keep(const char *fromP, const char *toP)
{
char from[256];
char to[256];
FILE *inP;
FILE *outP;
int c;

    snprintf(from, sizeof(from), "%s/%s", DIR, fromP);
    snprintf(to, sizeof(to), "%s/%s", DIR, toP);
    inP = fopen(from, "rb");
    outP = fopen(to, "wb");
    if( !inP || !outP )
    {
        check(false, "cannot copy %s to %s", from, to);
        if( inP )
        {
            fclose(inP);
        }
        if( outP )
        {
            fclose(outP);
        }
        return;
    }

    while( (c = fgetc(inP)) != EOF )
    {
        fputc(c, outP);
    }

    fclose(inP);
    fclose(outP);
}

// Returns the size of the log file, 0 if there is none.
static long
logSize(void)
{
struct stat st;

    return( (stat(LOG_FILE, &st) == 0) ? (long)st.st_size : 0 );
}

// Returns true if the log, from byte offset on, contains textP.
static bool
logHas(long offset, const char *textP)
{
FILE *fP;
char buf[8192];
size_t n;

    if( !(fP = fopen(LOG_FILE, "r")) )
    {
        return(false);
    }

    fseek(fP, offset, SEEK_SET);
    n = fread(buf, 1, (sizeof(buf) - 1), fP);
    buf[n] = 0;
    fclose(fP);
    return( strstr(buf, textP) != NULL );
}

// The tests.

// The instruction table: group (bits 7-8) by pulses (bits 9-11).
static const Expect table[4][8] =
{
    // Group 0: nothing.
    { {0,0,PEN_NONE}, {0,0,PEN_NONE}, {0,0,PEN_NONE}, {0,0,PEN_NONE},
      {0,0,PEN_NONE}, {0,0,PEN_NONE}, {0,0,PEN_NONE}, {0,0,PEN_NONE} },
    // Group 1: 04 pen up; 01 and 02 reserved.
    { {0,0,PEN_NONE}, {0,0,PEN_NONE}, {0,0,PEN_NONE}, {0,0,PEN_NONE},
      {0,0,PEN_UP},   {0,0,PEN_UP},   {0,0,PEN_UP},   {0,0,PEN_UP} },
    // Group 2: 01 pen right, 02 drum up, 04 drum down; up and down cancel.
    { {0,0,PEN_NONE},  {1,0,PEN_NONE},  {0,1,PEN_NONE},  {1,1,PEN_NONE},
      {0,-1,PEN_NONE}, {1,-1,PEN_NONE}, {0,0,PEN_NONE},  {1,0,PEN_NONE} },
    // Group 3: 01 pen left, 02 drum up, 04 pen down.
    { {0,0,PEN_NONE},  {-1,0,PEN_NONE},  {0,1,PEN_NONE},  {-1,1,PEN_NONE},
      {0,0,PEN_DOWN},  {-1,0,PEN_DOWN},  {0,1,PEN_DOWN},  {-1,1,PEN_DOWN} },
};

// Issues one command from a known state (pen up, mid-sheet) and returns how many of the
// observations disagree with want: the motion, the pen, the busy time, the completion.
static int
decodeCase(Word inst, Expect want, Word junk)
{
int startX;
int64_t startY;
uint64_t ns;
uint64_t wantNs;
int wrong;
bool wantCompletion;
bool gotCompletion;

    powerCycle();
    iot(PLNOP);
    penX = 500;
    penY = 0;
    pdp.io = junk;
    pdp.ac = junk;
    startX = penX;
    startY = penY;

    iot(inst);
    ns = deadlineArmed ? (deadlineAt - simNow) : 0;
    gotCompletion = (pdp.ios != 0);
    finish();
    gotCompletion = gotCompletion || (pdp.ios != 0);

    wantNs = 0;
    if( want.pen != PEN_NONE )
    {
        wantNs += 100 * MS;
    }
    if( (want.carriage != 0) || (want.drum != 0) )
    {
        wantNs += 5 * MS;
    }

    wantCompletion = (((inst & 014000) == 010000) || ((inst & 014000) == 004000));

    wrong = 0;
    wrong += ((penX - startX) != want.carriage);
    wrong += ((penY - startY) != want.drum);
    wrong += (penDown != (want.pen == PEN_DOWN));
    wrong += (ns != wantNs);
    wrong += (gotCompletion != wantCompletion);
    wrong += (busy != false);
    return(wrong);
}

// T2: every value of the command bits, each with the four wait/completion combinations and with
// and without junk in AC and IO.
static void
testDecode(void)
{
int v;
int w;
int j;
int wrong;
int swappedWrong;
Word inst;
Expect swapped[4][8];
static const Word waits[4] = { 0, BIT_I, BIT_C, (BIT_I | BIT_C) };
static const Word junks[2] = { 0, 0777777 };

    clearSettings();
    setSetting("plotterfile", DIR "/t2.png");
    setSetting("plotteridle", "0");
    powerCycle();

    for( v = 0; v < 32; ++v )
    {
        for( w = 0; w < 4; ++w )
        {
            for( j = 0; j < 2; ++j )
            {
                inst = 0720073 | ((Word)v << 6) | waits[w];
                wrong = decodeCase(inst, table[v >> 3][v & 7], junks[j]);
                check(wrong == 0, "%06o (io %06o): %d observations differ from the table", inst, junks[j], wrong);
            }
        }
    }

    // Control: drum up and drum down swapped in the expectation must be caught.
    memcpy(swapped, table, sizeof(swapped));
    swapped[2][2] = table[2][4];
    swapped[2][4] = table[2][2];
    swappedWrong = decodeCase(PLDU, swapped[2][2], 0) + decodeCase(PLDD, swapped[2][4], 0);
    check(swappedWrong > 0, "control: a table with drum up and down swapped was not caught");
}

// Issues inst and returns the busy time it armed, in ns (0 if none).
static uint64_t
busyTime(Word inst)
{
uint64_t ns;

    iot(inst);
    ns = deadlineArmed ? (deadlineAt - simNow) : 0;
    finish();
    return(ns);
}

// Checks the step, diagonal, pen, pen-and-step and unchanged-pen times against stepNs and the
// pen times. Returns the number that differ.
static int
timesDiffer(uint64_t wantStep, uint64_t wantDown, uint64_t wantUp)
{
int wrong;

    wrong = 0;
    plot(PLPU);
    wrong += (busyTime(PLPR) != wantStep);
    wrong += (busyTime(PLDD) != wantStep);
    wrong += (busyTime(PLPR | 0200) != wantStep);         // pen right and drum up: a diagonal
    wrong += (busyTime(PLPL | 0200) != wantStep);         // pen left and drum up
    wrong += (busyTime(PLPD) != wantDown);
    wrong += (busyTime(PLPD) != wantDown);                // already down: the time still passes
    wrong += (busyTime(PLPU) != wantUp);
    wrong += (busyTime(PLPU) != wantUp);
    wrong += (busyTime(PLPL | 0400) != (wantDown + wantStep));   // pen left and pen down
    wrong += (busyTime(PLUD | 0400) != (wantDown + wantStep));   // drum up and pen down
    return(wrong);
}

// T3: times for each model and the overrides, completion, the cks flag and the break.
static void
testTiming(void)
{
static const struct { const char *modelP; uint64_t step; } cases[] =
{
    { "560", 5000000ULL }, { "563", 5000000ULL }, { "565", 3333333ULL },
};
size_t i;
int other;
int before;

    clearSettings();
    setSetting("plotterfile", DIR "/t3.png");
    setSetting("plotteridle", "0");

    for( i = 0; i < (sizeof(cases) / sizeof(cases[0])); ++i )
    {
        setSetting("plottermodel", cases[i].modelP);
        powerCycle();
        iot(PLNOP);
        penX = 500;
        check(timesDiffer(cases[i].step, 100 * MS, 100 * MS) == 0, "model %s: a time differs", cases[i].modelP);
    }

    // Control: a step one microsecond longer than the model's must be caught.
    setSetting("plottermodel", "560");
    powerCycle();
    iot(PLNOP);
    penX = 500;
    check(timesDiffer(5001000ULL, 100 * MS, 100 * MS) > 0, "control: a step time 1 us off was not caught");

    // The overrides, a fractional step time included.
    setSetting("plotterstep", "2500");
    setSetting("plotterpendown", "60");
    setSetting("plotterpenup", "40");
    powerCycle();
    iot(PLNOP);
    penX = 500;
    check(timesDiffer(2500000ULL, 60 * MS, 40 * MS) == 0, "plotterstep 2500, plotterpendown 60, plotterpenup 40: a time differs");
    setSetting("plotterstep", "3333.3");
    powerCycle();
    iot(PLNOP);
    penX = 500;
    check(busyTime(PLPR) == 3333300ULL, "plotterstep 3333.3 gives %llu ns", (unsigned long long)stepNs);
    setSetting("plotterstep", NULL);
    setSetting("plotterpendown", NULL);
    setSetting("plotterpenup", NULL);
    powerCycle();

    // Completion: with i or with c, at the deadline and not before; with neither or both, never.
    iot(PLNOP);
    penX = 500;
    iot(PLPR | BIT_I);
    check(pdp.ios == 0, "i: completion before the step ended");
    advance(5 * MS - 1);
    check(pdp.ios == 0, "i: completion 1 ns before the step ended");
    advance(1);
    check(pdp.ios == 1, "i: no completion when the step ended");
    iot(PLPR | BIT_C);
    finish();
    check(pdp.ios == 1, "c: no completion");
    iot(PLPR | BIT_I | BIT_C);
    finish();
    check(pdp.ios == 0, "i and c together: a completion, which the emulator would not wait for");
    iot(PLPR);
    finish();
    check(pdp.ios == 0, "neither: a completion");
    iot(PLNOP | BIT_I);
    check(pdp.ios == 1, "an empty command with i: no completion at once");
    check(!deadlineArmed && !busy, "an empty command started an operation");

    // The cks flag, with the other cks bits holding a pattern that must survive.
    other = 0525252 & ~FLAG_BIT7;
    CKS(&pdp) = other | FLAG_BIT7;
    iotIOPoll(&pdp);
    check(flag() && (cksMask == FLAG_BIT7), "the flag is not cks bit 7, or not 1 when idle");
    iot(PLPR);
    check(!flag(), "the flag is 1 during a step");
    advance(5 * MS - 1);
    check(!flag(), "the flag is 1 one ns before the step ended");
    advance(1);
    check(flag(), "the flag is 0 after the step");
    check((CKS(&pdp) & ~FLAG_BIT7) == other, "another cks bit changed: %06o", CKS(&pdp));

    // Power clear in the middle of an operation: the flag is 1 after it, the pen is up.
    plot(PLPD);
    iot(PLPR);
    deadlineArmed = false;
    iotPowerClear();
    iotIOPoll(&pdp);
    check(flag() && !busy && !penDown, "after a power clear: flag %d busy %d pen down %d", flag(), busy, penDown);
    iotStop();

    // plottercks moves the flag, and the old bit is cleared.
    setSetting("plottercks", "3");
    iotUpdate();
    iotIOPoll(&pdp);
    check((CKS(&pdp) & FLAG_BIT7) == 0, "plottercks 3: bit 7 still set");
    check((CKS(&pdp) & 040000) != 0, "plottercks 3: bit 3 not set when idle");
    iot(PLNOP);
    iot(PLPR);
    check((CKS(&pdp) & 040000) == 0, "plottercks 3: bit 3 set while busy");
    finish();
    check((CKS(&pdp) & ~040000) == (other & ~040000), "plottercks 3: another cks bit changed: %06o", CKS(&pdp));
    setSetting("plottercks", NULL);
    powerCycle();

    // The break: none by default, or with 0 or off; on the channel when set.
    memset(breakCounts, 0, sizeof(breakCounts));
    iot(PLNOP);
    plot(PLPR);
    setSetting("plottersbs", "off");
    iotUpdate();
    plot(PLPR);
    setSetting("plottersbs", "0");
    iotUpdate();
    plot(PLPR);
    for( before = 0, i = 0; i < 16; ++i )
    {
        before += breakCounts[i];
    }
    check(before == 0, "%d breaks with plottersbs unset, off or 0", before);
    setSetting("plottersbs", "5");
    iotUpdate();
    iot(PLPR);
    check(breakCounts[5] == 0, "a break before the step ended");
    finish();
    check(breakCounts[5] == 1, "%d breaks on channel 5 after one step", breakCounts[5]);
    iot(PLNOP);
    check(breakCounts[5] == 1, "a break for an empty command");
    setSetting("plottersbs", NULL);
    powerCycle();
}

// T4: a command while busy.
static void
testBusy(void)
{
long logAt;
uint64_t armedAt;
int startX;

    clearSettings();
    setSetting("plotterfile", DIR "/t4.png");
    setSetting("plotteridle", "0");
    powerCycle();
    iot(PLNOP);
    penX = 500;
    startX = penX;

    logAt = logSize();
    iot(PLPR);
    armedAt = deadlineAt;
    advance(2 * MS);
    iot(PLPR | BIT_C);
    check(pdp.ios == 1, "the ignored command asked for a completion and did not get one at once");
    check(penX == (startX + 1), "the ignored command moved the pen: %d", penX);
    check(deadlineArmed && (deadlineAt == armedAt), "the ignored command changed the deadline");
    check(busy && !flag(), "the ignored command ended the operation");
    check(logHas(logAt, "command 726173 ignored"), "no log line for the ignored command");
    finish();
    check(simNow == armedAt, "the operation ended at %llu, not %llu", (unsigned long long)simNow, (unsigned long long)armedAt);

    // Control: a check that expects the command to have been queued must fail.
    advance(10 * MS);
    check(penX != (startX + 2), "control: the command was queued after all");

    // A careless program: a burst of pen-rights with no waiting moves the pen once.
    iot(PLPR);
    iot(PLPR);
    iot(PLPR);
    finish();
    check(penX == (startX + 2), "a burst of three moved the pen to %d, not %d", penX, (startX + 2));
}

// T5: the paper's edges and the roll.
static void
testPaper(void)
{
int i;
size_t allocated;
size_t b;
FILE *fP;
uint8_t ihdr[24];
uint32_t h;
uint32_t wantH;

    clearSettings();
    setSetting("plotterfile", DIR "/t5.png");
    setSetting("plotteridle", "0");
    powerCycle();
    iot(PLNOP);

    // The left edge: a step left from 0 stays at 0 and takes the step time.
    check(penX == 0, "the pen starts at %d, not the left edge", penX);
    check(busyTime(PLPL) == (5 * MS), "a step against the left edge does not take the step time");
    check(penX == 0, "a step left from 0 moved the pen to %d", penX);

    // The right edge: 1200 steps right from 0 end at 1099.
    for( i = 0; i < 1200; ++i )
    {
        plot(PLPR);
    }
    check(penX == 1099, "1200 steps right end at %d, not 1099", penX);

    // Control: a check expecting a step past the edge to move the pen must fail.
    plot(PLPR);
    check(penX != 1100, "control: a step past the right edge moved the pen");

    // plotterorigin sets the start; the 563's paper is 2950 wide.
    setSetting("plotterorigin", "100");
    setSetting("plottermodel", "563");
    powerCycle();
    iot(PLNOP);
    check((penX == 100) && (sheetWidth == 2950), "origin 100 on the 563: pen at %d, width %d", penX, sheetWidth);
    for( i = 0; i < 3000; ++i )
    {
        plot(PLPR);
    }
    check(penX == 2949, "the 563's right edge is at %d, not 2949", penX);
    setSetting("plotterorigin", NULL);
    setSetting("plottermodel", NULL);
    powerCycle();

    // The roll: 100,000 steps each way with the pen down.
    iot(PLNOP);
    plot(PLPD);
    for( i = 0; i < 100000; ++i )
    {
        plot(PLDU);
    }
    for( i = 0; i < 200000; ++i )
    {
        plot(PLDD);
    }
    check(penY == -100000, "the drum is at %lld, not -100000", (long long)penY);
    check(anyInk && (inkTop == -100000) && (inkBottom == 100000), "the ink runs %lld to %lld", (long long)inkTop, (long long)inkBottom);
    check((bandLow <= bandOf(-100000)) && ((bandLow + (int64_t)bandCount) > bandOf(100000)), "the bands do not cover the drawing");

    // Sparse: a dot far down the roll makes one more band, not the ones between.
    plot(PLPU);
    for( i = 0; i < 300000; ++i )
    {
        plot(PLDD);
    }
    plot(PLPD);
    for( allocated = 0, b = 0; b < bandCount; ++b )
    {
        allocated += (bandsP[b] != NULL);
    }
    check(allocated == (size_t)((bandOf(100000) - bandOf(-100000)) + 2), "%zu bands made for two drawn stretches", allocated);

    // The PNG's height is the drawn extent plus the margins; it is checked from the file's IHDR.
    plot(PLPU);
    deadlineArmed = false;
    iotPowerClear();
    iotStop();
    wbDrain(wbP);

    // The ink runs from the dot at -400,000 to the roll's end at 100,000.
    wantH = (uint32_t)(500000 + 1 + (2 * MARGIN_ROWS));
    fP = fopen(DIR "/t5.png", "rb");
    check(fP && (fread(ihdr, 1, 24, fP) == 24), "cannot read t5.png");
    if( fP )
    {
        fclose(fP);
        h = ((uint32_t)ihdr[20] << 24) | ((uint32_t)ihdr[21] << 16) | ((uint32_t)ihdr[22] << 8) | ihdr[23];
        check(h == wantH, "t5.png is %u rows high, not %u", h, wantH);
    }
    unlink(DIR "/t5.png");
}

// Draws the test figure from the pen's start (the left edge, drum 0): an L, 10 drum-ups down the
// picture then 5 pen-rights, a gap, and a dot. Adds it to pP.
static void
drawFigure(Picture *pP)
{
int i;

    plot(PLPD);
    ink(pP, 0, 0);
    for( i = 1; i <= 10; ++i )
    {
        plot(PLDU);
        ink(pP, 0, i);
    }
    for( i = 1; i <= 5; ++i )
    {
        plot(PLPR);
        ink(pP, i, 10);
    }
    plot(PLPU);
    plot(PLPR);
    plot(PLPR);
    plot(PLPD);
    ink(pP, 7, 10);
    plot(PLPU);
}

// T6: when and how the sheet is written.
static void
testWriting(void)
{
Picture fig;
Picture blank;
Picture more;
int i;
int dx;
int dy;
char name[64];

    clearSettings();
    setSetting("plotterfile", DIR "/t6.png");
    setSetting("plotteridle", "5");
    powerCycle();
    unlink(DIR "/t6.png");

    // The idle rewrite: not before plotteridle has passed after the last operation, then once.
    fig = newPicture(1100, -20, 40);
    iot(PLNOP);
    drawFigure(&fig);
    advance(5000 * MS - 1);
    wbDrain(wbP);
    check(!exists(DIR "/t6.png"), "the sheet was written before the plotter had been idle 5 s");
    advance(1);
    wbDrain(wbP);
    check(exists(DIR "/t6.png"), "the sheet was not written after 5 s idle");
    check(!exists(DIR "/t6.png.tmp"), "the .tmp was left behind");
    keep("t6.png", "t6-idle.png");
    expectPicture("t6-idle.png", "t6-idle.pbm", &fig, "pass");
    check(!deadlineArmed, "the idle rewrite rearmed itself with nothing new drawn");

    // Pen-up motion is not drawing: no rewrite is armed for it.
    plot(PLPR);
    check(!deadlineArmed, "a pen-up step armed the idle rewrite");

    // The write at STOP: more ink, then iotStop() with the idle time not yet passed.
    more = newPicture(1100, -20, 40);
    memcpy(more.inkP, fig.inkP, (size_t)(fig.width * fig.rows));
    plot(PLPD);
    ink(&more, penX, penY);
    plot(PLPU);
    iotStop();
    wbDrain(wbP);
    keep("t6.png", "t6-stop.png");
    expectPicture("t6-stop.png", "t6-stop.pbm", &more, "pass");

    // Control: the drawn sheet does not match a blank one.
    blank = newPicture(1100, 0, 0);
    expectPicture("t6-stop.png", "t6-blank.pbm", &blank, "fail");

    // A hlt: the machine stops running and no iotStop() comes. The sheet is written at the first
    // I/O poll that sees it. A wait in an IOT leaves the machine running, and writes nothing.
    pdp.run = 1;
    iotIOPoll(&pdp);
    plot(PLPR);
    plot(PLPD);
    ink(&more, penX, penY);
    plot(PLPU);
    unlink(DIR "/t6.png");
    pdp.ioh = 1;
    iotIOPoll(&pdp);
    wbDrain(wbP);
    check(!exists(DIR "/t6.png"), "the sheet was written during an in-out wait");
    pdp.ioh = 0;
    pdp.run = 0;
    iotIOPoll(&pdp);
    wbDrain(wbP);
    check(exists(DIR "/t6.png"), "the sheet was not written at a hlt");
    keep("t6.png", "t6-halt.png");
    expectPicture("t6-halt.png", "t6-halt.pbm", &more, "pass");

    // %d numbering and a new sheet after each power clear. A sheet the program used but drew
    // nothing on is a blank row; one it never used is not written at all.
    setSetting("plotterfile", DIR "/t6-%d.png");
    powerCycle();
    unlink(DIR "/t6-1.png");
    for( i = 1; i <= 4; ++i )
    {
        snprintf(name, sizeof(name), DIR "/t6-%d.png", (sheetNumber + i));
        unlink(name);
    }

    i = sheetNumber;
    iot(PLNOP);
    fig = newPicture(1100, -20, 40);
    drawFigure(&fig);
    powerCycle();
    iot(PLNOP);
    powerCycle();
    powerCycle();
    snprintf(name, sizeof(name), "t6-%d.png", (i + 1));
    keep(name, "t6-sheet1.png");
    expectPicture("t6-sheet1.png", "t6-sheet1.pbm", &fig, "pass");
    snprintf(name, sizeof(name), "t6-%d.png", (i + 2));
    keep(name, "t6-sheet2.png");
    expectPicture("t6-sheet2.png", "t6-sheet2.pbm", &blank, "pass");
    snprintf(name, sizeof(name), DIR "/t6-%d.png", (i + 3));
    check(!exists(name), "a power cycle with no plotter IOT wrote a sheet");

    // A wide pen: plotterpen 3 makes a 3 by 3 dot.
    setSetting("plotterfile", DIR "/t6-pen.png");
    setSetting("plotterpen", "3");
    setSetting("plotterorigin", "50");
    powerCycle();
    iot(PLNOP);
    plot(PLPD);
    plot(PLPU);
    powerCycle();
    fig = newPicture(1100, -5, 5);
    for( dx = -1; dx <= 1; ++dx )
    {
        for( dy = -1; dy <= 1; ++dy )
        {
            ink(&fig, (50 + dx), dy);
        }
    }
    expectPicture("t6-pen.png", "t6-pen.pbm", &fig, "pass");
    setSetting("plotterpen", NULL);
    setSetting("plotterorigin", NULL);
}

// Runs every group. Returns 0 if all passed.
int
main(void)
{
    mkdir(DIR, 0777);
    if( !(manifestP = fopen(DIR "/manifest.txt", "w")) )
    {
        printf("FAIL cannot write %s/manifest.txt\n", DIR);
        return(2);
    }

    // The plugin as the emulator first loads it: started, then the first poll.
    iotStart();
    iotIOPoll(&pdp);

    runTest(testDecode, "T2 decoding");
    runTest(testTiming, "T3 timing, completion, flag, break");
    runTest(testBusy, "T4 a command while busy");
    runTest(testPaper, "T5 paper");
    runTest(testWriting, "T6 writing");

    fclose(manifestP);
    printf("%d checks, %d failed\n", checkCount, failCount);
    return( (failCount == 0) ? 0 : 1 );
}
