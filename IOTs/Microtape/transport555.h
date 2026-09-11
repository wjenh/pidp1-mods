/*
 * transport555.h - defines for the transport implementaton.
 */
#ifndef TRANSPORT555_H
#define TRANSPORT555_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#define MT_UNITS            8               // up to 4 dual transports = 8 drives, numbered 1-8
#define MT_BLOCKS           576             // blocks 0-1077 octal, as the Format Pack writes them
#define MT_SLOTS_PER_BLOCK  264             // 4 control + 256 data + 4 control word slots
#define MT_DATA_WORDS       256             // data words per block
#define MT_STORED_WORDS     258             // leading checksum + 256 data + trailing checksum
#define MT_IMAGE_WORDS      (MT_BLOCKS * MT_STORED_WORDS)
#define MT_IMAGE_BYTES      (MT_IMAGE_WORDS * 4)
#define MT_BLOCK_BYTES      (MT_STORED_WORDS * 4)   // an image file is a whole number of these
#define MT_TOTAL_SLOTS      ((int64_t)MT_BLOCKS * MT_SLOTS_PER_BLOCK)

#define MT_SLOT_NS          200000LL        // one word = 6 lines of 33.3 us at speed
#define MT_BLOCK_NS         (MT_SLOTS_PER_BLOCK * MT_SLOT_NS)       // 52.8 ms per block
#define MT_BLOCKAREA_NS     (MT_TOTAL_SLOTS * MT_SLOT_NS)           // 202.7 ft
#define MT_TAPE_NS          ((1170000LL * 100000LL) / 3LL)          // 1,170,000 lines = 260 ft
#define MT_ENDZONE_NS       ((45000LL * 100000LL) / 3LL)            // 45,000 lines = 10 ft
#define MT_LEADER_NS        ((MT_TAPE_NS - MT_BLOCKAREA_NS - (2 * MT_ENDZONE_NS)) / 2)
#define MT_BA               (MT_LEADER_NS + MT_ENDZONE_NS)          // position of block 0 slot 0
#define MT_FEZ              (MT_BA + MT_BLOCKAREA_NS)               // start of forward end zone
#define MT_LOAD_POS         MT_LEADER_NS    // a freshly mounted reel: start of the reverse end zone

#define MT_START_NS         200000000LL     // 200 ms linear ramp from rest to speed
#define MT_STOP_NS          150000000LL     // 150 ms linear ramp from speed to rest
#define MT_TURN_ACCEL_NS    150000000LL     // turnaround = 150 ms stop + 150 ms start = 300 ms

// Physical slot numbers used by both layers (forward order; reverse mirrors s to 263 - s).
#define MT_SLOT_BLOCKMARK   1
#define MT_SLOT_LOCK        2
#define MT_SLOT_REVCHECK    3
#define MT_SLOT_FIRSTDATA   4
#define MT_SLOT_PREFINAL    258
#define MT_SLOT_FINAL       259
#define MT_SLOT_CHECK       260

#define MT_WORDMASK         0777777
#define MT_MINUS_ZERO       0777777         // ones'-complement -0, the automatic leading checksum

#define MT_PATH_MAX         256

// Motion states. A moving segment is described analytically from a reference time and
// position, so the position at any later time is computed on demand ("lazy"), as simh's
// dt_setpos() does; see mt555Position() for the formulas.
typedef enum
{
    MT_STOPPED = 0,         // not moving; refPos is the position
    MT_ACCEL,               // linear ramp up; refTime is the virtual time speed was 0
    MT_CRUISE,              // full speed; refTime/refPos is any point on the segment
    MT_DECEL                // linear ramp down; refTime is the time speed reaches 0
} MtMotion;

typedef struct
{
    // The reel and its image.
    bool mounted;           // an image is loaded; false = no tape on the drive
    bool locked;            // WRITE LOCK switch position: any mode except write
    bool offReel;           // ran off the end while deselected; unusable until remounted
    int fd;                 // image file, -1 for an in-memory image (host tests)
    char path[MT_PATH_MAX]; // image path, "" for an in-memory image
    dev_t fileDev;          // the image file's device and inode, for mt555SameFile()
    ino_t fileIno;
    int fileBlocks;         // blocks 0 .. fileBlocks-1 are in the file; the rest are blank
    bool created;           // the last mount created the file (the plugin reports it)
    uint32_t *wordsP;       // MT_IMAGE_WORDS words when mounted, each 18 bits
    int dirtyBlock;         // block with writes not yet flushed to the file, -1 if none
    unsigned long flushCount;   // blocks flushed so far (host tests check this)
    bool ioError;           // a flush or erase failed; reported on stderr by the plugin

    // Motion commands as the transport latched them. A deselected drive keeps obeying the
    // last ones it was given (manual p. 1-11), which is why they live here and not in the
    // control.
    bool goCmd;
    bool revCmd;

    // Motion.
    MtMotion motion;
    int dir;                // +1 forward, -1 reverse: direction of the current (or last) motion
    uint64_t refTime;       // see MtMotion
    int64_t refPos;         // see MtMotion
    int64_t rampNs;         // ACCEL/DECEL: time the full ramp 0 <-> full speed takes
    bool pendingAccel;      // this DECEL is the first half of a turnaround
    int pendingDir;         // direction to accelerate in when it ends
    int64_t pendingRampNs;  // ramp to use when it does

    // CRUISE only: the next word boundary the head will cross, as an index k meaning the
    // boundary at position MT_BA + k * MT_SLOT_NS. Kept current by mt555SyncBoundary().
    int64_t nextBoundary;
} Mt555Unit, *Mt555UnitP;

void mt555Init(Mt555UnitP uP);

// Reel and image.
bool mt555MountFile(Mt555UnitP uP, const char *pathP, bool locked, bool create, char *errP, int errSize);
bool mt555MountMemory(Mt555UnitP uP, const uint32_t *wordsP, bool locked);
void mt555Unmount(Mt555UnitP uP);
bool mt555Flush(Mt555UnitP uP);
bool mt555Erase(Mt555UnitP uP);
bool mt555SameFile(Mt555UnitP uP, const char *pathP);
void mt555BlankBlock(uint32_t *blockWordsP);
uint32_t mt555GetWord(Mt555UnitP uP, int block, int k);
void mt555PutWord(Mt555UnitP uP, int block, int k, uint32_t word);

// Motion.
int64_t mt555Position(Mt555UnitP uP, uint64_t t);
uint64_t mt555SegmentEnd(Mt555UnitP uP);
void mt555EndSegment(Mt555UnitP uP);
void mt555Command(Mt555UnitP uP, uint64_t t, bool go, bool rev);
void mt555EndZoneStop(Mt555UnitP uP, uint64_t t);
void mt555Park(Mt555UnitP uP, int64_t pos);

// Word boundaries while cruising.
void mt555SyncBoundary(Mt555UnitP uP, uint64_t t);
uint64_t mt555BoundaryTime(Mt555UnitP uP);
int64_t mt555TakeBoundary(Mt555UnitP uP);
uint64_t mt555CrossTime(Mt555UnitP uP, int64_t pos);
int64_t mt555SlotAt(Mt555UnitP uP, uint64_t t);

// Ones'-complement helpers shared with the tool and the tests.
uint32_t mt555RingAdd(uint32_t sum, uint32_t word);
uint32_t mt555BlockSum(const uint32_t *blockWordsP);

#endif
