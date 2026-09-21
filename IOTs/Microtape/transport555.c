/*
 * transport555.c - the implementation of the Type 555 microtape transport itself.
 *
 * 11-Sep-2026 wje/Claude - initial version
 * 21-Sep-2026 Claude - detect any tape file change when mounting, flush cache if seen
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "transport555.h"

static int64_t floorDiv(int64_t num, int64_t den);
static int64_t ceilDiv(int64_t num, int64_t den);
static void startDecel(Mt555UnitP uP, uint64_t t, int64_t ramp);
static void startAccel(Mt555UnitP uP, uint64_t t, int dir, int64_t ramp);
static void blankFrom(uint32_t *wordsP, int firstBlock);
static void noteStamp(Mt555UnitP uP, const struct stat *stP);

// Puts a unit into its power-on state: no tape mounted, stopped at the load position,
// no file. Must be called once before any other function on the unit. No return value.
void
mt555Init(Mt555UnitP uP)
{
    memset(uP, 0, sizeof(*uP));
    uP->fd = -1;
    uP->dirtyBlock = -1;
    uP->dir = 1;
    mt555Park(uP, MT_LOAD_POS);
}

// Integer division rounding toward minus infinity, for positions that can lie before
// block 0 (in the reverse end zone or leader). den must be positive.
// Returns floor(num / den).
static int64_t
floorDiv(int64_t num, int64_t den)
{
int64_t q;

    q = (num / den);
    if( ((num % den) != 0) && (num < 0) )
    {
        q = (q - 1);        // C truncates toward zero; step down for negative remainders
    }

    return(q);
}

// Integer division rounding toward plus infinity. den must be positive.
// Returns ceil(num / den).
static int64_t
ceilDiv(int64_t num, int64_t den)
{
    return( -floorDiv(-num, den) );
}

// ---- Reel and image ------------------------------------------------------------------------

// Records the size and times of the image file, from stP, as the state this unit knows: what
// the mount read, or what our own last write left. mt555FileChanged() compares against it.
// No return value.
static void
noteStamp(Mt555UnitP uP, const struct stat *stP)
{
    uP->fileSize = stP->st_size;
    uP->fileMtime = stP->st_mtim;
    uP->fileCtime = stP->st_ctim;
}

// Makes one block blank, as an unwritten tape reads: the leading checksum -0 and everything else
// 0, so the block totals -0 and checks. blockWordsP points at its MT_STORED_WORDS words.
// No return value.
void
mt555BlankBlock(uint32_t *blockWordsP)
{
    memset(blockWordsP, 0, (MT_STORED_WORDS * sizeof(uint32_t)));
    blockWordsP[0] = MT_MINUS_ZERO;
}

// Makes blocks firstBlock to the last block of the reel at wordsP blank. No return value.
static void
blankFrom(uint32_t *wordsP, int firstBlock)
{
int block;

    for( block = firstBlock; block < MT_BLOCKS; ++block )
    {
        mt555BlankBlock(&wordsP[block * MT_STORED_WORDS]);
    }
}

// Mounts the image file at pathP on the unit, replacing whatever was mounted. The file must be
// a regular file holding a whole number of blocks (MT_BLOCK_BYTES each), at most a full tape
// (MT_IMAGE_BYTES); the blocks past its end are blank (deferred formatting). An empty file is
// a blank tape. If the file does not exist, and create is true and locked is false, it is
// created empty -- a new blank tape -- and the unit's created flag is set. If locked is false
// but the file can only be opened read-only, it is mounted locked, which is what a read-only
// file means. The reel is parked at the load position, stopped.
// Returns true on success. On failure returns false, leaves the unit unmounted (and any file
// it had created removed), and puts a one-line reason in errP (at most errSize bytes, always
// terminated).
bool
mt555MountFile(Mt555UnitP uP, const char *pathP, bool locked, bool create, char *errP, int errSize)
{
int fd;
struct stat st;
uint32_t *bufP;
ssize_t got;
size_t done;
size_t bytes;
bool created;
int i;

    mt555Unmount(uP);
    errP[0] = 0;
    created = false;

    if( strlen(pathP) >= MT_PATH_MAX )
    {
        snprintf(errP, errSize, "image path too long: %s", pathP);
        return(false);
    }

    fd = open(pathP, (locked ? O_RDONLY : O_RDWR));
    if( (fd < 0) && (errno == ENOENT) && create && !locked )
    {
        // No such file: a new, blank tape. O_EXCL, so a file that appeared in the meantime is
        // never truncated here.
        fd = open(pathP, (O_RDWR | O_CREAT | O_EXCL), 0666);
        if( fd < 0 )
        {
            snprintf(errP, errSize, "%s: cannot create: %s", pathP, strerror(errno));
            return(false);
        }

        created = true;
    }
    else if( (fd < 0) && !locked && ((errno == EACCES) || (errno == EROFS) || (errno == EPERM)) )
    {
        // A file we may read but not write is a write-locked reel, not an error.
        fd = open(pathP, O_RDONLY);
        if( fd >= 0 )
        {
            locked = true;
        }
    }

    if( fd < 0 )
    {
        snprintf(errP, errSize, "%s: %s", pathP, strerror(errno));
        return(false);
    }

    if( fstat(fd, &st) != 0 )
    {
        snprintf(errP, errSize, "%s: %s", pathP, strerror(errno));
        close(fd);
        return(false);
    }

    if( !S_ISREG(st.st_mode) || ((st.st_size % MT_BLOCK_BYTES) != 0) || (st.st_size > (off_t)MT_IMAGE_BYTES) )
    {
        snprintf(errP, errSize, "%s: %s%lld bytes; a Type 550 image is a whole number of %d-byte blocks,"
            " at most %d bytes (use mkmicrotape)", pathP, (S_ISREG(st.st_mode) ? "size " : "not a regular file, "),
            (long long)st.st_size, MT_BLOCK_BYTES, MT_IMAGE_BYTES);
        close(fd);
        return(false);
    }

    if( !(bufP = (uint32_t *)malloc(MT_IMAGE_BYTES)) )
    {
        snprintf(errP, errSize, "%s: out of memory", pathP);
        close(fd);
        if( created )
        {
            unlink(pathP);
        }
        return(false);
    }

    // Read the blocks the file holds. pread() may return short counts, so loop until they
    // are all in; the rest of the reel is blank.
    bytes = (size_t)st.st_size;
    for( done = 0; done < bytes; done = (done + (size_t)got) )
    {
        got = pread(fd, ((char *)bufP) + done, (bytes - done), (off_t)done);
        if( got <= 0 )
        {
            snprintf(errP, errSize, "%s: read failed: %s", pathP, ((got < 0) ? strerror(errno) : "short file"));
            free(bufP);
            close(fd);
            return(false);
        }
    }

    // The file holds 18-bit words in 32-bit containers; ignore anything above bit 17.
    for( i = 0; i < (int)(bytes / sizeof(uint32_t)); ++i )
    {
        bufP[i] = (bufP[i] & MT_WORDMASK);
    }

    uP->fileBlocks = (int)(bytes / MT_BLOCK_BYTES);
    blankFrom(bufP, uP->fileBlocks);

    uP->wordsP = bufP;
    uP->fd = fd;
    strcpy(uP->path, pathP);
    uP->fileDev = st.st_dev;
    uP->fileIno = st.st_ino;
    noteStamp(uP, &st);
    uP->created = created;
    uP->mounted = true;
    uP->locked = locked;
    uP->offReel = false;
    uP->dirtyBlock = -1;
    uP->ioError = false;
    mt555Park(uP, MT_LOAD_POS);
    return(true);
}

// Mounts an in-memory copy of an image (MT_IMAGE_WORDS words at wordsP) with no backing file.
// Used by the host tests. The reel is parked at the load position, stopped.
// Returns true on success, false if memory could not be allocated.
bool
mt555MountMemory(Mt555UnitP uP, const uint32_t *wordsP, bool locked)
{
uint32_t *bufP;
int i;

    mt555Unmount(uP);
    if( !(bufP = (uint32_t *)malloc(MT_IMAGE_BYTES)) )
    {
        return(false);
    }

    for( i = 0; i < MT_IMAGE_WORDS; ++i )
    {
        bufP[i] = (wordsP[i] & MT_WORDMASK);
    }

    uP->wordsP = bufP;
    uP->fd = -1;
    uP->path[0] = 0;
    uP->fileDev = 0;
    uP->fileIno = 0;
    uP->fileBlocks = MT_BLOCKS;
    uP->created = false;
    uP->mounted = true;
    uP->locked = locked;
    uP->offReel = false;
    uP->dirtyBlock = -1;
    uP->ioError = false;
    mt555Park(uP, MT_LOAD_POS);
    return(true);
}

// Takes the reel off the drive: flushes any pending block, closes the file, frees the image
// and stops the unit. Safe to call on an unmounted unit. No return value.
void
mt555Unmount(Mt555UnitP uP)
{
    if( uP->mounted )
    {
        mt555Flush(uP);
    }

    if( uP->fd >= 0 )
    {
        close(uP->fd);
        uP->fd = -1;
    }

    free(uP->wordsP);
    uP->wordsP = NULL;
    uP->path[0] = 0;
    uP->fileDev = 0;
    uP->fileIno = 0;
    uP->fileSize = 0;
    memset(&uP->fileMtime, 0, sizeof(uP->fileMtime));
    memset(&uP->fileCtime, 0, sizeof(uP->fileCtime));
    uP->fileBlocks = 0;
    uP->created = false;
    uP->mounted = false;
    uP->offReel = false;
    uP->dirtyBlock = -1;
    mt555Park(uP, MT_LOAD_POS);
}

// Writes the block holding unflushed words back to the image file (258 words, one pwrite).
// A block past the end of a short file (deferred formatting) is written together with the
// blank blocks between the old end and it, so the file grows without a hole; those blocks
// are blank in memory because every block written before was flushed into the file.
// Called when the writers turn off at the end of a block, when write mode is left, and
// before any other block is written. An in-memory image has nothing to write.
// Returns true if there was nothing to do or the write succeeded; false on an I/O error,
// in which case ioError is set for the plugin to report.
bool
mt555Flush(Mt555UnitP uP)
{
int block;
int first;
ssize_t size;
ssize_t put;
struct stat st;

    if( (block = uP->dirtyBlock) < 0 )
    {
        return(true);
    }

    uP->dirtyBlock = -1;
    ++uP->flushCount;

    if( uP->fd < 0 )
    {
        return(true);           // in-memory image: the words are already where they live
    }

    first = ((block < uP->fileBlocks) ? block : uP->fileBlocks);
    size = (ssize_t)((block + 1 - first) * MT_BLOCK_BYTES);
    put = pwrite(uP->fd, &uP->wordsP[first * MT_STORED_WORDS], (size_t)size,
        ((off_t)first * (off_t)MT_BLOCK_BYTES));
    if( put != size )
    {
        uP->ioError = true;
        return(false);
    }

    if( block >= uP->fileBlocks )
    {
        uP->fileBlocks = (block + 1);
    }

    // Our own write moved the file's size and times; note them, or the next mse would take
    // the write for someone else's change and mount the tape again.
    if( fstat(uP->fd, &st) == 0 )
    {
        noteStamp(uP, &st);
    }

    return(true);
}

// Erases the whole reel to a blank tape, which is what mode 7 does here (a deviation from the
// manual, where mode 7 wrote the mark track and was "not presently connected"): every block
// in memory becomes blank, a block write in progress is dropped, and the image file is
// truncated to empty, the deferred-formatting form of a blank tape. The control refuses mode 7
// on a locked unit before calling this.
// Returns true if the reel was erased; false if it is unmounted or locked (nothing done), or
// if truncating the file failed, in which case ioError is set for the plugin to report (the
// copy in memory is blank all the same).
bool
mt555Erase(Mt555UnitP uP)
{
struct stat st;

    if( !uP->mounted || uP->locked )
    {
        return(false);
    }

    uP->dirtyBlock = -1;
    blankFrom(uP->wordsP, 0);

    if( uP->fd >= 0 )
    {
        if( ftruncate(uP->fd, 0) != 0 )
        {
            uP->ioError = true;
            return(false);
        }

        uP->fileBlocks = 0;
        if( fstat(uP->fd, &st) == 0 )
        {
            noteStamp(uP, &st);         // our own truncation, as in mt555Flush()
        }
    }

    return(true);
}

// Returns true if the unit has the file at pathP mounted: the same device and inode, however
// the path is spelled. Two drives on one image would each write back their own copy of it, so
// the plugin refuses a second mount of a file. An in-memory image matches no file.
bool
mt555SameFile(Mt555UnitP uP, const char *pathP)
{
struct stat st;

    if( !uP->mounted || (uP->fd < 0) || (stat(pathP, &st) != 0) )
    {
        return(false);
    }

    return( (st.st_dev == uP->fileDev) && (st.st_ino == uP->fileIno) );
}

// Returns true if the image file behind the mounted unit is no longer the one the mount read,
// or the one our own last write left: another file under the same name, or the same file with a
// different size, modification time or change time (the change time cannot be put back the way
// cp -p puts the modification time back), or no file at all. The image is read whole at mount,
// so a tape put under an old name is otherwise never seen. An in-memory image, an unmounted
// unit, and a path that cannot be examined for any reason but being absent are never changed.
bool
mt555FileChanged(Mt555UnitP uP)
{
struct stat st;

    if( !uP->mounted || (uP->fd < 0) )
    {
        return(false);
    }

    if( stat(uP->path, &st) != 0 )
    {
        return( (errno == ENOENT) || (errno == ENOTDIR) );
    }

    return( (st.st_dev != uP->fileDev) || (st.st_ino != uP->fileIno) || (st.st_size != uP->fileSize)
        || (st.st_mtim.tv_sec != uP->fileMtime.tv_sec) || (st.st_mtim.tv_nsec != uP->fileMtime.tv_nsec)
        || (st.st_ctim.tv_sec != uP->fileCtime.tv_sec) || (st.st_ctim.tv_nsec != uP->fileCtime.tv_nsec) );
}

// Returns stored word k (0 = leading checksum slot 3, 1-256 = data, 257 = trailing checksum
// slot 260) of the given block, or 0 if nothing is mounted or the indexes are out of range.
uint32_t
mt555GetWord(Mt555UnitP uP, int block, int k)
{
    if( !uP->mounted || (block < 0) || (block >= MT_BLOCKS) || (k < 0) || (k >= MT_STORED_WORDS) )
    {
        return(0);
    }

    return( uP->wordsP[(block * MT_STORED_WORDS) + k] );
}

// Stores word k of the given block (see mt555GetWord() for k) and marks the block dirty.
// Writing into a different block first flushes the one that was dirty, so at most one block
// is ever unflushed. A locked or unmounted reel, or bad indexes, are ignored: the control
// never writes to a locked unit, and this guard keeps a bug from reaching the file.
// No return value.
void
mt555PutWord(Mt555UnitP uP, int block, int k, uint32_t word)
{
    if( !uP->mounted || uP->locked || (block < 0) || (block >= MT_BLOCKS) || (k < 0) || (k >= MT_STORED_WORDS) )
    {
        return;
    }

    if( (uP->dirtyBlock >= 0) && (uP->dirtyBlock != block) )
    {
        mt555Flush(uP);
    }

    uP->wordsP[(block * MT_STORED_WORDS) + k] = (word & MT_WORDMASK);
    uP->dirtyBlock = block;
}

// ---- Motion --------------------------------------------------------------------------------

// Returns the tape position (speed-nanoseconds from the physical start) at time t, which
// must lie within the current motion segment (not past mt555SegmentEnd()). Times before the
// segment's reference are clamped so a stale call cannot move the tape backward.
int64_t
mt555Position(Mt555UnitP uP, uint64_t t)
{
int64_t dt;

    switch( uP->motion )
    {
    case MT_ACCEL:
        dt = (int64_t)(t - uP->refTime);
        if( dt < 0 )
        {
            dt = 0;
        }
        if( dt > uP->rampNs )
        {
            dt = uP->rampNs;
        }
        return( uP->refPos + (uP->dir * ((dt * dt) / (2 * uP->rampNs))) );

    case MT_CRUISE:
        dt = (int64_t)(t - uP->refTime);
        if( dt < 0 )
        {
            dt = 0;
        }
        return( uP->refPos + (uP->dir * dt) );

    case MT_DECEL:
        dt = (int64_t)(uP->refTime - t);      // time still to go before the tape is at rest
        if( dt < 0 )
        {
            dt = 0;
        }
        if( dt > uP->rampNs )
        {
            dt = uP->rampNs;
        }
        return( uP->refPos - (uP->dir * ((dt * dt) / (2 * uP->rampNs))) );

    case MT_STOPPED:
    default:
        return( uP->refPos );
    }
}

// Returns the time the current motion segment ends: the end of a ramp. A stopped or
// cruising tape has no scheduled end, reported as UINT64_MAX.
uint64_t
mt555SegmentEnd(Mt555UnitP uP)
{
    switch( uP->motion )
    {
    case MT_ACCEL:
        return( uP->refTime + (uint64_t)uP->rampNs );

    case MT_DECEL:
        return( uP->refTime );

    default:
        return( UINT64_MAX );
    }
}

// Moves the unit into the segment that follows the current ramp, at the time returned by
// mt555SegmentEnd(): the end of a start goes to full speed, the end of a stop comes to rest,
// or, for the first half of a turnaround, starts the ramp the other way. Must only be called
// when a ramp is in progress. No return value.
void
mt555EndSegment(Mt555UnitP uP)
{
uint64_t t;
int64_t pos;

    if( uP->motion == MT_ACCEL )
    {
        t = (uP->refTime + (uint64_t)uP->rampNs);
        pos = (uP->refPos + (uP->dir * (uP->rampNs / 2)));    // distance of a full ramp is R/2
        uP->motion = MT_CRUISE;
        uP->refTime = t;
        uP->refPos = pos;
        mt555SyncBoundary(uP, t);
    }
    else if( uP->motion == MT_DECEL )
    {
        t = uP->refTime;
        pos = uP->refPos;
        if( uP->pendingAccel )
        {
            // Second half of a turnaround: accelerate the other way from rest.
            uP->pendingAccel = false;
            uP->motion = MT_ACCEL;
            uP->dir = uP->pendingDir;
            uP->rampNs = uP->pendingRampNs;
            uP->refTime = t;
            uP->refPos = pos;
        }
        else
        {
            uP->motion = MT_STOPPED;
            uP->refTime = t;
            uP->refPos = pos;
        }
    }
}

// Begins slowing to rest at time t using a full-speed-to-rest ramp of length ramp. The
// current speed is kept: a tape part way up its start ramp takes proportionally less time
// and distance to stop. A tape already stopping keeps its existing ramp. No return value.
static void
startDecel(Mt555UnitP uP, uint64_t t, int64_t ramp)
{
int64_t pos;
int64_t rest;       // time from t until the tape is at rest

    pos = mt555Position(uP, t);

    if( uP->motion == MT_CRUISE )
    {
        rest = ramp;                            // speed 1.0 takes the full ramp
    }
    else if( uP->motion == MT_ACCEL )
    {
        // speed = (t - refTime) / rampNs; the same speed on the stop ramp is rest / ramp.
        rest = ((((int64_t)(t - uP->refTime)) * ramp) / uP->rampNs);
    }
    else
    {
        return;                                 // stopped, or already stopping
    }

    uP->motion = MT_DECEL;
    uP->refTime = (t + (uint64_t)rest);
    uP->refPos = (pos + (uP->dir * ((rest * rest) / (2 * ramp))));
    uP->rampNs = ramp;
}

// Begins accelerating in direction dir at time t using a rest-to-full-speed ramp of length
// ramp. From rest the ramp starts at t; from a stop in progress in the same direction the
// current speed is kept and the ramp is entered part way. Callers never ask for a direction
// opposite to a moving tape here (that is a turnaround, handled by mt555Command()).
// No return value.
static void
startAccel(Mt555UnitP uP, uint64_t t, int dir, int64_t ramp)
{
int64_t pos;
int64_t dt;         // time since the (virtual) moment the speed was 0 on the new ramp

    pos = mt555Position(uP, t);

    if( uP->motion == MT_STOPPED )
    {
        dt = 0;
    }
    else if( (uP->motion == MT_DECEL) && (dir == uP->dir) )
    {
        // speed = (refTime - t) / rampNs on the stop ramp; the same speed on the start ramp.
        dt = ((((int64_t)(uP->refTime - t)) * ramp) / uP->rampNs);
    }
    else
    {
        return;                                 // already accelerating or at speed
    }

    uP->motion = MT_ACCEL;
    uP->dir = dir;
    uP->rampNs = ramp;
    uP->refTime = (t - (uint64_t)dt);
    uP->refPos = (pos - (dir * ((dt * dt) / (2 * ramp))));
}

// Applies the go and reverse bits of a LOAD CONTROL to the unit at time t, which the caller
// has already brought the unit up to (every ramp ending at or before t processed).
//   go clear      stop (150 ms ramp) if moving
//   go, same dir  start from rest (200 ms), or keep going; a stop in progress re-accelerates
//   go, other dir turnaround: stop, then 150 ms start the other way (300 ms in all)
// The bits are latched in the transport, which keeps obeying them if deselected.
// No return value.
void
mt555Command(Mt555UnitP uP, uint64_t t, bool go, bool rev)
{
int dir;

    uP->goCmd = go;
    uP->revCmd = rev;
    dir = (rev ? -1 : 1);

    if( !go )
    {
        uP->pendingAccel = false;
        startDecel(uP, t, MT_STOP_NS);          // no effect if stopped or already stopping
        return;
    }

    switch( uP->motion )
    {
    case MT_STOPPED:
        startAccel(uP, t, dir, MT_START_NS);
        break;

    case MT_ACCEL:
    case MT_CRUISE:
        if( dir != uP->dir )
        {
            startDecel(uP, t, MT_STOP_NS);
            uP->pendingAccel = true;
            uP->pendingDir = dir;
            uP->pendingRampNs = MT_TURN_ACCEL_NS;
        }
        break;

    case MT_DECEL:
        if( dir == uP->dir )
        {
            // Going the way it is already moving: cancel the stop (or the turnaround).
            uP->pendingAccel = false;
            startAccel(uP, t, dir, MT_START_NS);
        }
        else
        {
            // Let the stop finish, then start the other way.
            uP->pendingAccel = true;
            uP->pendingDir = dir;
            uP->pendingRampNs = MT_TURN_ACCEL_NS;
        }
        break;
    }
}

// The end mark ahead was sensed: "the go flip-flop is reset, stopping the tape" (manual
// p. 3-9). Clears the latched go and starts the stop ramp at time t. No return value.
void
mt555EndZoneStop(Mt555UnitP uP, uint64_t t)
{
    uP->goCmd = false;
    uP->pendingAccel = false;
    startDecel(uP, t, MT_STOP_NS);
}

// Stops the unit dead at position pos with no motion commands latched. Used at mount (the
// reel is loaded at its start) and when a tape runs off its reel. No return value.
void
mt555Park(Mt555UnitP uP, int64_t pos)
{
    uP->motion = MT_STOPPED;
    uP->refPos = pos;
    uP->refTime = 0;
    uP->goCmd = false;
    uP->revCmd = false;
    uP->pendingAccel = false;
    uP->nextBoundary = 0;
}

// ---- Word boundaries -----------------------------------------------------------------------

// Recomputes, for a cruising unit, which word boundary the head crosses next after time t.
// A boundary exactly at the head's position at t counts as already crossed. Call whenever a
// unit starts cruising or the control starts watching it again (select, load control).
// No return value; a unit that is not cruising is left alone.
void
mt555SyncBoundary(Mt555UnitP uP, uint64_t t)
{
int64_t rel;

    if( uP->motion != MT_CRUISE )
    {
        return;
    }

    rel = (mt555Position(uP, t) - MT_BA);
    if( uP->dir > 0 )
    {
        uP->nextBoundary = (floorDiv(rel, MT_SLOT_NS) + 1);
    }
    else
    {
        uP->nextBoundary = (ceilDiv(rel, MT_SLOT_NS) - 1);
    }
}

// Returns the time the head of a cruising unit crosses its next word boundary.
uint64_t
mt555BoundaryTime(Mt555UnitP uP)
{
int64_t dist;

    dist = (((MT_BA + (uP->nextBoundary * MT_SLOT_NS)) - uP->refPos) * uP->dir);
    if( dist < 0 )
    {
        dist = 0;           // cannot happen once synchronized; never schedule in the past
    }

    return( uP->refTime + (uint64_t)dist );
}

// Consumes the next word boundary of a cruising unit and steps to the one after.
// Returns the absolute slot index (block * 264 + physical slot) of the word slot the head has
// just finished passing over. Moving forward that is the slot below the boundary, moving in
// reverse the slot above it. The value can lie outside 0..MT_TOTAL_SLOTS-1 in the end zones;
// the caller ignores those.
int64_t
mt555TakeBoundary(Mt555UnitP uP)
{
int64_t slot;

    if( uP->dir > 0 )
    {
        slot = (uP->nextBoundary - 1);
        uP->nextBoundary = (uP->nextBoundary + 1);
    }
    else
    {
        slot = uP->nextBoundary;
        uP->nextBoundary = (uP->nextBoundary - 1);
    }

    return(slot);
}

// Returns the time a cruising unit's head reaches position pos. If the head is already at or
// past pos in its direction of motion, returns the segment's reference time, which the
// caller treats as "due now".
uint64_t
mt555CrossTime(Mt555UnitP uP, int64_t pos)
{
int64_t dist;

    dist = ((pos - uP->refPos) * uP->dir);
    if( dist <= 0 )
    {
        return( uP->refTime );
    }

    return( uP->refTime + (uint64_t)dist );
}

// Returns the absolute slot index (see mt555TakeBoundary()) of the word slot under the head at
// time t. Outside the block area the value is out of range, which the caller checks.
int64_t
mt555SlotAt(Mt555UnitP uP, uint64_t t)
{
    return( floorDiv((mt555Position(uP, t) - MT_BA), MT_SLOT_NS) );
}

// ---- Ones'-complement arithmetic -------------------------------------------------------------

// Adds word into an 18-bit ones'-complement ring sum (end-around carry), the checksum the
// manual describes. Returns the new sum.
uint32_t
mt555RingAdd(uint32_t sum, uint32_t word)
{
uint32_t total;

    total = ((sum & MT_WORDMASK) + (word & MT_WORDMASK));
    if( total > MT_WORDMASK )
    {
        total = ((total + 1) & MT_WORDMASK);    // carry out of bit 0 comes back in at bit 17
    }

    return(total);
}

// Returns the ring sum of all MT_STORED_WORDS words of one block (both checksums and the
// data). A block that was written and checksummed correctly totals -0 (0777777).
uint32_t
mt555BlockSum(const uint32_t *blockWordsP)
{
uint32_t sum;
int i;

    sum = 0;
    for( i = 0; i < MT_STORED_WORDS; ++i )
    {
        sum = mt555RingAdd(sum, blockWordsP[i]);
    }

    return(sum);
}
