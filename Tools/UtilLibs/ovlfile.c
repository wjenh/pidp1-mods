/*
 * ovlfile.c -- an am1 overlay file read and checked, its overlays compared with a drum,
 * and the drum's other tenants recognized; see ovlfile.h.
 *
 * file.ovl is text: a label line, a version line, a stamp line giving the build stamp
 * and the full address of the program's overlay table, then for each overlay a line
 * naming its window, bank, first and last address, drum track, drum address and span,
 * length and mode, followed by its words, the two header words (the stamp and the
 * overlay's number) included.
 * Every number is octal.
 * A V2 file's overlay line has drum and span. A V1 file's has neither: am1 put each
 * overlay at the drum addresses of its window's core addresses, its span the window.
 *
 * 13-Oct-2026 wje/Claude moved out of ovlload.c
 * 21-Oct-2026 Claude the drum writer moved out of ovlload.c, for ad1 and fastload too
 * 21-Oct-2026 wje cleanup
 * 22-Oct-2026 Claude V2, the packed drum layout: each overlay's drum address and span
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "ovlfile.h"

#define OVLFILELABEL "%%am1 overlay file%%"
#define OVLFILEV1 "V1"
#define OVLFILEV2 "V2"

static bool ovlFault(char *errP, const char *pathP, int lineNo, const char *msgP, const char *argP);

// Read the .ovl at pathP into *ofP and check all of it: the label and version, the
// stamp line, each overlay's fields, its words and header, and no two overlays' spans
// overlapping on one track.
// Returns true, or false with *ofP empty and the first
// fault in errP (OVL_ERRSIZE bytes), as "path line n: what", with no final period.
bool
ovlRead(const char *pathP, OvlFile *ofP, char *errP)
{
FILE *fP;
char *lineP;
char *cP;
char *endP;
char mode[8];
size_t len;
int lineNo;
int consumed;
int have;
int i;
int j;
int fields;
bool v2;
long word;
Overlay *ovP;
Overlay *otherP;
Overlay *grownP;

    memset(ofP, 0, sizeof(OvlFile));
    if( !(fP = fopen(pathP, "r")) )
    {
        snprintf(errP, OVL_ERRSIZE, "can't open '%s': %s", pathP, strerror(errno));
        return(false);
    }

    lineP = NULL;
    len = 0;
    lineNo = 0;
    ovP = NULL;
    have = 0;
    v2 = false;

    while( getline(&lineP, &len, fP) != -1 )
    {
        ++lineNo;
        lineP[strcspn(lineP, "\r\n")] = 0;

        if( lineNo == 1 )
        {
            if( strcmp(lineP, OVLFILELABEL) != 0 )
            {
                ovlFault(errP, pathP, lineNo, "this is not an am1 overlay file", NULL);
                goto failed;
            }

            continue;
        }

        if( lineNo == 2 )
        {
            if( strcmp(lineP, OVLFILEV2) == 0 )
            {
                v2 = true;
            }
            else if( strcmp(lineP, OVLFILEV1) != 0 )
            {
                ovlFault(errP, pathP, lineNo, "version '%s' is not one this tool knows", lineP);
                goto failed;
            }

            continue;
        }

        if( lineNo == 3 )
        {
            consumed = -1;
            if( !(cP = strstr(lineP, " stamp ")) ||
                (sscanf(cP, " stamp %o table %o%n", &ofP->stamp, &ofP->tableAddr, &consumed) != 2) ||
                (cP[consumed] != 0) )
            {
                ovlFault(errP, pathP, lineNo, "the stamp line is not 'source stamp nnnnnn table nnnnnn'", NULL);
                goto failed;
            }

            if( (ofP->stamp <= 0) || (ofP->stamp > OVL_WORDMASK) || (ofP->tableAddr < 0) ||
                (ofP->tableAddr > 0177777) )
            {
                ovlFault(errP, pathP, lineNo, "the stamp or the table's address is out of range", NULL);
                goto failed;
            }

            continue;
        }

        if( strncmp(lineP, "overlay ", 8) == 0 )
        {
            if( ovP && (have != ovP->length) )
            {
                ovlFault(errP, pathP, lineNo, "overlay %s has fewer words than its length", ovP->name);
                goto failed;
            }

            if( !(grownP = realloc(ofP->overlaysP, (ofP->count + 1) * sizeof(Overlay))) )
            {
                ovlFault(errP, pathP, lineNo, "out of memory", NULL);
                goto failed;
            }

            ofP->overlaysP = grownP;
            ovP = &ofP->overlaysP[ofP->count++];
            memset(ovP, 0, sizeof(Overlay));
            ovP->lineNo = lineNo;
            consumed = -1;
            if( v2 )
            {
                fields = sscanf(lineP,
                    "overlay %63s window %63s bank %o first %o last %o track %o drum %o span %o length %o mode %7s%n",
                    ovP->name, ovP->window, &ovP->bank, &ovP->first, &ovP->last, &ovP->track, &ovP->drum,
                    &ovP->span, &ovP->length, mode, &consumed) - 2;
            }
            else
            {
                fields = sscanf(lineP, "overlay %63s window %63s bank %o first %o last %o track %o length %o mode %7s%n",
                    ovP->name, ovP->window, &ovP->bank, &ovP->first, &ovP->last, &ovP->track,
                    &ovP->length, mode, &consumed);
                ovP->drum = ovP->first;
                ovP->span = (ovP->last - ovP->first) + 1;
            }

            if( (fields != 8) || (lineP[consumed] != 0) )
            {
                ovlFault(errP, pathP, lineNo, "the overlay line is not in the form am1 writes", NULL);
                goto failed;
            }

            if( (ovP->bank < 0) || (ovP->bank > 017) || (ovP->first < 0) || (ovP->last > 07777) ||
                (ovP->first > ovP->last) || (ovP->track < 0) || (ovP->track >= OVL_TRACKS) )
            {
                ovlFault(errP, pathP, lineNo, "overlay %s's bank, window or track is out of range", ovP->name);
                goto failed;
            }

            if( (ovP->length <= OVL_HEADERWORDS) || (ovP->length > ((ovP->last - ovP->first) + 1)) )
            {
                ovlFault(errP, pathP, lineNo, "overlay %s's length does not fit its window", ovP->name);
                goto failed;
            }

            // The span is written from the window's first core address by the PDP-1's own
            // writer, so it fits the window as well as the track.
            if( (ovP->span < ovP->length) || (ovP->span > ((ovP->last - ovP->first) + 1)) ||
                (ovP->drum < 0) || ((ovP->drum + ovP->span) > OVL_TRACKWORDS) )
            {
                ovlFault(errP, pathP, lineNo, "overlay %s's drum address or span is out of range", ovP->name);
                goto failed;
            }

            if( strcmp(mode, "ro") == 0 )
            {
                ovP->persistent = false;
            }
            else if( strcmp(mode, "rw") == 0 )
            {
                ovP->persistent = true;
            }
            else
            {
                ovlFault(errP, pathP, lineNo, "overlay %s's mode is neither ro nor rw", ovP->name);
                goto failed;
            }

            if( !(ovP->wordsP = calloc(ovP->length, sizeof(int32_t))) )
            {
                ovlFault(errP, pathP, lineNo, "out of memory", NULL);
                goto failed;
            }

            have = 0;
            continue;
        }

        if( !ovP )
        {
            ovlFault(errP, pathP, lineNo, "words come before any overlay line", NULL);
            goto failed;
        }

        for( cP = lineP; *cP; cP = endP )
        {
            while( *cP == ' ' )
            {
                ++cP;
            }

            if( !*cP )
            {
                break;
            }

            word = strtol(cP, &endP, 8);
            if( (endP == cP) || ((*endP != ' ') && (*endP != 0)) || (word < 0) || (word > OVL_WORDMASK) )
            {
                ovlFault(errP, pathP, lineNo, "a word of overlay %s is not an 18-bit octal number", ovP->name);
                goto failed;
            }

            if( have >= ovP->length )
            {
                ovlFault(errP, pathP, lineNo, "overlay %s has more words than its length", ovP->name);
                goto failed;
            }

            ovP->wordsP[have++] = (int32_t)word;
        }
    }

    free(lineP);
    lineP = NULL;
    fclose(fP);
    fP = NULL;

    if( lineNo < 3 )
    {
        ovlFault(errP, pathP, lineNo, "the file ends before its stamp line", NULL);
        goto failed;
    }

    if( ofP->count == 0 )
    {
        ovlFault(errP, pathP, lineNo, "the file holds no overlay", NULL);
        goto failed;
    }

    if( have != ovP->length )
    {
        ovlFault(errP, pathP, lineNo, "overlay %s has fewer words than its length", ovP->name);
        goto failed;
    }

    // The header is what the overlay manager checks after a load; a file whose headers
    // are wrong would load overlays that always fail that check.
    for( i = 0; i < ofP->count; ++i )
    {
        ovP = &ofP->overlaysP[i];
        if( (ovP->wordsP[0] != ofP->stamp) || (ovP->wordsP[1] != (i + 1)) )
        {
            ovlFault(errP, pathP, ovP->lineNo, "overlay %s's header is not the stamp and its number", ovP->name);
            goto failed;
        }

        for( j = 0; j < i; ++j )
        {
            otherP = &ofP->overlaysP[j];
            if( (otherP->track == ovP->track) && (otherP->drum < (ovP->drum + ovP->span)) &&
                (ovP->drum < (otherP->drum + otherP->span)) )
            {
                ovlFault(errP, pathP, ovP->lineNo, "overlay %s overlaps another overlay on its track", ovP->name);
                goto failed;
            }
        }
    }

    return(true);

failed:
    free(lineP);
    if( fP )
    {
        fclose(fP);
    }

    ovlFree(ofP);
    return(false);
}

// Free what ovlRead() allocated, leaving *ofP empty.
void
ovlFree(OvlFile *ofP)
{
int i;

    for( i = 0; i < ofP->count; ++i )
    {
        free(ofP->overlaysP[i].wordsP);
    }

    free(ofP->overlaysP);
    memset(ofP, 0, sizeof(OvlFile));
}

// Put a fault in the .ovl at a line into errP, argP filling a %s in msgP.
// Returns false, for the caller's convenience.
static bool
ovlFault(char *errP, const char *pathP, int lineNo, const char *msgP, const char *argP)
{
int n;

    n = snprintf(errP, OVL_ERRSIZE, "%s line %d: ", pathP, lineNo);
    if( (n >= 0) && (n < OVL_ERRSIZE) )
    {
        snprintf(errP + n, OVL_ERRSIZE - n, msgP, argP);
    }

    return(false);
}

// Compare one overlay with the drum: a read-only one word for word over its span, the
// padding included; a persistent one by its header only, since the overlay manager
// writes its words back.
// Returns OVL_CURRENT, or OVL_STAMP_DIFFERS or OVL_DIFFERS with
// the first differing drum address, the drum's word and the file's in *addrP, *gotP and
// *wantP.
int
ovlCompare(const Overlay *ovP, const int32_t *drumP, int *addrP, int32_t *gotP, int32_t *wantP)
{
int a;
int span;
int32_t want;
int32_t got;

    span = (ovP->persistent) ? OVL_HEADERWORDS : ovP->span;
    for( a = 0; a < span; ++a )
    {
        want = (a < ovP->length) ? ovP->wordsP[a] : 0;
        got = drumP[(ovP->track * OVL_TRACKWORDS) + ovP->drum + a];
        if( got != want )
        {
            *addrP = ovP->drum + a;
            *gotP = got;
            *wantP = want;
            return( (a < OVL_HEADERWORDS) ? OVL_STAMP_DIFFERS : OVL_DIFFERS );
        }
    }

    return(OVL_CURRENT);
}

// Whether drumupdater's marker is in the last three words of a track.
bool
drumupdaterMarked(const int32_t *drumP, int track)
{
const int32_t *trackP;

    trackP = &drumP[track * OVL_TRACKWORDS];
    return( (trackP[DRUMUPDATER_MARKFIRST] == DRUMUPDATER_MARK) &&
        (trackP[DRUMUPDATER_MARKFIRST + 1] == DRUMUPDATER_MARK) &&
        (trackP[DRUMUPDATER_MARKFIRST + 2] == DRUMUPDATER_MARK) );
}

// Whether Adventure's save block is on its track, by its magic.
bool
adventureSaved(const int32_t *drumP)
{
    return( drumP[ADVENTURE_SAVE_TRACK * OVL_TRACKWORDS] == ADVENTURE_SAVE_MAGIC );
}

// The tracks a Z-machine story installed by zloader occupies from track 0, 0 if track 0
// does not hold one.
int
storyTracks(const int32_t *drumP)
{
int i;
int version;
long length;
long words;

    // Each of a story's words holds two bytes, so its header's words are 16 bits.
    for( i = 0; i < (ZH_HEADER_BYTES / 2); ++i )
    {
        if( drumP[i] & ~0177777 )
        {
            return(0);
        }
    }

    version = storyByte(drumP, ZH_VERSION);
    if( (version < 3) || (version > 5) )
    {
        return(0);
    }

    for( i = 0; i < ZH_SERIAL_BYTES; ++i )
    {
        if( (storyByte(drumP, ZH_SERIAL + i) < '0') || (storyByte(drumP, ZH_SERIAL + i) > '9') )
        {
            return(0);
        }
    }

    // The length field counts 2-byte units for V3 and 4-byte ones for V4 and V5 (S11.1.6).
    length = ((storyByte(drumP, ZH_FILE_LENGTH) << 8) | storyByte(drumP, ZH_FILE_LENGTH + 1)) *
        ((version == 3) ? 2 : 4);

    // A field of 0 (some early V3 stories) installs the whole file, whose length the
    // drum does not record; zloader zeros everything after the story, so the story
    // ends at the last word that is not zero.
    if( length == 0 )
    {
        for( words = OVL_DRUMWORDS; (words > 0) && (drumP[words - 1] == 0); --words )
        {
        }

        length = words * 2;
    }

    if( (((storyByte(drumP, ZH_HIGHMEM_BASE) << 8) | storyByte(drumP, ZH_HIGHMEM_BASE + 1)) > length) ||
        (((storyByte(drumP, ZH_INITIAL_PC) << 8) | storyByte(drumP, ZH_INITIAL_PC + 1)) > length) ||
        (((storyByte(drumP, ZH_STATIC_BASE) << 8) | storyByte(drumP, ZH_STATIC_BASE + 1)) > length) )
    {
        return(0);
    }

    words = (length + 1) / 2;
    return((int)((words + (OVL_TRACKWORDS - 1)) / OVL_TRACKWORDS));
}

// The story byte at byteOff, as zloader packs two to a word, the first in the high byte.
int
storyByte(const int32_t *drumP, int byteOff)
{
    return( (byteOff & 1) ? (drumP[byteOff / 2] & 0377) : ((drumP[byteOff / 2] >> 8) & 0377) );
}

// The drum writer.

static const char *progNameP = "ovlfile";
static FILE *msgFP;                 // NULL until ovlSetProgram(), then stderr is used

static FILE *errFile(void);
static int overlayNames(const OvlFile *ofP, int track, char *namesP, size_t size);

// Name the tool for the messages that follow, and say where its faults and warnings go.
void
ovlSetProgram(const char *nameP, FILE *errFP)
{
    progNameP = nameP;
    msgFP = errFP;
}

// Where faults and warnings go.
static FILE *
errFile(void)
{
    return( (msgFP) ? msgFP : stderr );
}

// Read the drum file into drumP (OVL_DRUMWORDS words), words past its end zero.
// A missing file leaves drumP zero and *fileWordsP -1.
// Returns false, after saying why, if the file can't be read.
bool
ovlReadDrum(const char *pathP, int32_t *drumP, long *fileWordsP)
{
int fd;
ssize_t got;
struct stat st;

    memset(drumP, 0, OVL_DRUMWORDS * sizeof(int32_t));
    if( (fd = open(pathP, O_RDONLY)) < 0 )
    {
        if( errno == ENOENT )
        {
            *fileWordsP = -1;
            return(true);
        }

        fprintf(errFile(), "%s: can't open drum file '%s': %s\n", progNameP, pathP, strerror(errno));
        return(false);
    }

    if( (fstat(fd, &st) < 0) || ((got = read(fd, drumP, OVL_DRUMWORDS * sizeof(int32_t))) < 0) )
    {
        fprintf(errFile(), "%s: can't read drum file '%s': %s\n", progNameP, pathP, strerror(errno));
        close(fd);
        return(false);
    }

    *fileWordsP = (long)(st.st_size / (off_t)sizeof(int32_t));
    close(fd);
    return(true);
}

// Check the tape's word at the overlay table's address, which haveWord says the tape has,
// against the .ovl's stamp, so a drum and a program from different builds are never loaded
// together.
// Returns true if they match, else false after saying why.
bool
ovlStampMatches(const OvlFile *ofP, const char *tapeP, bool haveWord, uint32_t word)
{
    if( !haveWord )
    {
        fprintf(errFile(), "%s: the tape '%s' has no word at the overlay table's address %06o;"
            " it is not this program's tape.\n", progNameP, tapeP, ofP->tableAddr);
        return(false);
    }

    if( (int)word != ofP->stamp )
    {
        fprintf(errFile(), "%s: the tape's stamp %06o is not the .ovl's %06o: they are from different"
            " builds.  Nothing was written.\n", progNameP, word, ofP->stamp);
        return(false);
    }

    return(true);
}

// Write each overlay's span on its track at its drum address: its words, then zeros to the
// span's end.
// fileWords is what ovlReadDrum() found.
// With eachOverlay, a line for each overlay says what was written.
// Without, one line for them all.
// Returns false, after saying why and what was written, if a write fails.
bool
ovlWriteDrum(const char *pathP, const OvlFile *ofP, long fileWords, bool eachOverlay)
{
int i;
int t;
int fd;
int span;
int nTracks;
size_t bytes;
size_t used;
long end;
bool onTrack[OVL_TRACKS];
char tracks[OVL_TRACKS * 4];
int32_t buffer[OVL_TRACKWORDS];
const Overlay *ovP;

    if( (fd = open(pathP, O_RDWR | O_CREAT, 0666)) < 0 )
    {
        fprintf(errFile(), "%s: can't open drum file '%s' to write it: %s\n", progNameP, pathP, strerror(errno));
        return(false);
    }

    if( fileWords < 0 )
    {
        printf("Drum file %s created.\n", pathP);
    }

    end = 0;
    memset(onTrack, 0, sizeof(onTrack));
    for( i = 0; i < ofP->count; ++i )
    {
        ovP = &ofP->overlaysP[i];
        span = ovP->span;
        if( ((long)(ovP->track * OVL_TRACKWORDS) + ovP->drum + span) > end )
        {
            end = (long)(ovP->track * OVL_TRACKWORDS) + ovP->drum + span;
        }
        memset(buffer, 0, sizeof(buffer));
        memcpy(buffer, ovP->wordsP, ovP->length * sizeof(int32_t));
        bytes = span * sizeof(int32_t);

        // Past the file's end, the write extends it and the gap reads as zeros.
        if( pwrite(fd, buffer, bytes, (off_t)((ovP->track * OVL_TRACKWORDS) + ovP->drum) * sizeof(int32_t))
            != (ssize_t)bytes )
        {
            fprintf(errFile(), "%s: writing overlay %s to '%s' failed: %s\n", progNameP, ovP->name, pathP,
                strerror(errno));
            fprintf(errFile(), "%s: %d of %d overlays were written before it.\n", progNameP, i, ofP->count);
            close(fd);
            return(false);
        }

        onTrack[ovP->track] = true;
        if( eachOverlay )
        {
            printf("overlay %s: track %o, drum %04o-%04o, %o words, %s\n", ovP->name, ovP->track,
                ovP->drum, ovP->drum + span - 1, ovP->length, (ovP->persistent) ? "rw" : "ro");
        }
    }

    if( close(fd) != 0 )
    {
        fprintf(errFile(), "%s: closing '%s' failed: %s\n", progNameP, pathP, strerror(errno));
        return(false);
    }

    if( !eachOverlay )
    {
        nTracks = 0;
        used = 0;
        tracks[0] = '\0';
        for( t = 0; t < OVL_TRACKS; ++t )
        {
            if( onTrack[t] )
            {
                used += (size_t)snprintf(tracks + used, sizeof(tracks) - used, "%s%o",
                    (nTracks++ == 0) ? "" : ", ", t);
            }
        }

        printf("Wrote %d overlay%s on track%s %s of drum file %s.\n", ofP->count,
            (ofP->count == 1) ? "" : "s", (nTracks == 1) ? "" : "s", tracks, pathP);
    }

    // Only a file that ended before the last overlay's span was extended; the file then
    // ends there, not at a whole drum.
    if( (fileWords >= 0) && (fileWords < end) )
    {
        printf("Drum file %s was %ld words long; the overlays past its end extended it with zeros.\n",
            pathP, fileWords);
    }

    return(true);
}

// Warn of each track an overlay was written on that held another tool's data: a program
// drumupdater loaded, Adventure's save block, or part of a Z-machine story.
// drumP still holds the drum as it was before the write.
void
ovlWarnOverwritten(const OvlFile *ofP, const int32_t *drumP)
{
int t;
int zTracks;
char names[1024];

    zTracks = storyTracks(drumP);
    for( t = 0; t < OVL_TRACKS; ++t )
    {
        if( !overlayNames(ofP, t, names, sizeof(names)) )
        {
            continue;
        }

        if( drumupdaterMarked(drumP, t) )
        {
            fprintf(errFile(), "%s: track %o (%d decimal) held a program drumupdater loaded; it was overwritten"
                " by %s.\n", progNameP, t, t, names);
        }

        if( (t == ADVENTURE_SAVE_TRACK) && adventureSaved(drumP) )
        {
            fprintf(errFile(), "%s: track %o (%d decimal) held Adventure's save block; it was overwritten"
                " by %s.\n", progNameP, t, t, names);
        }

        if( t < zTracks )
        {
            fprintf(errFile(), "%s: track %o (%d decimal) held part of a Z-machine story, tracks 0-%o; it was"
                " overwritten by %s.\n", progNameP, t, t, zTracks - 1, names);
        }
    }
}

// The overlays written on a track, as "overlay a" or "overlays a, b", in namesP.
// Returns how many there are.
static int
overlayNames(const OvlFile *ofP, int track, char *namesP, size_t size)
{
int i;
int n;
int listed;
size_t used;

    n = 0;
    for( i = 0; i < ofP->count; ++i )
    {
        if( ofP->overlaysP[i].track == track )
        {
            ++n;
        }
    }

    // snprintf() gives the length it wanted, so a truncated list ends the loop.
    used = (size_t)snprintf(namesP, size, "%s", (n == 1) ? "overlay" : "overlays");
    listed = 0;
    for( i = 0; (i < ofP->count) && (used < size); ++i )
    {
        if( ofP->overlaysP[i].track == track )
        {
            used += (size_t)snprintf(namesP + used, size - used, "%s%s", (listed++ == 0) ? " " : ", ",
                ofP->overlaysP[i].name);
        }
    }

    return(n);
}

// The .ovl that goes with a tape: its name with the extension replaced by .ovl, or with .ovl
// added if it has none.
// Returns it in allocated memory, or NULL if memory runs out.
char *
ovlNameForTape(const char *tapeP)
{
char *nameP;
const char *dotP;
const char *slashP;
size_t base;

    // A dot in a directory's name, or one that begins the file's name, is not an extension.
    base = strlen(tapeP);
    dotP = strrchr(tapeP, '.');
    slashP = strrchr(tapeP, '/');
    if( dotP && (!slashP || (dotP > (slashP + 1))) && (dotP != tapeP) )
    {
        base = (size_t)(dotP - tapeP);
    }

    if( !(nameP = malloc(base + 5)) )
    {
        return(NULL);
    }

    memcpy(nameP, tapeP, base);
    strcpy(nameP + base, ".ovl");
    return(nameP);
}
