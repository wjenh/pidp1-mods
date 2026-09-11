/*
 * microtape.c -- shim layer between the paper tape reader IOT and the microtape implementation.
 *
 * 11-Sep-2026 wje/Claude - initial version
 */

#define NOT_IN_PDP1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "configuration.h"
#include "iotLogger.h"                      // DOLOGGING is not defined: the calls compile away
#include "microtape.h"

#define LOG_MT_IOT      0
#define LOG_MT_CONFIG   0

// Where the drive list lives, and what relative image paths are relative to. The host tests
// compile with their own.
#ifndef MT_BASE_DIR
#define MT_BASE_DIR     "/opt/pidp1-mods"
#endif
#ifndef MT_LIST_FILE
#define MT_LIST_FILE    MT_BASE_DIR "/microtapes.txt"
#endif

#define SPEC_MAX        (MT_PATH_MAX + 16)  // a drive entry: path plus ",locked"
#define LINE_MAX_LEN    (SPEC_MAX + 32)     // a line of microtapes.txt

extern void initiateBreak(int chan);        // iotHandler.h, in the reader plugin

static Mt550 ctl;                           // the control and its eight drives
static bool ctlReady;                       // ctl initialized and configured once
static int sbsChan = MT_DEFAULT_SBS;        // break channel for every flag
static char listSpec[MT_UNITS + 1][SPEC_MAX];   // each drive's microtapes.txt entry, last read
static bool listFailed[MT_UNITS + 1];       // mounting that entry failed; SIGHUP retries it
static bool ioErrorReported[MT_UNITS + 1];

static void ensureReady(uint64_t now);
static void configure(uint64_t now);
static void readList(char specs[][SPEC_MAX]);
static void applyListEntry(int unit, uint64_t now);
static bool parseSpec(const char *specP, char *pathP, bool *lockedP);
static int mountDrive(int unit, const char *pathP, bool locked, uint64_t now, const char *whoP);
static int mountResolved(int unit, const char *fullP, bool locked, uint64_t now, const char *whoP);
static void unmountDrive(int unit, uint64_t now);
static Word mountIot(PDP1 *pdp1P, uint64_t now);
static bool unpackName(PDP1 *pdp1P, unsigned int addr, char *nameP, size_t size);
static void reportIoErrors(void);

// Break callback from the control: every raise of the data, block end or error flag requests
// a program break on the configured channel (manual p. 2-6). No return value.
static void
requestBreak(void *ctxP)
{
    (void)ctxP;
    initiateBreak(sbsChan);
}

// Returns true if inst, an IOT on device 01, is one of the tape's: sub-device 2 (mount) or 3-7
// (the Type 550). The caller has already checked the device; it must not call this for the
// read-in switch's rpb pulses, whose MB is a dio word, not an IOT.
bool
mtOwnsIot(Word inst)
{
int sub;

    sub = (int)((inst >> 6) & 037);
    return( (sub >= MT_SUB_MOUNT) && (sub <= MT_SUB_MRS) );
}

// Handles the mount IOT and mse, mlc, mrd, mwr and mrs. Called twice per instruction; the work
// is done on the second call (pulse 1, TP10), as the reader does, and both calls are claimed.
// A completion requested by the instruction's wait/completion bits is given at once, as
// IOT_61.c does: every one of these IOTs finishes immediately.
// Returns 1 for the tape's sub-devices, 0 for anything else (the caller then treats it as the
// reader's).
int
mtIotHandler(PDP1 *pdp1P, int pulse, int completion)
{
int sub;
uint64_t now;

    if( !mtOwnsIot(pdp1P->mb) )
    {
        return(0);
    }

    if( !pulse )
    {
        return(1);                          // claimed; acted on at TP10
    }

    sub = (int)((pdp1P->mb >> 6) & 037);
    now = pdp1P->simtime;
    ensureReady(now);

    switch( sub )
    {
    case MT_SUB_MOUNT:
        pdp1P->io = mountIot(pdp1P, now);
        break;

    case MT_SUB_MSE:
        mt550Select(&ctl, now, pdp1P->io);
        break;

    case MT_SUB_MLC:
        mt550LoadControl(&ctl, now, pdp1P->io);
        break;

    case MT_SUB_MRD:
        pdp1P->io = mt550ReadBuffer(&ctl, now);     // "clears I/O and transfers one word"
        break;

    case MT_SUB_MWR:
        mt550WriteBuffer(&ctl, now, pdp1P->io);
        break;

    case MT_SUB_MRS:
        pdp1P->io = mt550Status(&ctl, now);
        break;
    }

    iotCondLog(LOG_MT_IOT, "microtape %06o IO %06o\n", pdp1P->mb, pdp1P->io);

    if( completion )
    {
        pdp1P->ios = 1;                     // IOCOMPLETE(), which lives in iotHandler.h
    }

    reportIoErrors();
    return(1);
}

// Brings every drive up to the current simtime, raising the flags (and breaks) that have come
// due. Called once per main-loop iteration; between events it is one comparison.
// No return value.
void
mtIOPoll(PDP1 *pdp1P)
{
    if( !ctlReady )
    {
        return;                             // not configured yet: no tape can be moving
    }

    mt550Service(&ctl, pdp1P->simtime);
}

// Called when the reader plugin loads and whenever the machine goes from halt to run. The
// first call initializes the control and mounts the tapes in microtapes.txt; later calls
// change nothing, since the tapes do not care whether the CPU is running. No return value.
void
mtStart(void)
{
    ensureReady(0);
}

// Called when the machine halts. Writes any partly written block back to its image file;
// the tapes keep moving (the I/O poll still runs while halted). No return value.
void
mtStop(void)
{
    if( ctlReady )
    {
        mt550FlushAll(&ctl);
        reportIoErrors();
    }
}

// Called on SIGHUP after pidp1.config has been reloaded: picks up a new break channel and
// rereads microtapes.txt (see configure()). No return value.
void
mtUpdate(void)
{
    if( !ctlReady )
    {
        ensureReady(0);
        return;
    }

    configure(ctl.lastTime);
}

// Returns the control, for the host tests (IOTs/Microtape/Tests/plugintest.c).
Mt550P
mtControl(void)
{
    return( &ctl );
}

// Initializes the control and mounts the listed tapes the first time it is called.
// now is the current simtime if known (0 at load time, before any IOT has run).
// No return value.
static void
ensureReady(uint64_t now)
{
    if( ctlReady )
    {
        return;
    }

    mt550Init(&ctl, requestBreak, NULL);
    memset(listSpec, 0, sizeof(listSpec));
    memset(listFailed, 0, sizeof(listFailed));
    memset(ioErrorReported, 0, sizeof(ioErrorReported));
    ctlReady = true;
    configure(now);
}

// Reads the break channel from pidp1.config and the drive list from microtapes.txt, and
// brings each drive in line with it. A drive's line is acted on only if it changed since the
// last read (a new line mounts, a changed one remounts, a removed one unmounts), so a SIGHUP
// neither rewinds a tape in use nor undoes a program's mount IOT. Two more cases are
// remounted: a tape that ran off its reel (the emulator's way of rethreading it), and a line
// whose mount failed last time. No return value.
static void
configure(uint64_t now)
{
ConfigurationSettingP settingP;
Mt555UnitP uP;
char specs[MT_UNITS + 1][SPEC_MAX];
char path[MT_PATH_MAX];
bool locked;
int unit;

    sbsChan = MT_DEFAULT_SBS;
    if( (settingP = findConfigurationSetting(getConfiguration(), "microtapesbs")) )
    {
        if( (settingP->ivalue >= 0) && (settingP->ivalue <= 15) && !settingP->strvalueP )
        {
            sbsChan = settingP->ivalue;
        }
    }

    readList(specs);

    for( unit = 1; unit <= MT_UNITS; ++unit )
    {
        uP = mt550Unit(&ctl, unit);
        if( strcmp(specs[unit], listSpec[unit]) != 0 )
        {
            strcpy(listSpec[unit], specs[unit]);
            applyListEntry(unit, now);
        }
        else if( uP->mounted && uP->offReel )
        {
            // Rethread it: the same image, lock and all, back at the load point. The unit's
            // path is already resolved.
            snprintf(path, sizeof(path), "%s", uP->path);
            locked = uP->locked;
            mountResolved(unit, path, locked, now, "microtape");
        }
        else if( listFailed[unit] )
        {
            applyListEntry(unit, now);
        }
    }
}

// Reads microtapes.txt into specs[1..8] ("" for a drive with no line). A line is
// "<drive> <path>[,locked]": the drive number in decimal, 1-8 (the octal 01-10 of the mse
// field), then at least one space or tab, then the rest of the line with trailing spaces
// dropped. Blank lines and lines whose first non-space character is # are skipped. A bad line
// is reported on stderr and skipped; a drive listed twice gets the later line. A missing file
// means no tapes. No return value.
static void
readList(char specs[][SPEC_MAX])
{
FILE *fP;
char line[LINE_MAX_LEN];
char *cP;
char *endP;
size_t len;
long unit;
int lineNo;
int ch;

    memset(specs, 0, (sizeof(specs[0]) * (MT_UNITS + 1)));

    if( !(fP = fopen(MT_LIST_FILE, "r")) )
    {
        return;
    }

    for( lineNo = 1; fgets(line, sizeof(line), fP); ++lineNo )
    {
        len = strlen(line);
        if( (len > 0) && (line[len - 1] != '\n') && !feof(fP) )
        {
            while( ((ch = fgetc(fP)) != EOF) && (ch != '\n') )
            {
                // discard the rest of the line
            }
            continue;
        }

        while( (len > 0) && ((line[len - 1] == '\n') || (line[len - 1] == '\r') || (line[len - 1] == ' ')
            || (line[len - 1] == '\t')) )
        {
            line[--len] = 0;
        }

        for( cP = line; (*cP == ' ') || (*cP == '\t'); ++cP )
        {
            // skip leading white space
        }

        if( (*cP == 0) || (*cP == '#') )
        {
            continue;
        }

        errno = 0;
        unit = strtol(cP, &endP, 10);
        if( (endP == cP) || ((*endP != ' ') && (*endP != '\t')) )
        {
            continue;
        }

        if( (unit < 1) || (unit > MT_UNITS) || (errno != 0) )
        {
            continue;
        }

        for( cP = endP; (*cP == ' ') || (*cP == '\t'); ++cP )
        {
            // skip to the path
        }

        if( strlen(cP) >= SPEC_MAX )
        {
            continue;
        }

        strcpy(specs[unit], cP);
    }

    fclose(fP);
}

// Mounts, or for an empty entry unmounts, drive unit from its microtapes.txt entry, and
// records whether that failed so the next SIGHUP retries it. No return value.
static void
applyListEntry(int unit, uint64_t now)
{
char path[MT_PATH_MAX];
char who[32];
bool locked;

    listFailed[unit] = false;
    snprintf(who, sizeof(who), "microtape%d", unit);

    if( listSpec[unit][0] == 0 )
    {
        unmountDrive(unit, now);
        return;
    }

    if( !parseSpec(listSpec[unit], path, &locked) )
    {
        unmountDrive(unit, now);
        listFailed[unit] = true;
        return;
    }

    listFailed[unit] = (mountDrive(unit, path, locked, now, who) == MT_MOUNT_FAILED);
}

// Splits an entry "path[,locked]" into the path (at most MT_PATH_MAX - 1 characters, written
// to pathP) and the lock flag.
// Returns true if a path was found, false for an empty or over-long value.
static bool
parseSpec(const char *specP, char *pathP, bool *lockedP)
{
const char *commaP;
size_t len;

    *lockedP = false;
    len = strlen(specP);
    if( ((commaP = strrchr(specP, ',')) != NULL) && (strcmp(commaP, ",locked") == 0) )
    {
        *lockedP = true;
        len = (size_t)(commaP - specP);
    }

    if( (len == 0) || (len >= MT_PATH_MAX) )
    {
        return(false);
    }

    memcpy(pathP, specP, len);
    pathP[len] = 0;
    return(true);
}

// Puts the image at pathP (relative to MT_BASE_DIR unless it starts with /) on drive unit,
// replacing whatever was there; see mountResolved(). A path too long to resolve is reported
// on stderr under whoP and leaves the drive with no tape.
// Returns MT_MOUNT_OK, MT_MOUNT_LOCKED or MT_MOUNT_FAILED, as mountResolved().
static int
mountDrive(int unit, const char *pathP, bool locked, uint64_t now, const char *whoP)
{
char full[MT_PATH_MAX];
int len;

    if( pathP[0] == '/' )
    {
        len = snprintf(full, sizeof(full), "%s", pathP);
    }
    else
    {
        len = snprintf(full, sizeof(full), "%s/%s", MT_BASE_DIR, pathP);
    }

    if( (len < 0) || (len >= (int)sizeof(full)) )
    {
        unmountDrive(unit, now);
        return(MT_MOUNT_FAILED);
    }

    return( mountResolved(unit, full, locked, now, whoP) );
}

// Puts the image at fullP, a path already resolved against MT_BASE_DIR (a unit's own path is
// one), on drive unit, replacing whatever was there. An unlocked image that does not exist is
// created as a blank tape, and that is reported on stderr. A file already mounted on another
// drive is refused, since each drive would write back its own copy. Any failure is reported
// on stderr under whoP and leaves the drive with no tape.
// Returns MT_MOUNT_OK, MT_MOUNT_LOCKED (mounted write-locked, asked for or because the file
// is read-only), or MT_MOUNT_FAILED.
static int
mountResolved(int unit, const char *fullP, bool locked, uint64_t now, const char *whoP)
{
Mt555UnitP uP;
char err[MT_PATH_MAX + 160];
int other;
int result;

    uP = mt550Unit(&ctl, unit);
    mt555Unmount(uP);
    ioErrorReported[unit] = false;
    result = MT_MOUNT_FAILED;

    for( other = 1; other <= MT_UNITS; ++other )
    {
        if( (other != unit) && mt555SameFile(mt550Unit(&ctl, other), fullP) )
        {
            break;
        }
    }

    if( (other > MT_UNITS) && mt555MountFile(uP, fullP, locked, true, err, (int)sizeof(err)) )
    {
        iotCondLog(LOG_MT_CONFIG, "%s: mounted %s%s\n", whoP, fullP, (uP->locked ? " (locked)" : ""));
        result = (uP->locked ? MT_MOUNT_LOCKED : MT_MOUNT_OK);
    }

    mt550UnitRemounted(&ctl, unit, now);
    return(result);
}

// Takes the tape off drive unit, writing back any partly written block first. No return value.
static void
unmountDrive(int unit, uint64_t now)
{
    mt555Unmount(mt550Unit(&ctl, unit));
    ioErrorReported[unit] = false;
    mt550UnitRemounted(&ctl, unit, now);
}

// The mount IOT, 720201 is non-historic.
// IO bits 2-5 name the drive, as for mse
// (mtu1-mtu8); the other IO bits are ignored. AC, masked to 16 bits, is the address of the
// image's name, packed ascii as am1's ascii directive makes it; see unpackName(). The tape
// replaces whatever the drive held and is mounted unlocked; a missing image is created as a
// blank tape. AC = 0 (after masking) unmounts the drive. The drive's microtapes.txt line, if
// any, no longer applies until the operator changes it.
// Returns the value for IO: MT_MOUNT_OK, MT_MOUNT_LOCKED if the image file is read-only, or
// MT_MOUNT_FAILED. A bad drive number changes nothing; any other failure leaves the drive empty.
static Word
mountIot(PDP1 *pdp1P, uint64_t now)
{
char name[MT_PATH_MAX];
char who[32];
unsigned int addr;
int unit;

    unit = (int)((pdp1P->io >> MT_SEL_SHIFT) & MT_SEL_MASK);
    if( (unit < 1) || (unit > MT_UNITS) )
    {
        return(MT_MOUNT_FAILED);
    }

    snprintf(who, sizeof(who), "microtape%d (IOT 201)", unit);
    listFailed[unit] = false;               // the program has taken the drive over
    addr = (unsigned int)(pdp1P->ac & 0177777);

    if( addr == 0 )
    {
        unmountDrive(unit, now);
        return(MT_MOUNT_OK);
    }

    if( !unpackName(pdp1P, addr, name, sizeof(name)) )
    {
        unmountDrive(unit, now);
        return(MT_MOUNT_FAILED);
    }

    return( (Word)mountDrive(unit, name, false, now, who) );
}

// Unpacks the packed-ascii string at addr into nameP (size bytes): two characters per word,
// the first in the high 9 bits and the second in the low 9 bits, ending with a zero character,
// as am1's ascii directive packs them. The string may not run past the end of addr's bank: the
// word holding the terminating zero may be the bank's last (x7777), no further.
// Returns true for a nonempty name of printable ASCII (040-0176) that fits in size bytes with
// its terminator; false otherwise (no terminator in the bank, an empty name, a character
// outside that range -- which also catches an address that holds no string -- or too long).
static bool
unpackName(PDP1 *pdp1P, unsigned int addr, char *nameP, size_t size)
{
unsigned int last;
size_t n;
Word word;
int ch;
int half;

    last = (addr | 07777);
    n = 0;

    for( ; addr <= last; ++addr )
    {
        word = pdp1P->core[addr];
        for( half = 0; half < 2; ++half )
        {
            ch = (int)((half == 0) ? ((word >> 9) & 0777) : (word & 0777));
            if( ch == 0 )
            {
                nameP[n] = 0;
                return( n > 0 );
            }

            if( (ch < 040) || (ch > 0176) || ((n + 1) >= size) )
            {
                return(false);
            }

            nameP[n++] = (char)ch;
        }
    }

    return(false);                          // no terminator before the end of the bank
}

// Prints, once per mount, any failure to write a drive's image file (a block write-back, or
// the truncation of a mode 7 erase). No return value.
static void
reportIoErrors(void)
{
Mt555UnitP uP;
int unit;

    for( unit = 1; unit <= MT_UNITS; ++unit )
    {
        uP = mt550Unit(&ctl, unit);
        if( uP->ioError && !ioErrorReported[unit] )
        {
            ioErrorReported[unit] = true;
        }
    }
}
