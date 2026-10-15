/*
 * ovlfile.c -- an am1 overlay file read and checked, its overlays compared with a drum,
 * and the drum's other tenants recognized; see ovlfile.h.
 *
 * file.ovl is text: a label line, a version line, a stamp line giving the build stamp
 * and the full address of the program's overlay table, then for each overlay a line
 * naming its window, bank, first and last address, drum track, length and mode,
 * followed by its words, the two header words (the stamp and the overlay's number)
 * included.  Every number is octal.
 *
 * 13-Oct-2026 wje/Claude moved out of ovlload.c
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "ovlfile.h"

#define OVLFILELABEL "%%am1 overlay file%%"
#define OVLFILEVERSION "V1"

static bool ovlFault(char *errP, const char *pathP, int lineNo, const char *msgP, const char *argP);

// Read the .ovl at pathP into *ofP and check all of it: the label and version, the
// stamp line, each overlay's fields, its words and header, and no two overlays
// overlapping on one track.  Returns true, or false with *ofP empty and the first
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
            if( strcmp(lineP, OVLFILEVERSION) != 0 )
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
            if( (sscanf(lineP, "overlay %63s window %63s bank %o first %o last %o track %o length %o mode %7s%n",
                    ovP->name, ovP->window, &ovP->bank, &ovP->first, &ovP->last, &ovP->track,
                    &ovP->length, mode, &consumed) != 8) || (lineP[consumed] != 0) )
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
            if( (otherP->track == ovP->track) && (otherP->first <= ovP->last) && (ovP->first <= otherP->last) )
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

// Put a fault in the .ovl at a line into errP, argP filling a %s in msgP.  Returns false,
// for the caller's convenience.
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

// Compare one overlay with the drum: a read-only one word for word over its window, the
// padding included; a persistent one by its header only, since the overlay manager
// writes its words back.  Returns OVL_CURRENT, or OVL_STAMP_DIFFERS or OVL_DIFFERS with
// the first differing drum address, the drum's word and the file's in *addrP, *gotP and
// *wantP.
int
ovlCompare(const Overlay *ovP, const int32_t *drumP, int *addrP, int32_t *gotP, int32_t *wantP)
{
int a;
int span;
int32_t want;
int32_t got;

    span = (ovP->persistent) ? OVL_HEADERWORDS : ((ovP->last - ovP->first) + 1);
    for( a = 0; a < span; ++a )
    {
        want = (a < ovP->length) ? ovP->wordsP[a] : 0;
        got = drumP[(ovP->track * OVL_TRACKWORDS) + ovP->first + a];
        if( got != want )
        {
            *addrP = ovP->first + a;
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
// does not hold one.  The header must hold up: a version zloader installs, a serial of
// ASCII digits, and its base addresses within the story's declared length.
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
