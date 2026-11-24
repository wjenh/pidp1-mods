/*
 * The IBM 1301 disk as the Stanford PDP-1 used it with the disk adapter on IOT 10, the
 * 131D data channel on IOTs 46 and 36 aliased to IOT 10 with an IBM 7631 file controller
 * and a 1301 of up to ten modules behind that.
 * See Docs/UsingIBM1301Disk.md for details.
 * IBM's 7631 OEM manual (A22-6725) is the source for the orders,
 * the status word, the format track and the timing.
 *
 * The program gives the 7631 a command with IOT 10.
 * Every character the command moves, its order and its status included, goes between core and
 * the 7631 through the 131D channel, three six-bit characters or two eight-bit ones to a
 * word, one word per high-speed channel steal on channel HSC_CHAN.
 * The 7631 answers with end, unusual end, or attention, seen in IO after any IOT 10,
 * in a cks bit, and as a sequence break.
 *
 * Everything runs on the emulator time base (iotTime(IOT_TIME_DEVICE)), which keeps running
 * through a halt, never on host time.
 * The disk spins and the accesses move and each word is transferred with the proper timing.
 *
 * Each disk module is a file laid out as ibm1301image.h defined, mapped shared.
 * A change another program makes to the file is seen immediately
 *
 * 23-Nov-2026 Claude written.
 * 23-Nov-2026 wje cleanup.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "highSpeedChannels.h"
#include "iotHandler.h"
#include "configuration.h"
#include "ibm1301image.h"

//#define DOLOGGING
#include "iotLogger.h"
#define LOG_CONFIG 0
#define LOG_FILE 0
#define LOG_IOT 0
#define LOG_CMD 0

// High speed channel 1 is the drum, 2 the DACs, 3 the Type 340, the 131D uses 4.
// A lower number has the higher priority.
#define HSC_CHAN 4

#define DEV_ADAPTER 010
#define DEV_CHANNEL 046
#define DEV_COUNTER 036

// The defaults for the configuration settings.
#define DEF_MODULES 2
#define DEF_FILE "/opt/pidp1-mods/ibm1301m%d"
#define DEF_SBS 6
#define DEF_CKS 10

// Rotation and character times, ns.
#define REV_NS 33519553ULL              // 60/1790 s
#define INDEX_NS 475000ULL              // the index area
#define TI_NS 14200ULL                  // a track identification position
#define SIX_NS 11100ULL                 // a six-bit position
#define EIGHT_NS 14200ULL               // an eight-bit position
#define STEP_NS 5000ULL                 // one main-loop pass, one memory cycle
#define FILLER_POS 11                   // positions the 7631 keeps after gap 3

// Command times, six-bit then eight-bit.
#define ORDER10_NS(six) ((six) ? 148000ULL : 190000ULL)
#define ORDER2_NS(six) ((six) ? 55000ULL : 75000ULL)
#define SENSE_NS(six) ((six) ? 123000ULL : 158000ULL)
#define REFUSE_NS 11100ULL              // a read or write refused with no data, one character

// Access motion.
#define SEEK_GROUP_NS MSTONS(50)
#define SEEK_ZONE_NS MSTONS(120)
#define SEEK_FAR_NS MSTONS(180)

// The adapter word, left in IO by every device-10 IOT.
#define AW_END 0400000
#define AW_UEND 0200000
#define AW_ATTN 0100000
#define AW_BUSY 0040000
#define AW_REFUSED 0020000
#define AW_CHAN 0010000
#define AW_SIX 0004000

// The status word's error characters 1-3 and their bits.
#define ST_PROG 1
#define ST_DATA 2
#define ST_EXC 3
#define PC_SEQ 020
#define PC_CODE 004
#define PC_FMT 002
#define PC_NOREC 001
#define DC_ADDR 020
#define DC_RESP 004
#define DC_CMP 002
#define EX_INOP 020
#define EX_NRDY 004
#define EX_DSCC 002

// The order codes, as decimal numbers.
#define OP_DNOP 0
#define OP_DREL 4
#define OP_DEBM 8
#define OP_DSBM 9
#define OP_DSEK 80
#define OP_DVSR 82
#define OP_DWRF 83
#define OP_DVTN 84
#define OP_DVCY 85
#define OP_DWRC 86
#define OP_DSAI 87
#define OP_DVTA 88
#define OP_DVHA 89

// A character the channel never delivered, an invalid digit in every position.
#define NOCHAR 077

// What a command is doing; PH_IDLE when none is busy.
#define PH_IDLE 0
#define PH_ORDER 1                      // a control command's order words moving
#define PH_SENSE 2                      // a sense's status words moving
#define PH_HOME 3                       // waiting for a home address to compare
#define PH_DATA 4                       // a read, write or write check moving words
#define PH_FORMAT 5                     // a format write moving words
#define PH_ENDING 6                     // waiting for the command's end

// A transfer's direction.
#define DIR_READ 0                      // characters to core
#define DIR_WRITE 1                     // characters from core
#define DIR_COMPARE 2                   // from core, compared and not stored

// What stepWords() found.
#define XS_WAIT 0                       // nothing more until eventAt
#define XS_MOVED 1                      // a word finished moving
#define XS_DONE 2                       // every word has moved
#define XS_STOPPED 3                    // the channel stopped first, its count run out or by SDF
#define XS_ABORT 4                      // the channel did not answer, at a halt

// One module, with its access, its image and its place in the rotation.
typedef struct
{
    bool installed;
    bool inoperative;                   // DSAI, until the emulator next starts
    bool failed;                        // its image could not be opened or mapped
    bool attention;                     // its attention bit in the status word
    int fd;
    unsigned char *imageP;
    char path[PATH_MAX];
    int cylinder;                       // where the access is, or is going
    int head;
    bool moving;
    uint64_t restAt;
    uint64_t phase;                     // its index passes at phase + k * REV_NS
} Module;

// A prepare-to-verify or DWRF order, kept for the read or write after it.
typedef struct
{
    bool valid;
    int code;
    int module;
    int track;
    unsigned char addr[8];              // the order's characters 2-9
    bool writeCheck;                    // a DWRC came after the write, so compare and do not write
    bool sixBit;                        // the mode of the write a check repeats
} Prepare;

// One area a transfer moves, a run of characters at a fixed rate.
// Character k is in the area's position k + 1, so it comes off the disk when that position
// ends and is written when it starts.
// An area of n characters takes n + 4 positions, and its check characters are done two
// positions after that.
typedef struct
{
    uint64_t start;                     // when the area's position 0 starts
    uint64_t posNs;
    int n;
    unsigned char *charsP;
    bool isRecord;                      // a record area, not a record address or HA2
} Span;

// The characters of a command, packed into words as one stream across its areas.
typedef struct
{
    Span spans[(2 * IBM1301_MAXRECORDS) + 2];
    int nSpans;
    int total;                          // characters in all the spans
    int cpw;                            // characters to a word, 3 or 2
    int dir;
    bool dueFirst;                      // a word is due when its first character is, not its last
    int nextWord;                       // the next word to move
    bool inFlight;                      // nextWord is on the high-speed channel
    bool mismatch;                      // a write check found a difference
} Transfer;

// The 131D channel.
typedef struct
{
    HSCChannelP chanP;
    HSCRequest request;
    Word buf;
    int bank;
    int addr;
    int count;                          // words left; 0 once it ran out
    bool moving;                        // a command is using it and it has not stopped
} Channel;

// The adapter, the 7631, the channel and the drive.
typedef struct
{
    PDP1 *pdp1P;

    bool end;                           // the adapter's three signals, until DEN
    bool uend;
    bool attn;

    bool sixBit;                        // DSBM or DEBM
    unsigned char err[4];               // status characters 1-3; 0 is made from them
    Prepare pv;                         // for the next read or write
    Prepare lastWrite;                  // the write a DWRC can check

    int phase;
    uint64_t eventAt;                   // when the command next needs attention
    uint64_t startAt;                   // when its IOT was given
    uint64_t endAt;
    bool unusual;                       // the end at endAt is an unusual end
    int pendingMode;                    // -1, or the mode DSBM (1) or DEBM (0) sets at its end
    int opCode;                         // a control command's order, once it has arrived
    Prepare savedWrite;                 // lastWrite, as a control command found it
    uint64_t indexAt;                   // the index a track operation starts from
    int fmtCylinder;                    // the cylinder a format write is for
    Prepare active;                     // the order a read or write is using
    unsigned char order[10];
    unsigned char sense[12];
    unsigned char format[IBM1301_SLOTSIZE];
    Transfer xfer;
    Channel chan;

    Module modules[IBM1301_MAXMODULES];
    int nModules;
    char filePattern[PATH_MAX];
    bool keyLock;                       // the format switches are at WRITE
    int sbsChan;
    int cksMask;
    bool started;
    bool reloadPending;                 // modules or files changed while a command was busy
} FileControl;

static FileControl fc;

static uint64_t now(void);
static void raiseSignal(FileControl *fcP, bool *signalP);
static void updateCks(FileControl *fcP);
static Word adapterWord(FileControl *fcP);
static bool startCommand(FileControl *fcP);
static void finishAt(FileControl *fcP, uint64_t at, bool unusual);
static void fail(FileControl *fcP, int stChar, int bit, uint64_t at);
static void endCommand(FileControl *fcP);
static void resetControl(FileControl *fcP);
static void beginSense(FileControl *fcP);
static void beginControl(FileControl *fcP);
static void decodeCode(FileControl *fcP);
static void executeOrder(FileControl *fcP);
static int digitOf(unsigned char c);
static void beginData(FileControl *fcP, bool write);
static void searchRecord(FileControl *fcP, Module *mP, const Ibm1301Map *mapP);
static void compareHome(FileControl *fcP);
static void formatStopped(FileControl *fcP);
static void dataStopped(FileControl *fcP, bool abort);
static void xferInit(Transfer *xP, int cpw, int dir, bool dueFirst);
static void xferAdd(Transfer *xP, uint64_t start, uint64_t posNs, int n, unsigned char *charsP, bool isRecord);
static int xferWords(const Transfer *xP);
static int spanOf(const Transfer *xP, int s, int *kP);
static int recordSpan(const Transfer *xP, int i);
static uint64_t charTime(const Transfer *xP, int s, bool first);
static uint64_t wordDue(const Transfer *xP, int w);
static uint64_t spanDone(const Transfer *xP, int i);
static int stepWords(FileControl *fcP, uint64_t t);
static void zeroFill(Transfer *xP, int s, int lastSpan);
static void serviceCommand(FileControl *fcP, uint64_t t);
static void serviceAccesses(FileControl *fcP, uint64_t t);
static void rearm(FileControl *fcP);
static uint64_t nextIndex(const Module *mP, uint64_t t);
static uint64_t areaStart(const Ibm1301Map *mapP, const Ibm1301Area *aP, uint64_t index);
static uint64_t posNsOf(const Ibm1301Map *mapP, const Ibm1301Area *aP);
static uint64_t seekNs(int from, int to);
static bool mapOf(Module *mP, int cyl, Ibm1301Map *mapP);
static unsigned char *trackOf(Module *mP, int cyl, int head);
static void configure(bool atStart);
static void openModules(FileControl *fcP);
static void closeModules(FileControl *fcP);
static bool openModule(Module *mP, int module);
static void syncModules(FileControl *fcP);
static void atExit(void) __attribute__((destructor));

// One edge of each IOT on devices 10, 46 and 36.
// None waits and none skips, each completing at once, and each device-10 IOT leaves the
// adapter word in IO.
int
iotHandler(PDP1 *pdp1P, int dev, int pulse, int completion)
{
FileControl *fcP;
int sub;
bool refused;
Word before;

    if( pulse )
    {
        return( 1 );
    }

    fcP = &fc;
    fcP->pdp1P = pdp1P;
    if( completion )
    {
        IOCOMPLETE(pdp1P);
    }

    // The C bit is bit 6 of the instruction, inside the subcode's field.
    sub = (MB(pdp1P) >> 6) & 037;
    iotCondLog(LOG_IOT, "ibm1301: iot %02o subcode %02o io %06o\n", dev, sub, IO(pdp1P));
    serviceAccesses(fcP, now());
    serviceCommand(fcP, now());

    switch( dev )
    {
    case DEV_ADAPTER:
        refused = false;
        switch( sub )
        {
        case 000:                       // DRS
            resetControl(fcP);
            break;

        case 001:                       // DEN, the program seeing what it answered
            before = adapterWord(fcP);
            fcP->end = false;
            fcP->uend = false;
            fcP->attn = false;
            updateCks(fcP);
            rearm(fcP);
            IO(pdp1P) = before;
            return( 1 );

        case 004:                       // DSN
            if( !(refused = !startCommand(fcP)) )
            {
                beginSense(fcP);
            }
            break;

        case 005:                       // DRD
        case 006:                       // DWR
            if( !(refused = !startCommand(fcP)) )
            {
                beginData(fcP, (sub == 006));
            }
            break;

        case 007:                       // DCT
            if( !(refused = !startCommand(fcP)) )
            {
                beginControl(fcP);
            }
            break;

        default:
            return( 1 );
        }

        serviceCommand(fcP, now());
        rearm(fcP);
        IO(pdp1P) = adapterWord(fcP) | (refused ? AW_REFUSED : 0);
        break;

    case DEV_CHANNEL:
        if( (sub == 013) && !fcP->chan.moving )                 // SID
        {
            fcP->chan.bank = (IO(pdp1P) >> 12) & 017;
            fcP->chan.addr = IO(pdp1P) & 07777;
        }
        else if( (sub == 005) && !fcP->chan.moving )            // SWD
        {
            fcP->chan.count = IO(pdp1P) & 07777;
            if( fcP->chan.count == 0 )
            {
                fcP->chan.count = 4096;
            }
        }
        else if( sub == 001 )                                   // SDF
        {
            // The command sees the stop at once if it is moving words, else when it would.
            if( fcP->chan.moving )
            {
                fcP->chan.moving = false;
                if( (fcP->phase == PH_ORDER) || (fcP->phase == PH_SENSE) || (fcP->phase == PH_DATA) ||
                    (fcP->phase == PH_FORMAT) )
                {
                    fcP->eventAt = now();
                }
                serviceCommand(fcP, now());
                rearm(fcP);
            }
        }
        break;

    case DEV_COUNTER:
        if( sub == 013 )                                        // RLD
        {
            IO(pdp1P) = (fcP->chan.bank << 12) | fcP->chan.addr;
        }
        break;

    default:
        return( 0 );
    }

    return( 1 );
}

// Called at each start; the first sets up the channel and opens the modules.
void
iotStart(void)
{
    if( !fc.started )
    {
        fc.started = true;
        fc.sixBit = true;
        fc.pendingMode = -1;
        fc.chan.count = 0;
        configure(true);
    }

    if( !fc.chan.chanP )
    {
        fc.chan.chanP = HSCallocateChannel(HSC_CHAN);
        iotCondLog(LOG_CONFIG, "ibm1301: high-speed channel %d %s\n", HSC_CHAN,
            (fc.chan.chanP ? "allocated" : "not available"));
    }

    openModules(&fc);
}

// At a STOP, the images are flushed to their files.
void
iotStop(void)
{
    syncModules(&fc);
    iotCloseLog();
}

// A config reload, on the emulator thread.
void
iotUpdate(void)
{
    configure(false);
}

// The power switch went off, and a power clear is a DRS.
// The drive is separate equipment and keeps its accesses where they are.
// One that was moving is put at rest at its target, its attention bit set in the status word
// but not raised as a signal.
// Every deadline is already canceled.
void
iotPowerClear(void)
{
int m;

    resetControl(&fc);
    fc.xfer.inFlight = false;
    for( m = 0; m < fc.nModules; ++m )
    {
        if( fc.modules[m].moving )
        {
            fc.modules[m].moving = false;
            fc.modules[m].attention = true;
        }
    }
}

// The deadline came, so the accesses and the command catch up to now.
void
iotDeadline(PDP1 *pdp1P)
{
    fc.pdp1P = pdp1P;
    serviceAccesses(&fc, now());
    serviceCommand(&fc, now());
    rearm(&fc);
}

// Returns the device time base, ns.
static uint64_t
now(void)
{
    return( iotTime(IOT_TIME_DEVICE) );
}

// Raises end, unusual end or attention, requesting a break on the configured channel each
// time one rises.
static void
raiseSignal(FileControl *fcP, bool *signalP)
{
    if( !*signalP )
    {
        *signalP = true;
        initiateBreak(fcP->sbsChan);
    }

    updateCks(fcP);
}

// Sets the cks bit while any of the three signals is up.
static void
updateCks(FileControl *fcP)
{
    if( !fcP->pdp1P )
    {
        return;
    }

    if( fcP->end || fcP->uend || fcP->attn )
    {
        CKS(fcP->pdp1P) |= fcP->cksMask;
    }
    else
    {
        CKS(fcP->pdp1P) &= ~fcP->cksMask;
    }
}

// Returns the adapter word, without the refused bit.
static Word
adapterWord(FileControl *fcP)
{
Word w;

    w = 0;
    w |= (fcP->end) ? AW_END : 0;
    w |= (fcP->uend) ? AW_UEND : 0;
    w |= (fcP->attn) ? AW_ATTN : 0;
    w |= (fcP->phase != PH_IDLE) ? AW_BUSY : 0;
    w |= (fcP->chan.moving) ? AW_CHAN : 0;
    w |= (fcP->sixBit) ? AW_SIX : 0;
    return( w );
}

// A command can start only when none is busy and the last one's end is answered.
// An unanswered attention does not hold it off.
// Returns true if it may start, with the channel set to move words for it.
static bool
startCommand(FileControl *fcP)
{
    if( (fcP->phase != PH_IDLE) || fcP->end || fcP->uend )
    {
        return( false );
    }

    fcP->startAt = now();
    fcP->unusual = false;
    fcP->pendingMode = -1;
    fcP->chan.moving = (fcP->chan.count > 0);
    return( true );
}

// The command ends at a given time, normally or not.
static void
finishAt(FileControl *fcP, uint64_t at, bool unusual)
{
    fcP->phase = PH_ENDING;
    fcP->endAt = at;
    fcP->eventAt = at;
    fcP->unusual = unusual;
}

// The command fails with a status bit, ending at a given time; the channel stops.
static void
fail(FileControl *fcP, int stChar, int bit, uint64_t at)
{
    fcP->err[stChar] |= (unsigned char)bit;
    fcP->chan.moving = false;
    finishAt(fcP, at, true);
}

// The command is over, so end or unusual end rises, and a reload held for it is done.
static void
endCommand(FileControl *fcP)
{
    fcP->phase = PH_IDLE;
    fcP->chan.moving = false;
    if( fcP->pendingMode >= 0 )
    {
        fcP->sixBit = (fcP->pendingMode == 1);
        fcP->pendingMode = -1;
    }

    iotCondLog(LOG_CMD, "ibm1301: %s at %llu\n", (fcP->unusual ? "unusual end" : "end"),
        (unsigned long long)now());
    raiseSignal(fcP, (fcP->unusual ? &fcP->uend : &fcP->end));
    if( fcP->reloadPending )
    {
        fcP->reloadPending = false;
        configure(false);
    }
}

// DRS, and the power clear.
// Any command ends at once with no end signal, the channel stops, the signals drop, and the
// error bits and any prepare-to-verify are cleared.
// The mode, the accesses, their attention bits and the channel's address and count are kept.
// A write cut short keeps what moved and has the rest of its record zeroed, as a halt leaves
// it.
static void
resetControl(FileControl *fcP)
{
int s;

    if( (fcP->phase == PH_DATA) && (fcP->xfer.dir == DIR_WRITE) )
    {
        s = fcP->xfer.nextWord * fcP->xfer.cpw;
        zeroFill(&fcP->xfer, s, recordSpan(&fcP->xfer, spanOf(&fcP->xfer, s, NULL)));
    }

    fcP->phase = PH_IDLE;
    fcP->chan.moving = false;
    fcP->end = false;
    fcP->uend = false;
    fcP->attn = false;
    memset(fcP->err, 0, sizeof(fcP->err));
    fcP->pv.valid = false;
    fcP->lastWrite.valid = false;
    fcP->pendingMode = -1;
    updateCks(fcP);
    rearm(fcP);
}

// DSN sends the ten status characters, as they are now, to core over the sense's time.
// A sense clears nothing, so a second one reads the same word.
static void
beginSense(FileControl *fcP)
{
unsigned char *cP;
uint64_t pos;
int m;

    cP = fcP->sense;
    memset(cP, 0, sizeof(fcP->sense));
    cP[1] = fcP->err[ST_PROG];
    cP[2] = fcP->err[ST_DATA];
    cP[3] = fcP->err[ST_EXC];
    cP[0] = (unsigned char)(((cP[1] || (cP[2] & DC_ADDR)) ? 004 : 0) |
        ((cP[2] & (DC_RESP | DC_CMP | 001)) ? 002 : 0) | (cP[3] ? 001 : 0));
    cP[4] = fcP->sixBit ? 004 : 0;
    for( m = 0; m < fcP->nModules; ++m )
    {
        if( fcP->modules[m].attention )
        {
            cP[5 + (m / 4)] |= (unsigned char)(020 >> ((m % 4) + (((m % 4) > 0) ? 1 : 0)));
        }
    }

    // Spread so the last word moves as the sense ends, character k done at (k + 1) tenths.
    pos = SENSE_NS(fcP->sixBit) / 10;
    xferInit(&fcP->xfer, (fcP->sixBit ? 3 : 2), DIR_READ, false);
    xferAdd(&fcP->xfer, (fcP->startAt - pos), pos, 10, cP, false);
    fcP->phase = PH_SENSE;
    fcP->endAt = fcP->startAt + SENSE_NS(fcP->sixBit);
    fcP->eventAt = fcP->startAt;
}

// DCT takes the order's characters from core, the operation code first.
// What the order is decides how long it takes and how many words it uses (decodeCode()).
// Any control order cancels a prepare-to-verify and the write a DWRC would check, and clears
// the error bits as it starts.
static void
beginControl(FileControl *fcP)
{
uint64_t pos;

    memset(fcP->err, 0, sizeof(fcP->err));
    fcP->savedWrite = fcP->lastWrite;
    fcP->pv.valid = false;
    fcP->lastWrite.valid = false;
    fcP->opCode = -1;
    memset(fcP->order, NOCHAR, sizeof(fcP->order));

    pos = ORDER10_NS(fcP->sixBit) / 10;
    xferInit(&fcP->xfer, (fcP->sixBit ? 3 : 2), DIR_WRITE, false);
    xferAdd(&fcP->xfer, (fcP->startAt - pos), pos, 10, fcP->order, false);
    fcP->phase = PH_ORDER;
    fcP->endAt = fcP->startAt + ORDER10_NS(fcP->sixBit);
    fcP->eventAt = fcP->startAt;
}

// Returns a character's digit, the 7631 using only its digit portion, or -1 for
// 12-17, which is no digit.
static int
digitOf(unsigned char c)
{
    return( ((c & 017) <= 9) ? (c & 017) : -1 );
}

// The first word of an order has come, so its operation code is known.
// An invalid code, and the two-character orders, end at the two-character time with the one
// word; the rest wait for their other characters.
static void
decodeCode(FileControl *fcP)
{
int d0;
int d1;
int code;
bool bad;

    d0 = digitOf(fcP->order[0]);
    d1 = digitOf(fcP->order[1]);
    code = ((d0 < 0) || (d1 < 0)) ? -1 : ((d0 * 10) + d1);
    bad = (code != OP_DNOP) && (code != OP_DREL) && (code != OP_DEBM) && (code != OP_DSBM) &&
        (code != OP_DSEK) && (code != OP_DVSR) && (code != OP_DWRF) && (code != OP_DVTN) &&
        (code != OP_DWRC) && (code != OP_DSAI) && (code != OP_DVTA);

    // DVCY and DVHA are left out, as on a 7631 without the cylinder feature and with its HAO
    // switch off; DWRF with the key-lock switch off is refused the same way.
    if( (code == OP_DWRF) && !fcP->keyLock )
    {
        bad = true;
    }

    fcP->opCode = code;
    if( bad )
    {
        iotCondLog(LOG_CMD, "ibm1301: invalid code %02o %02o\n", fcP->order[0], fcP->order[1]);
        fail(fcP, ST_PROG, PC_CODE, (fcP->startAt + ORDER2_NS(fcP->sixBit)));
        return;
    }

    if( (code == OP_DNOP) || (code == OP_DREL) || (code == OP_DEBM) || (code == OP_DSBM) )
    {
        if( code == OP_DEBM )
        {
            fcP->pendingMode = 0;
        }
        else if( code == OP_DSBM )
        {
            fcP->pendingMode = 1;
        }

        fcP->chan.moving = false;
        finishAt(fcP, (fcP->startAt + ORDER2_NS(fcP->sixBit)), false);
    }
}

// Every character of a ten-character order is in, one the channel never sent being NOCHAR.
// The order's checks are made in the 7631's order, then the order acts.
static void
executeOrder(FileControl *fcP)
{
Module *mP;
Prepare *pP;
int digits[8];
int access;
int module;
int track;
int code;
int i;

    code = fcP->opCode;
    for( i = 0; i < 8; ++i )
    {
        digits[i] = digitOf(fcP->order[2 + i]);
    }

    for( i = 0; i < 6; ++i )
    {
        if( digits[i] < 0 )
        {
            fail(fcP, ST_DATA, DC_ADDR, fcP->endAt);
            return;
        }
    }

    access = digits[0];
    module = digits[1];
    track = (digits[2] * 1000) + (digits[3] * 100) + (digits[4] * 10) + digits[5];

    // A module that is not there, or not installed, answers as an inoperative access.
    // A 1301 module has one access, 0.
    if( (access != 0) || (module >= fcP->nModules) || fcP->modules[module].inoperative )
    {
        fail(fcP, ST_EXC, EX_INOP, fcP->endAt);
        return;
    }

    mP = &fcP->modules[module];
    mP->attention = false;

    // DSAI acts whether or not the access is ready.
    if( code == OP_DSAI )
    {
        mP->inoperative = true;
        finishAt(fcP, fcP->endAt, false);
        return;
    }

    if( mP->moving )
    {
        fail(fcP, ST_EXC, EX_NRDY, fcP->endAt);
        return;
    }

    switch( code )
    {
    case OP_DSEK:
        // Head selection is electronic; the access moves after the order ends and raises
        // attention when it comes to rest, at once if it is already on the cylinder.
        mP->head = track % IBM1301_HEADS;
        mP->restAt = fcP->endAt + seekNs(mP->cylinder, (track / IBM1301_HEADS));
        mP->cylinder = track / IBM1301_HEADS;
        mP->moving = true;
        break;

    case OP_DWRC:
        // It checks the write just made, needing the prepare-to-verify's address and a write
        // since.
        pP = &fcP->savedWrite;
        if( !pP->valid || (memcmp(pP->addr, &fcP->order[2], 8) != 0) )
        {
            fail(fcP, ST_PROG, PC_SEQ, fcP->endAt);
            return;
        }

        fcP->pv = *pP;
        fcP->pv.writeCheck = true;
        break;

    default:                            // DVSR, DVTA, DVTN, DWRF
        if( (code == OP_DVTA) || (code == OP_DVTN) )
        {
            mP->head = track % IBM1301_HEADS;
        }

        pP = &fcP->pv;
        pP->valid = true;
        pP->code = code;
        pP->module = module;
        pP->track = track;
        memcpy(pP->addr, &fcP->order[2], 8);
        pP->writeCheck = false;
        pP->sixBit = fcP->sixBit;
        break;
    }

    finishAt(fcP, fcP->endAt, false);
}

// DRD or DWR uses the prepare-to-verify before it, or for a write the DWRF.
// Without one it ends at once with invalid sequence and no data.
static void
beginData(FileControl *fcP, bool write)
{
Prepare p;
Module *mP;
Ibm1301Map map;
uint64_t at;
bool six;

    p = fcP->pv;
    fcP->pv.valid = false;
    memset(fcP->err, 0, sizeof(fcP->err));
    at = fcP->startAt + REFUSE_NS;

    if( !p.valid || (!write && (p.writeCheck || (p.code == OP_DWRF))) )
    {
        fcP->lastWrite.valid = false;
        fail(fcP, ST_PROG, PC_SEQ, at);
        return;
    }

    mP = &fcP->modules[p.module];
    mP->attention = false;
    fcP->active = p;

    // The next DWRC checks this write; anything else, a read included, ends that.
    fcP->lastWrite.valid = false;
    if( write && !p.writeCheck )
    {
        fcP->lastWrite = p;
        fcP->lastWrite.sixBit = fcP->sixBit;
    }

    if( mP->inoperative )
    {
        fail(fcP, ST_EXC, EX_INOP, at);
        return;
    }

    if( mP->failed || !mP->imageP )
    {
        fail(fcP, ST_EXC, EX_DSCC, at);
        return;
    }

    six = (p.writeCheck) ? p.sixBit : fcP->sixBit;
    xferInit(&fcP->xfer, (six ? 3 : 2), (write ? (p.writeCheck ? DIR_COMPARE : DIR_WRITE) : DIR_READ), write);
    fcP->indexAt = nextIndex(mP, fcP->startAt);

    // For DWRF the address is verified at the next home address, in the track identification
    // area, and the format is written from the index after it.
    if( p.code == OP_DWRF )
    {
        fcP->phase = PH_HOME;
        fcP->eventAt = fcP->indexAt + INDEX_NS + (IBM1301_TIPOS * TI_NS);
        return;
    }

    // A cylinder's format sets its mode; a read or write in the other mode is refused.
    mapOf(mP, mP->cylinder, &map);
    if( map.formatted && (map.eightBit == six) )
    {
        fail(fcP, ST_PROG, PC_FMT, at);
        return;
    }

    if( p.code == OP_DVSR )
    {
        searchRecord(fcP, mP, &map);
        return;
    }

    // DVTA and DVTN compare the home address after the next index, at the end of HA2's area.
    fcP->phase = PH_HOME;
    if( map.formatted )
    {
        fcP->eventAt = areaStart(&map, &map.ha2, fcP->indexAt) + ((uint64_t)(map.ha2.n + 4) * posNsOf(&map, &map.ha2));
    }
    else
    {
        fcP->eventAt = fcP->indexAt + INDEX_NS + (IBM1301_TIPOS * TI_NS);
    }
}

// DVSR starts the search wherever the track is.
// Each record address that passes whole is compared, the first four characters by their digit
// portion, the fifth and sixth in full, and at the first match the record after it moves.
// No match by the second passing of the index ends with no record found.
static void
searchRecord(FileControl *fcP, Module *mP, const Ibm1301Map *mapP)
{
unsigned char *trackP;
const unsigned char *raP;
uint64_t second;
uint64_t index;
uint64_t ra;
uint64_t done;
int64_t first;
int rev;
int r;
int i;
bool match;

    second = nextIndex(mP, fcP->indexAt);
    trackP = trackOf(mP, mP->cylinder, mP->head);
    first = (int64_t)fcP->indexAt - (int64_t)REV_NS;
    for( rev = 0; mapP->formatted && (rev < 3); ++rev )
    {
        if( (first + ((int64_t)rev * (int64_t)REV_NS)) < 0 )
        {
            continue;
        }

        index = (uint64_t)(first + ((int64_t)rev * (int64_t)REV_NS));
        for( r = 0; r < mapP->count; ++r )
        {
            ra = areaStart(mapP, &mapP->ra[r], index);
            done = ra + ((uint64_t)(mapP->ra[r].n + 4) * posNsOf(mapP, &mapP->ra[r]));
            if( ra < fcP->startAt )
            {
                continue;
            }

            if( done >= second )
            {
                rev = 3;
                break;
            }

            raP = trackP + mapP->ra[r].offset;
            match = true;
            for( i = 0; i < 4; ++i )
            {
                match = match && ((raP[i] & 017) == (fcP->active.addr[2 + i] & 017));
            }

            match = match && (raP[4] == fcP->active.addr[6]) && (raP[5] == fcP->active.addr[7]);
            if( match )
            {
                xferAdd(&fcP->xfer, areaStart(mapP, &mapP->rec[r], index), posNsOf(mapP, &mapP->rec[r]),
                    mapP->rec[r].n, (trackP + mapP->rec[r].offset), true);
                fcP->phase = PH_DATA;
                fcP->eventAt = fcP->startAt;
                return;
            }
        }
    }

    fail(fcP, ST_PROG, PC_NOREC, second);
}

// The home address has passed.
// DVTA and DVTN compare the track with HA1 and their two identifier characters with HA2's
// first two; DWRF compares the cylinder only.
// A match starts the transfer.
static void
compareHome(FileControl *fcP)
{
Module *mP;
Prepare *pP;
Ibm1301Map map;
unsigned char *trackP;
uint64_t fmtStart;
uint64_t pos;
int r;

    pP = &fcP->active;
    mP = &fcP->modules[pP->module];
    if( (pP->track / IBM1301_HEADS) != mP->cylinder )
    {
        fail(fcP, ST_PROG, PC_NOREC, fcP->eventAt);
        return;
    }

    if( pP->code == OP_DWRF )
    {
        // The format record's characters, the track identification area's at its rate and the
        // rest at the channel's, from the next index until the stop.
        // A write check keeps the record too, to compare it whole at the stop.
        fcP->indexAt += REV_NS;
        fcP->fmtCylinder = mP->cylinder;
        fcP->xfer.dir = DIR_WRITE;
        memset(fcP->format, 0, sizeof(fcP->format));
        pos = (fcP->xfer.cpw == 3) ? SIX_NS : EIGHT_NS;
        fmtStart = fcP->indexAt + INDEX_NS;
        xferAdd(&fcP->xfer, (fmtStart - TI_NS), TI_NS, IBM1301_TIPOS, fcP->format, false);
        xferAdd(&fcP->xfer, (fmtStart + (IBM1301_TIPOS * TI_NS) - pos), pos,
            (IBM1301_FORMATMAX - IBM1301_TIPOS), (fcP->format + IBM1301_TIPOS), false);
        fcP->phase = PH_FORMAT;
        return;
    }

    mapOf(mP, mP->cylinder, &map);
    trackP = trackOf(mP, mP->cylinder, mP->head);
    if( !map.formatted || (trackP[0] != pP->addr[6]) || (trackP[1] != pP->addr[7]) )
    {
        fail(fcP, ST_PROG, PC_NOREC, fcP->eventAt);
        return;
    }

    for( r = 0; r < map.count; ++r )
    {
        if( pP->code == OP_DVTA )
        {
            xferAdd(&fcP->xfer, areaStart(&map, &map.ra[r], fcP->indexAt), posNsOf(&map, &map.ra[r]),
                map.ra[r].n, (trackP + map.ra[r].offset), false);
        }

        xferAdd(&fcP->xfer, areaStart(&map, &map.rec[r], fcP->indexAt), posNsOf(&map, &map.rec[r]),
            map.rec[r].n, (trackP + map.rec[r].offset), true);
    }

    fcP->phase = PH_DATA;
}

// The channel stopped a format write, and the record is what moved, checked as the 7631 reads
// it.
// A good one is written, or compared in a write check, and the 7631 writes filler to the next
// index and ends three positions after it.
// A bad one is refused at once.
static void
formatStopped(FileControl *fcP)
{
Ibm1301Map map;
Module *mP;
unsigned char *slotP;
uint64_t pos;
uint64_t t;
int len;
int code;

    mP = &fcP->modules[fcP->active.module];
    t = now();
    pos = (fcP->xfer.cpw == 3) ? SIX_NS : EIGHT_NS;
    len = fcP->xfer.nextWord * fcP->xfer.cpw;
    while( (len > 0) && (fcP->format[len - 1] == 0) )
    {
        --len;
    }

    if( (code = ibm1301ParseFormat(fcP->format, len, &map)) != IBM1301_FMT_OK )
    {
        iotCondLog(LOG_CMD, "ibm1301: format record refused, %d\n", code);
        fail(fcP, ST_PROG, PC_FMT, (t + pos));
        return;
    }

    slotP = mP->imageP + ibm1301SlotOffset(fcP->fmtCylinder, 0);
    if( fcP->active.writeCheck )
    {
        if( (ibm1301FormatLength(slotP) != len) || (memcmp(slotP + 2, fcP->format, (size_t)len) != 0) )
        {
            fail(fcP, ST_DATA, DC_CMP, (fcP->indexAt + REV_NS + (3 * pos)));
            return;
        }
    }
    else
    {
        memcpy(slotP + 2, fcP->format, (size_t)len);
        ibm1301SetFormatLength(slotP, len);
    }

    fcP->chan.moving = false;
    finishAt(fcP, (fcP->indexAt + REV_NS + (3 * pos)), false);
}

// A read or write the channel stopped (its count ran out, or SDF), or that a halt cut short.
// At a stop the 7631 goes on to the end of the record it is in, a read without sending and a
// write filling it with 0.
// A halt gives response check at the end of the area it was in, a write keeping what moved and
// zeroing the rest of its record.
static void
dataStopped(FileControl *fcP, bool abort)
{
Transfer *xP;
int s;
int i;
int rec;

    xP = &fcP->xfer;
    s = xP->nextWord * xP->cpw;
    i = spanOf(xP, s, NULL);
    rec = recordSpan(xP, i);

    if( xP->dir == DIR_WRITE )
    {
        zeroFill(xP, s, rec);
    }

    fcP->chan.moving = false;
    if( abort )
    {
        fail(fcP, ST_DATA, DC_RESP, spanDone(xP, i));
        return;
    }

    if( xP->mismatch )
    {
        fail(fcP, ST_DATA, DC_CMP, spanDone(xP, rec));
        return;
    }

    finishAt(fcP, spanDone(xP, rec), false);
}

// Starts a transfer of no spans yet.
static void
xferInit(Transfer *xP, int cpw, int dir, bool dueFirst)
{
    xP->nSpans = 0;
    xP->total = 0;
    xP->cpw = cpw;
    xP->dir = dir;
    xP->dueFirst = dueFirst;
    xP->nextWord = 0;
    xP->inFlight = false;
    xP->mismatch = false;
}

// Adds an area to the transfer's stream.
static void
xferAdd(Transfer *xP, uint64_t start, uint64_t posNs, int n, unsigned char *charsP, bool isRecord)
{
Span *sP;

    sP = &xP->spans[xP->nSpans++];
    sP->start = start;
    sP->posNs = posNs;
    sP->n = n;
    sP->charsP = charsP;
    sP->isRecord = isRecord;
    xP->total += n;
}

// Returns the words the stream fills, its last one perhaps in part.
static int
xferWords(const Transfer *xP)
{
    return( (xP->total + xP->cpw - 1) / xP->cpw );
}

// Returns the span stream character s is in, its index there in *kP; past the end, the last.
static int
spanOf(const Transfer *xP, int s, int *kP)
{
int i;

    for( i = 0; i < (xP->nSpans - 1); ++i )
    {
        if( s < xP->spans[i].n )
        {
            break;
        }

        s -= xP->spans[i].n;
    }

    if( kP )
    {
        *kP = (s < xP->spans[i].n) ? s : (xP->spans[i].n - 1);
    }

    return( i );
}

// Returns the record span i belongs to, itself or, for a record address, the record after it.
static int
recordSpan(const Transfer *xP, int i)
{
    if( !xP->spans[i].isRecord && ((i + 1) < xP->nSpans) )
    {
        return( i + 1 );
    }

    return( i );
}

// Returns when character s is written (first, when its position starts) or has come off the
// disk (its position ends).
static uint64_t
charTime(const Transfer *xP, int s, bool first)
{
const Span *sP;
int k;

    sP = &xP->spans[spanOf(xP, s, &k)];
    return( sP->start + ((uint64_t)(k + (first ? 1 : 2)) * sP->posNs) );
}

// Returns when word w is due, before its first character is written on a write, after its
// last has come off the disk on a read.
static uint64_t
wordDue(const Transfer *xP, int w)
{
int s;

    if( xP->dueFirst )
    {
        return( charTime(xP, (w * xP->cpw), true) );
    }

    s = (w * xP->cpw) + xP->cpw - 1;
    return( charTime(xP, ((s < xP->total) ? s : (xP->total - 1)), false) );
}

// Returns when span i's check characters are done, two positions after the area.
static uint64_t
spanDone(const Transfer *xP, int i)
{
    return( xP->spans[i].start + ((uint64_t)(xP->spans[i].n + 6) * xP->spans[i].posNs) );
}

// Moves the transfer's words as they come due, one high-speed channel steal each.
// A word due while the machine is halted, or one a STOP, start or continue aborted, has no
// answer from the channel.
// Returns an XS_ value; on XS_WAIT, eventAt is when to call again.
static int
stepWords(FileControl *fcP, uint64_t t)
{
Transfer *xP;
Channel *cP;
Span *sP;
uint64_t due;
int status;
int s;
int j;
int k;
unsigned char c;

    xP = &fcP->xfer;
    cP = &fcP->chan;
    if( xP->inFlight )
    {
        if( HSCgetStatus(cP->chanP) == HSC_BUSY )
        {
            fcP->eventAt = t + STEP_NS;
            return( XS_WAIT );
        }

        xP->inFlight = false;
        if( HSCwait(cP->chanP) == HSC_ABORT )
        {
            return( XS_ABORT );
        }

        // A word from core has its characters, those the stream has, written or compared.
        if( xP->dir != DIR_READ )
        {
            for( j = 0; j < xP->cpw; ++j )
            {
                s = (xP->nextWord * xP->cpw) + j;
                if( s >= xP->total )
                {
                    break;
                }

                c = (xP->cpw == 3) ? (unsigned char)((cP->buf >> (12 - (6 * j))) & 077) :
                    (unsigned char)((cP->buf >> (9 - (9 * j))) & 0377);
                sP = &xP->spans[spanOf(xP, s, &k)];
                if( xP->dir == DIR_WRITE )
                {
                    sP->charsP[k] = c;
                }
                else if( sP->charsP[k] != c )
                {
                    xP->mismatch = true;
                }
            }
        }

        ++(xP->nextWord);
        cP->addr = (cP->addr + 1) & 07777;
        if( --(cP->count) <= 0 )
        {
            cP->count = 0;
            cP->moving = false;
        }

        return( XS_MOVED );
    }

    if( xP->nextWord >= xferWords(xP) )
    {
        return( XS_DONE );
    }

    if( !cP->moving )
    {
        return( XS_STOPPED );
    }

    due = wordDue(xP, xP->nextWord);
    if( t < due )
    {
        fcP->eventAt = due;
        return( XS_WAIT );
    }

    if( !cP->chanP || !fcP->pdp1P || !fcP->pdp1P->run )
    {
        return( XS_ABORT );
    }

    // A word DRS left on the channel finishes first.
    if( HSCgetStatus(cP->chanP) == HSC_BUSY )
    {
        fcP->eventAt = t + STEP_NS;
        return( XS_WAIT );
    }

    // A word to core holds its characters, 0 past the stream's end.
    // Six-bit fills bits 0-5, 6-11 and 12-17; eight-bit right-justifies a byte in each nine-bit
    // half.
    cP->buf = 0;
    if( xP->dir == DIR_READ )
    {
        for( j = 0; j < xP->cpw; ++j )
        {
            s = (xP->nextWord * xP->cpw) + j;
            c = 0;
            if( s < xP->total )
            {
                sP = &xP->spans[spanOf(xP, s, &k)];
                c = sP->charsP[k];
            }

            cP->buf |= (xP->cpw == 3) ? ((Word)(c & 077) << (12 - (6 * j))) : ((Word)(c & 0377) << (9 - (9 * j)));
        }
    }

    cP->request.mode = HSC_MODE_TRUESTEAL | HSC_MODE_UPDATEPANEL | ((xP->dir == DIR_READ) ? HSC_MODE_TOMEM : HSC_MODE_FROMMEM);
    cP->request.count = 1;
    cP->request.memBank = cP->bank;
    cP->request.memAddr = cP->addr;
    cP->request.wordTime = 50;
    cP->request.toBufferP = &cP->buf;
    cP->request.fromBufferP = &cP->buf;
    status = HSCexecute(cP->chanP, &cP->request);
    if( status != HSC_BUSY )
    {
        return( XS_ABORT );
    }

    xP->inFlight = true;
    fcP->eventAt = t + STEP_NS;
    return( XS_WAIT );
}

// Zeroes the stream from character s to the end of span lastSpan, the rest of a record a
// write did not finish.
static void
zeroFill(Transfer *xP, int s, int lastSpan)
{
int i;
int k;

    if( (xP->nSpans == 0) || (s >= xP->total) )
    {
        return;
    }

    i = spanOf(xP, s, &k);
    for( ; i <= lastSpan; ++i )
    {
        memset(xP->spans[i].charsP + k, 0, (size_t)(xP->spans[i].n - k));
        k = 0;
    }
}

// Runs the command up to time t.
static void
serviceCommand(FileControl *fcP, uint64_t t)
{
uint64_t limit;
int r;

    while( (fcP->phase != PH_IDLE) && (t >= fcP->eventAt) )
    {
        switch( fcP->phase )
        {
        case PH_ORDER:
            r = stepWords(fcP, t);
            if( (r == XS_MOVED) && (fcP->opCode < 0) )
            {
                decodeCode(fcP);
            }
            else if( (r == XS_DONE) || (r == XS_STOPPED) )
            {
                if( fcP->opCode < 0 )
                {
                    decodeCode(fcP);
                }

                if( fcP->phase == PH_ORDER )
                {
                    fcP->phase = PH_ENDING;
                    fcP->eventAt = fcP->endAt;
                    executeOrder(fcP);
                }
            }
            else if( r == XS_ABORT )
            {
                fail(fcP, ST_DATA, DC_RESP, fcP->endAt);
            }
            break;

        case PH_SENSE:
            r = stepWords(fcP, t);
            if( (r == XS_DONE) || (r == XS_STOPPED) )
            {
                fcP->chan.moving = false;
                finishAt(fcP, fcP->endAt, false);
            }
            else if( r == XS_ABORT )
            {
                fail(fcP, ST_DATA, DC_RESP, fcP->endAt);
            }
            break;

        case PH_HOME:
            compareHome(fcP);
            break;

        case PH_DATA:
            r = stepWords(fcP, t);
            if( r == XS_DONE )
            {
                dataStopped(fcP, false);
            }
            else if( (r == XS_STOPPED) || (r == XS_ABORT) )
            {
                dataStopped(fcP, (r == XS_ABORT));
            }
            break;

        case PH_FORMAT:
            // The stop has to come in time for the 7631's filler before the next index.
            limit = fcP->indexAt + REV_NS - (FILLER_POS * fcP->xfer.spans[1].posNs);
            if( !fcP->xfer.inFlight && fcP->chan.moving && (wordDue(&fcP->xfer, fcP->xfer.nextWord) >= limit) )
            {
                fail(fcP, ST_PROG, PC_FMT, limit);
                break;
            }

            r = stepWords(fcP, t);
            if( (r == XS_STOPPED) || (r == XS_DONE) )
            {
                formatStopped(fcP);
            }
            else if( r == XS_ABORT )
            {
                fail(fcP, ST_DATA, DC_RESP, (t + fcP->xfer.spans[1].posNs));
            }
            break;

        case PH_ENDING:
            endCommand(fcP);
            break;

        default:
            fcP->phase = PH_IDLE;
            break;
        }
    }
}

// Accesses that have come to rest raise attention.
static void
serviceAccesses(FileControl *fcP, uint64_t t)
{
int m;

    for( m = 0; m < fcP->nModules; ++m )
    {
        if( fcP->modules[m].moving && (t >= fcP->modules[m].restAt) )
        {
            fcP->modules[m].moving = false;
            fcP->modules[m].attention = true;
            raiseSignal(fcP, &fcP->attn);
        }
    }
}

// Sets the one deadline at the next event, the command's or an access coming to rest.
static void
rearm(FileControl *fcP)
{
uint64_t next;
int m;

    next = UINT64_MAX;
    if( fcP->phase != PH_IDLE )
    {
        next = fcP->eventAt;
    }

    for( m = 0; m < fcP->nModules; ++m )
    {
        if( fcP->modules[m].moving && (fcP->modules[m].restAt < next) )
        {
            next = fcP->modules[m].restAt;
        }
    }

    if( next == UINT64_MAX )
    {
        iotPollCancel();
    }
    else
    {
        iotPollAt(IOT_TIME_DEVICE, next);
    }
}

// Returns the module's first index after time t.
static uint64_t
nextIndex(const Module *mP, uint64_t t)
{
    if( t < mP->phase )
    {
        return( mP->phase );
    }

    return( mP->phase + ((((t - mP->phase) / REV_NS) + 1) * REV_NS) );
}

// Returns when an area's position 0 starts, in the revolution from an index, after the index area,
// then the track identification area's positions, then the cylinder's at its mode's rate.
static uint64_t
areaStart(const Ibm1301Map *mapP, const Ibm1301Area *aP, uint64_t index)
{
    return( index + INDEX_NS + (IBM1301_TIPOS * TI_NS) +
        ((uint64_t)(aP->pos - IBM1301_TIPOS) * (mapP->eightBit ? EIGHT_NS : SIX_NS)) );
}

// Returns the position time of an area of the cylinder.
static uint64_t
posNsOf(const Ibm1301Map *mapP, const Ibm1301Area *aP)
{
    (void)aP;
    return( mapP->eightBit ? EIGHT_NS : SIX_NS );
}

// Returns the time an access takes between two cylinders.
// A zone is 50 cylinders, and its groups are its cylinders 0-9, 10-19, 20-24, 25-34, 35-44,
// 45-49.
static uint64_t
seekNs(int from, int to)
{
static const int groupOf[50] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5 };

    if( from == to )
    {
        return( 0 );
    }

    if( (from / 50) != (to / 50) )
    {
        return( SEEK_FAR_NS );
    }

    if( groupOf[from % 50] != groupOf[to % 50] )
    {
        return( SEEK_ZONE_NS );
    }

    return( SEEK_GROUP_NS );
}

// Reads a cylinder's format into a map; a cylinder never formatted, or one whose slot is
// damaged, has a map with formatted false.
// Returns true if the cylinder is formatted.
static bool
mapOf(Module *mP, int cyl, Ibm1301Map *mapP)
{
unsigned char *slotP;
int len;

    memset(mapP, 0, sizeof(*mapP));
    if( !mP->imageP )
    {
        return( false );
    }

    slotP = mP->imageP + ibm1301SlotOffset(cyl, 0);
    len = ibm1301FormatLength(slotP);
    if( (len == 0) || (len > IBM1301_FORMATMAX) || (ibm1301ParseFormat(slotP + 2, len, mapP) != IBM1301_FMT_OK) )
    {
        memset(mapP, 0, sizeof(*mapP));
        return( false );
    }

    return( true );
}

// Returns a track's slot in the module's image.
static unsigned char *
trackOf(Module *mP, int cyl, int head)
{
    return( mP->imageP + ibm1301SlotOffset(cyl, (head + 1)) );
}

// Reads the ibm1301 settings, at the first start and at each reload.
// The modules and their files change only when no command is busy, else at its end.
static void
configure(bool atStart)
{
ConfigurationSettingP settingP;
ConfigurationP configP;
char pattern[PATH_MAX];
const char *pctP;
int modules;
int cks;

    configP = getConfiguration();

    modules = DEF_MODULES;
    if( (settingP = findConfigurationSetting(configP, "ibm1301modules")) && !settingP->strvalueP )
    {
        if( (settingP->ivalue >= 1) && (settingP->ivalue <= IBM1301_MAXMODULES) )
        {
            modules = settingP->ivalue;
        }
        else
        {
            iotCondLog(LOG_CONFIG, "ibm1301modules must be 1 to 10, %d used\n", DEF_MODULES);
        }
    }

    // The file name has one %d, for the module number, and no other %.
    snprintf(pattern, sizeof(pattern), "%s", DEF_FILE);
    if( (settingP = findConfigurationSetting(configP, "ibm1301file")) && settingP->strvalueP )
    {
        pctP = strchr(settingP->strvalueP, '%');
        if( pctP && (pctP[1] == 'd') && !strchr(pctP + 2, '%') &&
            (strlen(settingP->strvalueP) < (sizeof(pattern) - 8)) )
        {
            snprintf(pattern, sizeof(pattern), "%s", settingP->strvalueP);
        }
        else
        {
            iotCondLog(LOG_CONFIG, "ibm1301file needs one %%d for the module number, %s used\n", DEF_FILE);
        }
    }

    fc.keyLock = false;
    if( (settingP = findConfigurationSetting(configP, "ibm1301format")) )
    {
        fc.keyLock = settingP->onOff;
    }

    fc.sbsChan = DEF_SBS;
    if( (settingP = findConfigurationSetting(configP, "ibm1301sbs")) && !settingP->strvalueP &&
        (settingP->ivalue >= 0) && (settingP->ivalue <= 15) )
    {
        fc.sbsChan = settingP->ivalue;
    }

    cks = DEF_CKS;
    if( (settingP = findConfigurationSetting(configP, "ibm1301cks")) && !settingP->strvalueP &&
        !settingP->onOff && (settingP->ivalue >= 0) && (settingP->ivalue <= 17) )
    {
        cks = settingP->ivalue;
    }

    // A changed cks bit moves the signal with it.
    if( fc.pdp1P && fc.cksMask )
    {
        CKS(fc.pdp1P) &= ~fc.cksMask;
    }
    fc.cksMask = 1 << (17 - cks);
    updateCks(&fc);

    if( atStart || ((modules != fc.nModules) || (strcmp(pattern, fc.filePattern) != 0)) )
    {
        if( !atStart && (fc.phase != PH_IDLE) )
        {
            fc.reloadPending = true;
            return;
        }

        closeModules(&fc);
        fc.nModules = modules;
        snprintf(fc.filePattern, sizeof(fc.filePattern), "%s", pattern);
        if( !atStart )
        {
            openModules(&fc);
        }
    }
}

// Opens each installed module's image that is not open, creating it as a fresh one if it
// does not exist.
// Each module's index phase is fixed and differs from the others', so two modules do not turn
// in step.
static void
openModules(FileControl *fcP)
{
Module *mP;
int m;

    for( m = 0; m < fcP->nModules; ++m )
    {
        mP = &fcP->modules[m];
        mP->phase = (REV_NS * (uint64_t)((7 * m) + 3)) / 17;
        mP->phase %= REV_NS;
        if( !mP->installed )
        {
            mP->installed = true;
            mP->fd = -1;
            mP->failed = !openModule(mP, m);
        }
    }
}

// Opens and maps one module's image, creating it if it does not exist.
// Returns true on success; false leaves the module with no image, after logging why.
static bool
openModule(Module *mP, int module)
{
unsigned char *p;
struct stat st;
const char *whyP;
int fd;

    snprintf(mP->path, sizeof(mP->path), fc.filePattern, module);
    if( (access(mP->path, F_OK) != 0) && !ibm1301CreateImage(mP->path, module, true, false) &&
        (errno != EEXIST) )
    {
        iotCondLog(LOG_FILE, "ibm1301: cannot create %s: %s\n", mP->path, strerror(errno));
        return( false );
    }

    if( (fd = open(mP->path, O_RDWR)) < 0 )
    {
        iotCondLog(LOG_FILE, "ibm1301: cannot open %s: %s\n", mP->path, strerror(errno));
        return( false );
    }

    if( (fstat(fd, &st) != 0) || (st.st_size != IBM1301_FILESIZE) )
    {
        iotCondLog(LOG_FILE, "ibm1301: %s is not the size of an IBM 1301 image\n", mP->path);
        close(fd);
        return( false );
    }

    p = mmap(NULL, (size_t)IBM1301_FILESIZE, (PROT_READ | PROT_WRITE), MAP_SHARED, fd, 0);
    if( p == MAP_FAILED )
    {
        iotCondLog(LOG_FILE, "ibm1301: cannot map %s: %s\n", mP->path, strerror(errno));
        close(fd);
        return( false );
    }

    if( (whyP = ibm1301CheckHeader(p, module)) != NULL )
    {
        iotCondLog(LOG_FILE, "ibm1301: %s is %s\n", mP->path, whyP);
        munmap(p, (size_t)IBM1301_FILESIZE);
        close(fd);
        return( false );
    }

    mP->fd = fd;
    mP->imageP = p;
    return( true );
}

// Flushes and closes every module's image; the accesses keep their places.
static void
closeModules(FileControl *fcP)
{
Module *mP;
int m;

    for( m = 0; m < IBM1301_MAXMODULES; ++m )
    {
        mP = &fcP->modules[m];
        if( mP->imageP )
        {
            msync(mP->imageP, (size_t)IBM1301_FILESIZE, MS_SYNC);
            munmap(mP->imageP, (size_t)IBM1301_FILESIZE);
            mP->imageP = NULL;
        }

        if( mP->installed && (mP->fd >= 0) )
        {
            close(mP->fd);
        }

        mP->fd = -1;
        mP->installed = false;
        mP->failed = false;
    }
}

// Writes every mapped image's changes to its file.
static void
syncModules(FileControl *fcP)
{
int m;

    for( m = 0; m < IBM1301_MAXMODULES; ++m )
    {
        if( fcP->modules[m].imageP )
        {
            msync(fcP->modules[m].imageP, (size_t)IBM1301_FILESIZE, MS_SYNC);
        }
    }
}

// At the emulator's exit, which calls no iotStop().
static void
atExit(void)
{
    syncModules(&fc);
}
