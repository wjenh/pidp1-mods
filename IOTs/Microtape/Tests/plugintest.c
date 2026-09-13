/*
 * plugintest.c -- host tests for the Microtape as the reader plugin carries it.
 *
 * Purpose: Magtape/TASK-REWORK.md moved the tape out of its own plugin (and the IOT loader)
 * into the paper tape reader plugin. This drives the real reader plugin source
 * (IOTs/Reader/IOT_2.c) and the real shim (../microtape.c), linked with the core, through the
 * plugin's own entry points -- iotHandler(), iotIOPoll(), iotStart(), iotUpdate() -- exactly as
 * dynamicIots.c would, and checks:
 *   - routing: device-01 sub-devices 2-7 reach the tape; rpa (sub-device 0) and the other
 *     sub-devices reach the reader; read-in's rpb (device 2), whose MB is a dio word, always
 *     reaches the reader, with a control leg showing the same MB on device 1 goes to the tape;
 *   - microtapes.txt: comments, blank lines, relative and absolute paths, a path with a space,
 *     ",locked", a missing locked image (refused, not created), a missing unlocked one
 *     (created), a bad drive number, a duplicate drive, a malformed line;
 *   - the mount IOT 201: drive in IO bits 2-5, AC masked to 16 bits, AC = 0 unmounts, the
 *     result in IO, the bank-end rule (a control leg ending exactly at x7777), bad names, a
 *     file already on another drive, a file of the wrong size left untouched, a read-only file;
 *   - SIGHUP (iotUpdate): an unchanged line leaves a program's mount alone, a changed line
 *     wins, a removed line unmounts, a failed line is retried, an off-reel tape is rethreaded;
 *     the break channel follows microtapesbs;
 *   - All Halt (TASK-TAPE-HALT-WRITE): the I/O poll stops every moving drive when RUN falls,
 *     and does nothing while RUN stays 0 or when it rises;
 *   - mse rereads microtapes.txt: an edited, renamed-over, removed or restored list takes
 *     effect at the next mse; an unchanged file undoes no program mount and rewinds no tape;
 *     two tapes can trade drives in one edit; a bad line is reported once.
 *
 * Architectural scope: a standalone host program. It stands in for the emulator: the PDP1
 * struct, the configuration (getConfiguration()/findConfigurationSetting()) and the break
 * hook dynamicIotProcessBreak(). Built with MT_BASE_DIR = "plugintest.d", so microtapes.txt
 * and the relative images live in that directory, which it creates and removes.
 *
 * Dependencies: ../../Reader/IOT_2.c, ../microtape.[ch], ../control550.[ch],
 * ../transport555.[ch], pdp1.h, configuration.h; POSIX file calls.
 *
 * Execution model: single-threaded; simtime is advanced by hand.
 *
 * Usage: plugintest (make plugintest). Prints one PASS/FAIL line per group, every failed check
 * and a summary; exit status 0 if everything passed. The plugin's own stderr messages (for
 * the deliberately bad lines and names) are expected.
 *
 * 11-Sep-2026 Claude -- initial version, for Magtape/TASK-REWORK.md.
 * 13-Sep-2026 Claude -- All Halt (MiscTasks/Completed/TASK-TAPE-HALT-WRITE.md); mse rereads the list.
 */

#define NOT_IN_PDP1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "pdp1.h"
#include "configuration.h"
#include "microtape.h"

#define DIR             "plugintest.d"
#define LIST            DIR "/microtapes.txt"
#define MS              1000000ULL

// The reader plugin's exports (IOT_2.c), as dynamicIots.c finds them.
int iotHandler(PDP1 *pdp1P, int device, int pulse, int completion);
void iotIOPoll(PDP1 *pdp1P);
void iotStart(void);
void iotStop(void);
void iotUpdate(void);

static PDP1 pdp;                            // the machine
static int breakCounts[16];                 // break requests per channel
static ConfigurationSetting sbsSetting;     // microtapesbs
static Configuration config;
static char cwd[512];

static const char *testNameP;
static int checkCount;
static int failCount;

// ---- The emulator's side ------------------------------------------------------------------

// The break hook iotHandler.h's initiateBreak() calls. Counts the request. No return value.
void
dynamicIotProcessBreak(int chan)
{
    if( (chan >= 0) && (chan < 16) )
    {
        ++breakCounts[chan];
    }
}

// Returns the configuration: only the microtapesbs extra setting.
ConfigurationP
getConfiguration(void)
{
    return( &config );
}

// Returns the extra setting called nameP, or NULL.
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

// ---- Reporting ----------------------------------------------------------------------------

// Counts one check; on failure prints the test name and the printf-style message.
// No return value.
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

// Runs one test group and prints PASS or FAIL for it. No return value.
static void
runTest(void (*fnP)(void), const char *nameP)
{
int before;

    testNameP = nameP;
    before = failCount;
    fnP();
    printf("%s  %s\n", ((failCount == before) ? "PASS" : "FAIL"), nameP);
}

// ---- Helpers ------------------------------------------------------------------------------

// Executes one IOT on device the way dynamicIotProcessor() does: MB holds inst, and the
// handler is called for both pulses with the completion value computed from bits 5 and 6.
// No return value.
static void
iot(int device, Word inst)
{
int completion;

    pdp.mb = inst;
    completion = (((inst & 014000) == 010000) || ((inst & 014000) == 004000));
    iotHandler(&pdp, device, 0, completion);
    iotHandler(&pdp, device, 1, completion);
}

// Returns drive unit of the tape.
static Mt555UnitP
unit(int u)
{
    return( mt550Unit(mtControl(), u) );
}

// Returns true if drive u has a tape whose path ends with tailP.
static bool
mountedAs(int u, const char *tailP)
{
size_t len;
size_t tail;

    if( !unit(u)->mounted )
    {
        return(false);
    }

    len = strlen(unit(u)->path);
    tail = strlen(tailP);
    return( (len >= tail) && (strcmp(unit(u)->path + len - tail, tailP) == 0) );
}

// Returns the size of the file at pathP in bytes, or -1 if it does not exist.
static long long
fileSize(const char *pathP)
{
struct stat st;

    return( (stat(pathP, &st) == 0) ? (long long)st.st_size : -1 );
}

// Writes bytes zero bytes to pathP, replacing it. Returns true on success.
static bool
makeFile(const char *pathP, size_t bytes)
{
FILE *fP;
size_t i;
bool ok;

    if( !(fP = fopen(pathP, "wb")) )
    {
        return(false);
    }

    ok = true;
    for( i = 0; (i < bytes) && ok; ++i )
    {
        ok = (fputc(0, fP) != EOF);
    }

    return( (fclose(fP) == 0) && ok );
}

// Writes textP as the whole of microtapes.txt. No return value.
static void
writeList(const char *textP)
{
FILE *fP;

    if( (fP = fopen(LIST, "w")) )
    {
        fputs(textP, fP);
        fclose(fP);
    }
}

// Packs strP into core at addr as am1's ascii directive does: two characters a word, high 9
// bits first, ending with a zero character. No return value.
static void
putString(unsigned int addr, const char *strP)
{
size_t len;
size_t i;
Word hi;
Word lo;

    len = strlen(strP);
    for( i = 0; i <= len; i += 2 )
    {
        hi = (Word)(unsigned char)strP[i];
        lo = ((i + 1) <= len) ? (Word)(unsigned char)strP[i + 1] : 0;
        pdp.core[addr++] = ((hi << 9) | lo);
    }
}

// Issues the mount IOT for drive u (1-8, or anything to put in IO bits 2-5) with AC = ac.
// Returns the result the IOT leaves in IO.
static Word
mount(int u, Word ac)
{
    pdp.io = ((Word)u << 12);
    pdp.ac = ac;
    iot(1, 0720201);
    return( pdp.io );
}

// Removes everything in the test directory and the directory itself. No return value.
static void
cleanDir(void)
{
    if( system("rm -rf " DIR) != 0 )
    {
        printf("    note: could not remove %s\n", DIR);
    }
}

// ---- Tests --------------------------------------------------------------------------------

// Loads the plugin: iotStart() reads microtapes.txt. The list exercises the parser.
static void
testList(void)
{
char text[2048];

    makeFile(DIR "/t2.img", 0);
    snprintf(text, sizeof(text),
        "# microtapes.txt for plugintest\n"
        "\n"
        "   \t\n"
        "1 t1.img\n"
        "2   %s/" DIR "/t2.img,locked   \n"
        "3 missing3.img,locked\n"
        "4\tmy tape.img\n"
        "9 t9.img\n"
        "5 first5.img\n"
        "5 second5.img\n"
        "x junk.img\n"
        "6 nodir/t6.img\n", cwd);
    writeList(text);

    iotStart();

    check((mountedAs(1, DIR "/t1.img") && unit(1)->created && !unit(1)->locked), "drive 1 not mounted and created");
    check((fileSize(DIR "/t1.img") == 0), "t1.img: not created empty (%lld bytes)", fileSize(DIR "/t1.img"));
    check((mountedAs(2, "/" DIR "/t2.img") && unit(2)->locked && (unit(2)->path[0] == '/')),
        "drive 2: absolute path with ,locked and trailing spaces not mounted locked");
    check((!unit(3)->mounted && (fileSize(DIR "/missing3.img") < 0)), "drive 3: missing locked image mounted or created");
    check((mountedAs(4, DIR "/my tape.img") && (fileSize(DIR "/my tape.img") == 0)), "drive 4: path with a space");
    check((fileSize(DIR "/t9.img") < 0), "drive 9 was acted on");
    check((mountedAs(5, "second5.img") && (fileSize(DIR "/first5.img") < 0)), "drive 5: the later line did not win");
    check((fileSize(DIR "/junk.img") < 0), "a malformed line was acted on");
    check(!unit(6)->mounted, "drive 6: a path in a missing directory mounted");
    check((!unit(7)->mounted && !unit(8)->mounted), "drives 7 and 8 have tapes");
}

// Device-01 routing, and read-in's rpb.
static void
testRouting(void)
{
Mt550P cP;

    cP = mtControl();
    pdp.r_fd = -1;
    pdp.simtime = (1000 * MS);

    // rpa (sub-device 0) arms the reader: alphanumeric framing.
    pdp.rc = 99;
    pdp.rcl = 0;
    iot(1, 0730001);
    check(((pdp.rby == 0) && (pdp.rc == 3) && (pdp.rcl == 1)), "rpa did not arm the reader (rby %d rc %d rcl %d)",
        pdp.rby, pdp.rc, pdp.rcl);
    check((cP->selUnit == 0), "rpa reached the tape");

    // Sub-devices 1 and 010 are not the tape's: the reader, as before.
    pdp.rc = 99;
    iot(1, 0720101);
    check((pdp.rc == 3), "sub-device 1 did not reach the reader");
    pdp.rc = 99;
    iot(1, 0721001);
    check((pdp.rc == 3), "sub-device 010 did not reach the reader");

    // mse (sub-device 3) reaches the tape and leaves the reader alone.
    pdp.rc = 99;
    pdp.io = 030000;
    iot(1, 0720301);
    check(((cP->selUnit == 3) && (pdp.rc == 99)), "mse: unit %d, rc %d; want unit 3 and the reader untouched",
        cP->selUnit, pdp.rc);

    // Read-in: rpb on device 2 with MB holding a dio word whose bits 7-11 are 3. The reader.
    pdp.rc = 99;
    pdp.io = 050000;
    iot(2, 0320301);
    check(((pdp.rby == 1) && (pdp.rc == 1) && (cP->selUnit == 3)),
        "rpb with a dio word in MB: rby %d rc %d unit %d; want the reader, unit still 3", pdp.rby, pdp.rc, cP->selUnit);

    // Control leg: the same MB on device 1 is mse, and selects unit 5.
    pdp.io = 050000;
    iot(1, 0320301);
    check((cP->selUnit == 5), "control leg: MB 320301 on device 1 selected unit %d, want 5", cP->selUnit);

    // The completion bits: mse C gives the completion at once; plain mse gives none.
    pdp.ios = 0;
    pdp.io = 010000;
    iot(1, 0724301);
    check((pdp.ios == 1), "mse C: no completion");
    pdp.ios = 0;
    iot(1, 0720301);
    check((pdp.ios == 0), "mse: a completion nobody asked for");

    // mrs answers in IO: drive 1 has a tape, so mlc stop is accepted and the status is 0.
    pdp.io = 0;
    iot(1, 0720401);
    iot(1, 0720701);
    check((pdp.io == 0), "mrs after mlc 0 on drive 1: %06o, want 0", pdp.io);
    pdp.io = 030000;
    iot(1, 0720301);
    pdp.io = 040;
    iot(1, 0720401);
    iot(1, 0720701);
    check((pdp.io == (MT_ST_ERF | MT_ST_UNABLE)), "mrs after mlc on tapeless drive 3: %06o, want 101000", pdp.io);
}

// The mount IOT 201.
static void
testMountIot(void)
{
char name[600];
Word r;
int i;

    // A relative name in bank 1; the file is created.
    putString(010100, "t7.img");
    r = mount(7, 010100);
    check(((r == MT_MOUNT_OK) && mountedAs(7, DIR "/t7.img") && (fileSize(DIR "/t7.img") == 0)),
        "mount t7.img: IO %06o", r);

    // AC is masked to 16 bits; remounting the same file on the same drive is fine.
    r = mount(7, (0600000 | 010100));
    check(((r == MT_MOUNT_OK) && mountedAs(7, "t7.img")), "AC 610100 not masked to 010100: IO %06o", r);

    // An absolute name, in bank 0.
    snprintf(name, sizeof(name), "%s/" DIR "/abs8.img", cwd);
    putString(03000, name);
    r = mount(8, 03000);
    check(((r == MT_MOUNT_OK) && mountedAs(8, "/" DIR "/abs8.img")), "absolute name: IO %06o", r);

    // The file on drive 7 cannot also go on drive 8; drive 8 is left empty.
    r = mount(8, 010100);
    check(((r == MT_MOUNT_FAILED) && !unit(8)->mounted && mountedAs(7, "t7.img")),
        "the same file on two drives: IO %06o", r);

    // AC = 0 unmounts, and so does an AC that masks to 0.
    r = mount(7, 0);
    check(((r == MT_MOUNT_OK) && !unit(7)->mounted), "AC 0 did not unmount: IO %06o", r);
    mount(7, 010100);
    r = mount(7, 0200000);
    check(((r == MT_MOUNT_OK) && !unit(7)->mounted), "AC 200000 did not unmount: IO %06o", r);

    // A bad drive number fails and changes nothing.
    mount(7, 010100);
    r = mount(0, 0);
    check(((r == MT_MOUNT_FAILED) && unit(7)->mounted), "drive 0: IO %06o, or it changed a drive", r);
    r = mount(011, 010100);
    check((r == MT_MOUNT_FAILED), "drive 011: IO %06o", r);

    // The bank end. "abcd" in 017776-017777 with its zero in 020000: past the bank, refused.
    // Control leg: "abc" in 017776-017777 with the zero in the low half of 017777 is fine.
    pdp.core[017776] = ((Word)'a' << 9) | 'b';
    pdp.core[017777] = ((Word)'c' << 9) | 'd';
    pdp.core[020000] = 0;
    r = mount(6, 017776);
    check(((r == MT_MOUNT_FAILED) && !unit(6)->mounted && (fileSize(DIR "/abcd") < 0)),
        "a name running past the bank end: IO %06o", r);
    pdp.core[017777] = ((Word)'c' << 9);
    r = mount(6, 017776);
    check(((r == MT_MOUNT_OK) && mountedAs(6, DIR "/abc")), "a name ending in the bank's last word: IO %06o", r);

    // Bad names: empty, a control character, too long.
    pdp.core[04000] = 0;
    r = mount(6, 04000);
    check(((r == MT_MOUNT_FAILED) && !unit(6)->mounted), "an empty name: IO %06o", r);
    pdp.core[04000] = ((Word)'x' << 9) | 001;
    pdp.core[04001] = 0;
    check((mount(6, 04000) == MT_MOUNT_FAILED), "a name with a control character mounted");
    for( i = 0; i < 300; ++i )
    {
        name[i] = 'n';
    }
    name[300] = 0;
    putString(05000, name);
    check((mount(6, 05000) == MT_MOUNT_FAILED), "a 300-character name mounted");

    // An existing file that is not an image is refused and left as it was.
    makeFile(DIR "/bad.img", 5);
    putString(06000, "bad.img");
    r = mount(6, 06000);
    check(((r == MT_MOUNT_FAILED) && (fileSize(DIR "/bad.img") == 5)), "a 5-byte file: IO %06o, size %lld",
        r, fileSize(DIR "/bad.img"));

    // A read-only file mounts write-locked, and says so (not checkable as root).
    if( geteuid() != 0 )
    {
        makeFile(DIR "/ro.img", 0);
        chmod(DIR "/ro.img", 0444);
        putString(06100, "ro.img");
        r = mount(6, 06100);
        check(((r == MT_MOUNT_LOCKED) && unit(6)->locked), "read-only file: IO %06o, want 1", r);
    }
    else
    {
        printf("    note: running as root: read-only file check skipped\n");
    }

    // A program's mount replaces a tape from the list.
    putString(06200, "prog1.img");
    r = mount(1, 06200);
    check(((r == MT_MOUNT_OK) && mountedAs(1, "prog1.img")), "mount over drive 1's listed tape: IO %06o", r);
}

// SIGHUP: iotUpdate() rereads microtapes.txt, and the break channel.
static void
testUpdate(void)
{
Mt550P cP;
uint64_t limit;
int before;
int chan;

    cP = mtControl();

    // The list as loaded, unchanged: drive 1 keeps the program's tape, and drive 7, which has
    // no line, keeps the one a program put there.
    iotUpdate();
    check(mountedAs(1, "prog1.img"), "an unchanged line undid a program's mount");
    check(mountedAs(7, "t7.img"), "SIGHUP disturbed drive 7, which has no line");

    // A failed line is retried: drive 3's missing locked image now exists. Drive 6's line
    // failed too, but a program has used the drive since, so the line is not retried even
    // though its directory now exists.
    makeFile(DIR "/missing3.img", 0);
    mkdir(DIR "/nodir", 0755);
    iotUpdate();
    check((mountedAs(3, "missing3.img") && unit(3)->locked), "drive 3's failed line was not retried");
    check(!mountedAs(6, "nodir/t6.img"), "drive 6's failed line was retried after a program took the drive");

    // A changed line wins over the program's mount; a removed line unmounts.
    writeList("1 t1b.img\n"
        "3 missing3.img,locked\n"
        "5 second5.img\n"
        "6 nodir/t6.img\n");
    iotUpdate();
    check(mountedAs(1, "t1b.img"), "a changed line did not remount drive 1");
    check(!unit(4)->mounted, "a removed line did not unmount drive 4");
    check(!unit(2)->mounted, "a removed line did not unmount drive 2");
    check((mountedAs(3, "missing3.img") && mountedAs(5, "second5.img") && mountedAs(7, "t7.img")),
        "an unchanged line disturbed drive 3, 5 or 7");

    // A tape off its reel is rethreaded from the same file.
    unit(5)->offReel = true;
    iotUpdate();
    check((mountedAs(5, "second5.img") && !unit(5)->offReel), "an off-reel tape was not rethreaded");

    // The break channel: microtapesbs 5, then 7. Drive 1 searches from the load point; its
    // first block mark comes within about 2 s.
    for( chan = 5; chan <= 7; chan += 2 )
    {
        sbsSetting.ivalue = chan;
        iotUpdate();
        memset(breakCounts, 0, sizeof(breakCounts));
        pdp.io = 010000;
        iot(1, 0720301);
        pdp.io = (MT_CTL_GO | MT_MODE_SEARCH);
        iot(1, 0720401);
        before = breakCounts[chan];
        limit = (pdp.simtime + (3000 * MS));
        while( (breakCounts[chan] == before) && (pdp.simtime < limit) )
        {
            pdp.simtime += 100000;
            iotIOPoll(&pdp);
        }
        check(((breakCounts[chan] > before) && cP->df), "microtapesbs %d: no search flag break on it", chan);
        check(((breakCounts[(chan == 5) ? 7 : 5] == 0) && (breakCounts[2] == 0)),
            "microtapesbs %d: a break on another channel", chan);
        pdp.io = 0;
        iot(1, 0720401);                    // stop
    }

    iotStop();
}

// Advances simtime by ms milliseconds in 1 ms steps, polling the plugin at each, as the main
// loop does. No return value.
static void
pollFor(int ms)
{
int i;

    for( i = 0; i < ms; ++i )
    {
        pdp.simtime += MS;
        iotIOPoll(&pdp);
    }
}

// All Halt through the plugin's I/O poll: RUN held at 0 (as the rest of this program runs,
// and as the machine is at load time) stops nothing; RUN falling from 1 to 0 stops every
// moving drive, the selected one and a deselected one; RUN rising starts none of them.
static void
testAllHalt(void)
{
    // Drive 5 moving, then deselected; drive 1 selected and searching. RUN is 0 throughout.
    pdp.run = 0;
    pdp.io = 050000;
    iot(1, 0720301);
    pdp.io = (MT_CTL_GO | MT_MODE_MOVE);
    iot(1, 0720401);
    pdp.io = 010000;
    iot(1, 0720301);
    pdp.io = (MT_CTL_GO | MT_MODE_SEARCH);
    iot(1, 0720401);
    pollFor(300);
    check(((unit(1)->motion == MT_CRUISE) && (unit(5)->motion == MT_CRUISE) && unit(1)->goCmd && unit(5)->goCmd),
        "RUN held at 0 stopped a tape");

    // RUN rises: nothing changes.
    pdp.run = 1;
    pollFor(5);
    check(((unit(1)->motion == MT_CRUISE) && (unit(5)->motion == MT_CRUISE)), "RUN rising stopped a tape");

    // RUN falls: both stop, and GO reads 0.
    pdp.run = 0;
    pollFor(1);
    check(((unit(1)->motion == MT_DECEL) && (unit(5)->motion == MT_DECEL) && !unit(1)->goCmd && !unit(5)->goCmd),
        "RUN falling did not stop both tapes");
    iot(1, 0720701);
    check(((pdp.io & MT_ST_GO) == 0), "after RUN fell, mrs shows GO (%06o)", pdp.io);

    // RUN rises again: they stay stopped until a program commands them.
    pdp.run = 1;
    pollFor(1000);
    check(((unit(1)->motion == MT_STOPPED) && (unit(5)->motion == MT_STOPPED)), "a tape moved again after RUN rose");
    pdp.run = 0;
    pollFor(1);
}

// Sends everything written to stderr to DIR/stderr.txt while on is true, and back to the
// terminal when it is false. No return value.
static void
captureStderr(bool on)
{
static int saved = -1;
int fd;

    fflush(stderr);
    if( on && (saved < 0) )
    {
        saved = dup(2);
        if( (fd = open(DIR "/stderr.txt", (O_WRONLY | O_CREAT | O_TRUNC), 0644)) >= 0 )
        {
            dup2(fd, 2);
            close(fd);
        }
    }
    else if( !on && (saved >= 0) )
    {
        dup2(saved, 2);
        close(saved);
        saved = -1;
    }
}

// Returns how many times strP occurs in DIR/stderr.txt.
static int
stderrCount(const char *strP)
{
char buf[4096];
FILE *fP;
char *cP;
size_t len;
int n;

    n = 0;
    if( (fP = fopen(DIR "/stderr.txt", "r")) )
    {
        len = fread(buf, 1, (sizeof(buf) - 1), fP);
        buf[len] = 0;
        fclose(fP);
        for( cP = buf; (cP = strstr(cP, strP)); ++cP )
        {
            ++n;
        }
    }

    return(n);
}

// Issues mse for drive u. No return value.
static void
mse(int u)
{
    pdp.io = ((Word)u << 12);
    iot(1, 0720301);
}

// mse rereads microtapes.txt (owner, 13-Sep-2026): an edited list takes effect at the next
// mse, with no SIGHUP; an unchanged file changes nothing.
static void
testSelectReread(void)
{
Mt550P cP;
FILE *fP;
int64_t pos;
const char *baseP;

    cP = mtControl();

    // As testUpdate left it: drive 1 t1b.img, 3 missing3.img locked, 5 second5.img, from the
    // list; drive 7 a program's t7.img. A program mounts over drive 1, and mse with the file
    // unchanged undoes nothing.
    putString(06300, "prog1b.img");
    mount(1, 06300);
    mse(2);
    check((mountedAs(1, "prog1b.img") && mountedAs(7, "t7.img") && mountedAs(3, "missing3.img")),
        "mse with the list unchanged disturbed a drive");
    check((cP->selUnit == 2), "mse selected unit %d, want 2", cP->selUnit);

    // A changed line takes effect at the next mse, and the mse still selects.
    baseP = "1 t1c.img\n3 missing3.img,locked\n5 second5.img\n";
    writeList(baseP);
    mse(4);
    check((mountedAs(1, "t1c.img") && (fileSize(DIR "/t1c.img") == 0)), "mse did not remount drive 1 from the edited list");
    check((cP->selUnit == 4), "mse after an edit selected unit %d, want 4", cP->selUnit);
    check((mountedAs(3, "missing3.img") && mountedAs(5, "second5.img") && mountedAs(7, "t7.img")),
        "an edit to drive 1's line disturbed drive 3, 5 or 7");

    // Rewritten with the same bytes: a changed file, but no changed line, so the tape in use is
    // not rewound. Drive 1 is moved off the load point first.
    mse(1);
    pdp.io = (MT_CTL_GO | MT_MODE_MOVE);
    iot(1, 0720401);
    pollFor(400);
    pdp.io = 0;
    iot(1, 0720401);
    pollFor(300);
    pos = mt555Position(unit(1), pdp.simtime);
    check((pos != MT_LOAD_POS), "drive 1 did not leave the load point, so the next check proves nothing");
    writeList(baseP);
    mse(1);
    check((mountedAs(1, "t1c.img") && (mt555Position(unit(1), pdp.simtime) == pos)),
        "rewriting the list unchanged rewound or remounted drive 1");

    // Two tapes trade drives in one edit: every changed drive is emptied before any is
    // mounted, so neither image is refused as already on the other drive.
    writeList("1 second5.img\n3 missing3.img,locked\n5 t1c.img\n");
    mse(2);
    check((mountedAs(1, "second5.img") && mountedAs(5, "t1c.img")),
        "drives 1 and 5 did not trade tapes (1 %s, 5 %s)", unit(1)->path, unit(5)->path);

    // A list written elsewhere and renamed over (as Tools/TapeUtils/mtp writes it).
    if( (fP = fopen(DIR "/microtapes.tmp", "w")) )
    {
        fputs("1 second5.img\n3 missing3.img,locked\n5 t1c.img\n8 t8.img\n", fP);
        fclose(fP);
    }
    rename(DIR "/microtapes.tmp", LIST);
    mse(2);
    check(mountedAs(8, "t8.img"), "a list renamed into place was not read at mse");

    // A bad line is reported once, when it appears, not at every mse.
    captureStderr(true);
    writeList("1 second5.img\n3 missing3.img,locked\n5 t1c.img\n8 t8.img\nbogus line\n");
    mse(2);
    mse(3);
    mse(2);
    captureStderr(false);
    check((stderrCount("want \"<drive 1-8>") == 1), "a bad line was reported %d times over three mse, want 1",
        stderrCount("want \"<drive 1-8>"));

    // A removed line unmounts at the next mse; so does a removed file, for every listed drive.
    // Drive 7 has no line and keeps the program's tape.
    writeList("1 second5.img\n5 t1c.img\n8 t8.img\n");
    mse(2);
    check((!unit(3)->mounted && mountedAs(1, "second5.img")), "a removed line did not unmount drive 3 at mse");
    unlink(LIST);
    mse(2);
    check((!unit(1)->mounted && !unit(5)->mounted && !unit(8)->mounted && mountedAs(7, "t7.img")),
        "with the list removed, mse left a listed tape mounted or took drive 7's");

    // The list comes back: its drives are mounted at the next mse.
    writeList("1 t1c.img\n");
    mse(1);
    check(mountedAs(1, "t1c.img"), "a list that reappeared was not read at mse");
}

// ---- main ---------------------------------------------------------------------------------

// Runs every test group. Returns 0 if every check passed, 1 otherwise.
int
main(void)
{
    if( !getcwd(cwd, sizeof(cwd)) )
    {
        printf("cannot get the current directory\n");
        return(1);
    }

    sbsSetting.nameP = "microtapesbs";
    sbsSetting.ivalue = 2;
    config.settingsP = &sbsSetting;

    cleanDir();
    if( mkdir(DIR, 0755) != 0 )
    {
        printf("cannot make %s\n", DIR);
        return(1);
    }

    runTest(testList, "microtapes.txt: paths, locked, created, bad lines, duplicates");
    runTest(testRouting, "device 01 routing: rpa, other sub-devices, the tape, read-in's rpb");
    runTest(testMountIot, "mount IOT 201: AC mask, unmount, bank end, bad names, same file, read-only");
    runTest(testUpdate, "SIGHUP: program mounts kept, changed and removed lines, retry, rethread, microtapesbs");
    runTest(testAllHalt, "All Halt: RUN falling stops every moving drive; RUN at 0 at load and RUN rising do not");
    runTest(testSelectReread, "mse rereads microtapes.txt: edits take effect, unchanged file inert, trades, one warning");

    cleanDir();
    printf("%d checks, %d failed\n", checkCount, failCount);
    return( (failCount == 0) ? 0 : 1 );
}
