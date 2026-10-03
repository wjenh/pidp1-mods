// Reads microtapes.txt, the Type 550 drives' mount list, as the Microtape plugin reads it
// (IOTs/Microtape/microtape.c, readListFile, parseList, parseListLine and parseSpec), so the
// Microtape tab shows what the emulator will mount at its next mse.
// A line is "<drive> <path>[,locked]": the drive in decimal, 1-8, then spaces or tabs, then the
// rest of the line with trailing spaces, tabs and CR dropped; the path may hold spaces. Blank
// lines and lines whose first non-space character is # are skipped. A drive listed twice gets
// the later line. A line that does not parse is skipped and counted.
//
// 02-Oct-2026 wje (Claude) - written.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "core.h"

#define LIST_MAX_BYTES  16384               // the plugin reads no more of the file than this
#define LINE_MAX_LEN    (MT_SPEC_MAX + 32)  // and no line longer than this
#define PATH_LIMIT      256                 // MT_PATH_MAX: a path is at most 255 characters

static void parseLine(char *lineP, MtList *listP);
static void parseSpec(MtList *listP, int drive);

// Read the list at pathP into listP. A missing file lists no tapes.
// Returns false, with listP->error set and every drive empty, if the file exists but could not
// be read.
bool
mtListLoad(const char *pathP, MtList *listP)
{
static char buf[LIST_MAX_BYTES + 1];
char line[LINE_MAX_LEN];
struct stat st;
const char *cP, *endP, *nlP;
size_t len;
FILE *fP;

    memset(listP, 0, sizeof(*listP));
    if( stat(pathP, &st) )
    {
        listP->error = ((errno == ENOENT) ? 0 : errno);
        return(listP->error == 0);
    }

    listP->exists = true;
    listP->ino = st.st_ino;
    listP->size = st.st_size;
    listP->mtime = st.st_mtim;
    listP->ctime = st.st_ctim;
    if( !(fP = fopen(pathP, "r")) )
    {
        listP->error = errno;
        return(false);
    }

    len = fread(buf, 1, LIST_MAX_BYTES, fP);
    if( ferror(fP) )
    {
        listP->error = EIO;
        fclose(fP);
        return(false);
    }
    fclose(fP);

    cP = buf;
    endP = (buf + len);
    while( cP < endP )
    {
        nlP = memchr(cP, '\n', (size_t)(endP - cP));
        len = (size_t)((nlP ? nlP : endP) - cP);
        if( len >= (sizeof(line) - 1) )
        {
            listP->skipped++;           // too long for any valid entry
        }
        else
        {
            memcpy(line, cP, len);
            line[len] = '\0';
            parseLine(line, listP);
        }

        cP = (nlP ? (nlP + 1) : endP);
    }

    return(true);
}

// Whether the file at pathP is not the one listP was read from: created, removed, renamed over
// (as mtp writes it), or changed in size or time.
bool
mtListChanged(const char *pathP, const MtList *listP)
{
struct stat st;

    if( stat(pathP, &st) )
    {
        return(listP->exists || ((errno != ENOENT) && (errno != listP->error)));
    }

    return( !listP->exists || (st.st_ino != listP->ino) || (st.st_size != listP->size) ||
        (st.st_mtim.tv_sec != listP->mtime.tv_sec) || (st.st_mtim.tv_nsec != listP->mtime.tv_nsec) ||
        (st.st_ctim.tv_sec != listP->ctime.tv_sec) || (st.st_ctim.tv_nsec != listP->ctime.tv_nsec) );
}

// Take one line (no newline) into listP. No return value.
static void
parseLine(char *lineP, MtList *listP)
{
char *cP, *endP;
size_t len;
long drive;

    len = strlen(lineP);
    while( (len > 0) && ((lineP[len - 1] == '\r') || (lineP[len - 1] == ' ') || (lineP[len - 1] == '\t')) )
    {
        lineP[--len] = '\0';
    }

    for( cP = lineP; (*cP == ' ') || (*cP == '\t'); cP++ )
    {
        // skip leading white space
    }

    if( (*cP == '\0') || (*cP == '#') )
    {
        return;
    }

    errno = 0;
    drive = strtol(cP, &endP, 10);
    if( (endP == cP) || ((*endP != ' ') && (*endP != '\t')) || (errno != 0) || (drive < 1) ||
        (drive > MT_DRIVES) )
    {
        listP->skipped++;
        return;
    }

    for( cP = endP; (*cP == ' ') || (*cP == '\t'); cP++ )
    {
        // skip to the path
    }

    if( strlen(cP) >= MT_SPEC_MAX )
    {
        listP->skipped++;
        return;
    }

    strcpy(listP->spec[drive], cP);
    parseSpec(listP, (int)drive);
}

// Split a drive's entry into its path and lock. An entry with no path, or a path too long, is
// one the plugin will not mount: its path is left "". No return value.
static void
parseSpec(MtList *listP, int drive)
{
const char *specP, *commaP;
size_t len;

    specP = listP->spec[drive];
    listP->locked[drive] = false;
    listP->path[drive][0] = '\0';
    len = strlen(specP);
    if( ((commaP = strrchr(specP, ',')) != NULL) && !strcmp(commaP, ",locked") )
    {
        listP->locked[drive] = true;
        len = (size_t)(commaP - specP);
    }

    if( (len > 0) && (len < PATH_LIMIT) )
    {
        memcpy(listP->path[drive], specP, len);
        listP->path[drive][len] = '\0';
    }
}
