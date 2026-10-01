// A name=value settings file edited line by line, for pidp1.config and pdp1control.config.
// The file is kept as its lines, so loading and saving with no edit gives back the same bytes,
// and an edit changes only the value it names: comments, order and unknown settings survive.
// What counts as a setting is what the emulator's parser (src/blincolnlights/pdp1/configuration.c)
// accepts: '#' in column 1 is a comment, an empty line is skipped, and anything else must match
// sscanf "%63[a-zA-Z0-9] = %63s", read by fgets() into a 256-byte buffer. When a name appears on
// more than one line, the last one wins, as it does in that parser.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core.h"

#define MAX_ENTRIES 64          // lines for one name that a single edit will handle

// Where a setting sits: its line, and its value within the line.
typedef struct
{
    int line;
    bool editable;              // the line fits in one fgets() read, so editing it is safe
    size_t valueStart;
    size_t valueLen;            // the value as the parser takes it, at most 63 characters
    size_t tokenLen;            // the whole token, which is longer when the value is
} ConfEntry;

// The characters sscanf's %[a-zA-Z0-9] accepts.
static bool
isNameChar(char c)
{
    return( ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) );
}

// The characters sscanf skips for a space in its format and stops %s at, in the C locale.
static bool
isSpaceChar(char c)
{
    return( (c == ' ') || (c == '\t') || (c == '\n') || (c == '\v') || (c == '\f') || (c == '\r') );
}

// Parse one fgets() read of a line as the emulator does.
// Returns true for a setting, with the name's length and the value's place set.
static bool
parseChunk(const char *sP, size_t len, size_t *nameLenP, size_t *valueStartP, size_t *valueLenP,
    size_t *tokenLenP)
{
size_t i, nameLen, valueStart;

    if( (len == 0) || (sP[0] == '#') )
    {
        return(false);
    }

    for( nameLen = 0; ((nameLen < len) && isNameChar(sP[nameLen])); nameLen++ )
    {
    }

    // A name over 63 characters leaves an alphanumeric where the '=' must be.
    if( (nameLen == 0) || (nameLen > CONF_MAX_VALUE) )
    {
        return(false);
    }

    for( i = nameLen; ((i < len) && isSpaceChar(sP[i])); i++ )
    {
    }

    if( (i >= len) || (sP[i] != '=') )
    {
        return(false);
    }

    for( i++; ((i < len) && isSpaceChar(sP[i])); i++ )
    {
    }

    if( i >= len )
    {
        return(false);
    }

    valueStart = i;
    for( ; ((i < len) && !isSpaceChar(sP[i])); i++ )
    {
    }

    *nameLenP = nameLen;
    *valueStartP = valueStart;
    *tokenLenP = (i - valueStart);
    *valueLenP = ((*tokenLenP > CONF_MAX_VALUE) ? CONF_MAX_VALUE : *tokenLenP);
    return(true);
}

// Find every active line for a name, in file order, as the parser sees them.
// A line longer than the parser's buffer is read in pieces, and each piece is a line to it,
// so each piece is parsed on its own. Once maxEntries are stored, each later one overwrites
// the last slot, so that slot always holds the line that wins.
// Returns the number found, which can exceed maxEntries.
static int
findEntries(ConfFile *cfP, const char *nameP, ConfEntry *entriesP, int maxEntries)
{
int line, count, slot;
size_t lineLen, start, chunkLen, nameLen, valueStart, valueLen, tokenLen;
const char *lineP;

    count = 0;
    for( line = 0; line < cfP->lineCount; line++ )
    {
        lineP = cfP->linesP[line];
        lineLen = strlen(lineP);
        for( start = 0; start < lineLen; start += CONF_MAX_LINE )
        {
            chunkLen = (((lineLen - start) > CONF_MAX_LINE) ? CONF_MAX_LINE : (lineLen - start));
            if( parseChunk(lineP + start, chunkLen, &nameLen, &valueStart, &valueLen, &tokenLen) &&
                (strlen(nameP) == nameLen) && !strncmp(lineP + start, nameP, nameLen) )
            {
                slot = ((count < maxEntries) ? count : (maxEntries - 1));
                entriesP[slot].line = line;
                entriesP[slot].editable = (lineLen <= CONF_MAX_LINE);
                entriesP[slot].valueStart = (start + valueStart);
                entriesP[slot].valueLen = valueLen;
                entriesP[slot].tokenLen = tokenLen;
                count++;
            }
        }
    }

    return(count);
}

// Record why a call failed, for the caller to show.
static void
setError(ConfFile *cfP, const char *textP)
{
    snprintf(cfP->errorText, sizeof(cfP->errorText), "%s", textP);
}

// Append a line, taking ownership of textP.
// Returns false if memory ran out, and then frees textP.
static bool
addLine(ConfFile *cfP, char *textP)
{
char **newP;
int newCap;

    if( cfP->lineCount == cfP->lineCap )
    {
        newCap = ((cfP->lineCap == 0) ? 64 : (cfP->lineCap * 2));
        if( !(newP = realloc(cfP->linesP, (newCap * sizeof(char *)))) )
        {
            free(textP);
            return(false);
        }

        cfP->linesP = newP;
        cfP->lineCap = newCap;
    }

    cfP->linesP[cfP->lineCount++] = textP;
    return(true);
}

// Replace line lineIndex with the concatenation of three pieces.
// Returns false, with the line unchanged, if the result would be longer than the parser reads.
static bool
replaceLine(ConfFile *cfP, int lineIndex, const char *aP, size_t aLen, const char *bP, size_t bLen,
    const char *cP)
{
char *newP;
size_t cLen;

    cLen = strlen(cP);
    if( (aLen + bLen + cLen) > CONF_MAX_LINE )
    {
        setError(cfP, "the line would be longer than the 255 characters the emulator reads");
        return(false);
    }

    if( !(newP = malloc(aLen + bLen + cLen + 1)) )
    {
        setError(cfP, "out of memory");
        return(false);
    }

    memcpy(newP, aP, aLen);
    memcpy(newP + aLen, bP, bLen);
    memcpy(newP + aLen + bLen, cP, cLen + 1);
    free(cfP->linesP[lineIndex]);
    cfP->linesP[lineIndex] = newP;
    return(true);
}

// Note the file's modification time and size, to see later whether it changed on disk.
static void
noteDiskState(ConfFile *cfP)
{
struct stat st;

    if( !stat(cfP->pathP, &st) )
    {
        cfP->existed = true;
        cfP->loadedMtime = st.st_mtim;
        cfP->loadedSize = st.st_size;
    }
    else
    {
        cfP->existed = false;
        cfP->loadedMtime.tv_sec = 0;
        cfP->loadedMtime.tv_nsec = 0;
        cfP->loadedSize = 0;
    }
}

// Load a file as its lines. A missing file loads as empty.
// Returns NULL on a read error, or for a file holding a NUL byte, which a line cannot keep.
ConfFile *
confLoad(const char *pathP)
{
ConfFile *cfP;
FILE *fP;
char *dataP, *growP, *startP, *endP, *lineP;
size_t len, cap, got;

    if( !(cfP = calloc(1, sizeof(ConfFile))) || !(cfP->pathP = strdup(pathP)) )
    {
        free(cfP);
        return(NULL);
    }

    noteDiskState(cfP);
    if( !(fP = fopen(pathP, "r")) )
    {
        if( errno == ENOENT )
        {
            return(cfP);
        }

        confFree(cfP);
        return(NULL);
    }

    // Read the whole file.
    len = 0;
    cap = 4096;
    dataP = malloc(cap);
    while( dataP && ((got = fread(dataP + len, 1, (cap - len), fP)) > 0) )
    {
        len += got;
        if( len == cap )
        {
            cap *= 2;
            if( !(growP = realloc(dataP, cap)) )
            {
                free(dataP);
                dataP = NULL;
            }
            else
            {
                dataP = growP;
            }
        }
    }

    if( !dataP || ferror(fP) || memchr(dataP, '\0', len) )
    {
        fclose(fP);
        free(dataP);
        confFree(cfP);
        return(NULL);
    }

    fclose(fP);

    // Split at each newline; a final newline ends the last line rather than starting another.
    cfP->finalNewline = ((len > 0) && (dataP[len - 1] == '\n'));
    startP = dataP;
    while( startP < (dataP + len) )
    {
        if( !(endP = memchr(startP, '\n', ((dataP + len) - startP))) )
        {
            endP = (dataP + len);
        }

        if( !(lineP = strndup(startP, (endP - startP))) || !addLine(cfP, lineP) )
        {
            free(dataP);
            confFree(cfP);
            return(NULL);
        }

        startP = (endP + 1);
    }

    free(dataP);
    return(cfP);
}

// Free a loaded file.
void
confFree(ConfFile *cfP)
{
int i;

    if( !cfP )
    {
        return;
    }

    for( i = 0; i < cfP->lineCount; i++ )
    {
        free(cfP->linesP[i]);
    }

    free(cfP->linesP);
    free(cfP->pathP);
    free(cfP);
}

// Copy a loaded file, so the editor can keep what is on disk beside its edits.
// Returns NULL if memory ran out.
ConfFile *
confCopy(const ConfFile *cfP)
{
ConfFile *newP;
char *lineP;
int i;

    if( !(newP = calloc(1, sizeof(ConfFile))) )
    {
        return(NULL);
    }

    *newP = *cfP;
    newP->linesP = NULL;
    newP->lineCount = 0;
    newP->lineCap = 0;
    if( !(newP->pathP = strdup(cfP->pathP)) )
    {
        free(newP);
        return(NULL);
    }

    for( i = 0; i < cfP->lineCount; i++ )
    {
        if( !(lineP = strdup(cfP->linesP[i])) || !addLine(newP, lineP) )
        {
            confFree(newP);
            return(NULL);
        }
    }

    return(newP);
}

// Point a loaded file at another path, so that a file loaded from pidp1.config.example can be
// saved as pidp1.config. Returns false if memory ran out.
bool
confRetarget(ConfFile *cfP, const char *pathP)
{
char *newP;

    if( !(newP = strdup(pathP)) )
    {
        return(false);
    }

    free(cfP->pathP);
    cfP->pathP = newP;
    cfP->backedUp = false;
    noteDiskState(cfP);
    return(true);
}

// The value the emulator would use for a name: the last active line's, as it reads it.
// Returns NULL if no line sets it. The result is valid until the next call.
const char *
confGet(ConfFile *cfP, const char *nameP)
{
ConfEntry last;

    if( findEntries(cfP, nameP, &last, 1) == 0 )
    {
        return(NULL);
    }

    memcpy(cfP->valueText, cfP->linesP[last.line] + last.valueStart, last.valueLen);
    cfP->valueText[last.valueLen] = '\0';
    return(cfP->valueText);
}

// Check a name and a value against what the parser can read back.
static bool
checkNameValue(ConfFile *cfP, const char *nameP, const char *valueP)
{
size_t i;

    if( (strlen(nameP) == 0) || (strlen(nameP) > CONF_MAX_VALUE) )
    {
        setError(cfP, "a name is 1 to 63 characters");
        return(false);
    }

    for( i = 0; nameP[i]; i++ )
    {
        if( !isNameChar(nameP[i]) )
        {
            setError(cfP, "a name is letters and digits only");
            return(false);
        }
    }

    if( !valueP )
    {
        return(true);
    }

    if( (strlen(valueP) == 0) || (strlen(valueP) > CONF_MAX_VALUE) )
    {
        setError(cfP, "a value is 1 to 63 characters");
        return(false);
    }

    for( i = 0; valueP[i]; i++ )
    {
        if( isSpaceChar(valueP[i]) || ((unsigned char)valueP[i] < ' ') )
        {
            setError(cfP, "a value cannot hold a space or a control character");
            return(false);
        }
    }

    return(true);
}

// Set a name's value.
// The last active line gets the new value, and only its value token changes; earlier active
// lines for the name are commented out so a reader that takes the first line sees the same
// value. With no active line, the last commented-out "#name=value" is restored with the new
// value, and with neither, a line is appended.
// Returns false, with the file unchanged, if the value or the resulting line is rejected.
bool
confSet(ConfFile *cfP, const char *nameP, const char *valueP)
{
ConfEntry entries[MAX_ENTRIES];
ConfEntry *lastP;
int count, i, line, commented;
size_t lineLen, nameLen, valueStart, valueLen, tokenLen;
char *textP;
size_t newLen;

    if( !checkNameValue(cfP, nameP, valueP) )
    {
        return(false);
    }

    if( (count = findEntries(cfP, nameP, entries, MAX_ENTRIES)) > MAX_ENTRIES )
    {
        setError(cfP, "too many lines for this name");
        return(false);
    }

    if( count > 0 )
    {
        // Check every line first, so a refusal leaves the file as it was.
        for( i = 0; i < count; i++ )
        {
            if( !entries[i].editable )
            {
                setError(cfP, "a line for this name is longer than the emulator reads; edit it by hand");
                return(false);
            }

            if( (i < (count - 1)) && (strlen(cfP->linesP[entries[i].line]) >= CONF_MAX_LINE) )
            {
                setError(cfP, "commenting out a duplicate line would make it too long");
                return(false);
            }
        }

        lastP = &entries[count - 1];
        textP = cfP->linesP[lastP->line];
        if( !replaceLine(cfP, lastP->line, textP, lastP->valueStart, valueP, strlen(valueP),
                textP + lastP->valueStart + lastP->tokenLen) )
        {
            return(false);
        }

        for( i = 0; i < (count - 1); i++ )
        {
            textP = cfP->linesP[entries[i].line];
            replaceLine(cfP, entries[i].line, "#", 1, textP, strlen(textP), "");
        }

        return(true);
    }

    // Find the last commented-out default for the name.
    commented = -1;
    for( line = 0; line < cfP->lineCount; line++ )
    {
        textP = cfP->linesP[line];
        lineLen = strlen(textP);
        if( (lineLen > 1) && (lineLen <= CONF_MAX_LINE) && (textP[0] == '#') &&
            parseChunk(textP + 1, (lineLen - 1), &nameLen, &valueStart, &valueLen, &tokenLen) &&
            (nameLen == strlen(nameP)) && !strncmp(textP + 1, nameP, nameLen) )
        {
            commented = line;
        }
    }

    if( commented >= 0 )
    {
        textP = cfP->linesP[commented];
        parseChunk(textP + 1, (strlen(textP) - 1), &nameLen, &valueStart, &valueLen, &tokenLen);
        return( replaceLine(cfP, commented, textP + 1, valueStart, valueP, strlen(valueP),
            textP + 1 + valueStart + tokenLen) );
    }

    // Append a new line.
    newLen = (strlen(nameP) + 1 + strlen(valueP));
    if( !(textP = malloc(newLen + 1)) )
    {
        setError(cfP, "out of memory");
        return(false);
    }

    snprintf(textP, (newLen + 1), "%s=%s", nameP, valueP);
    if( !addLine(cfP, textP) )
    {
        setError(cfP, "out of memory");
        return(false);
    }

    cfP->finalNewline = true;
    return(true);
}

// Comment out every active line for a name, so each reader falls back to its default.
// Returns false, with the file unchanged, if a line cannot be commented out safely.
bool
confUnset(ConfFile *cfP, const char *nameP)
{
ConfEntry entries[MAX_ENTRIES];
int count, i;
char *textP;

    if( !checkNameValue(cfP, nameP, NULL) )
    {
        return(false);
    }

    if( (count = findEntries(cfP, nameP, entries, MAX_ENTRIES)) > MAX_ENTRIES )
    {
        setError(cfP, "too many lines for this name");
        return(false);
    }

    for( i = 0; i < count; i++ )
    {
        if( !entries[i].editable || (strlen(cfP->linesP[entries[i].line]) >= CONF_MAX_LINE) )
        {
            setError(cfP, "a line for this name is too long to comment out; edit it by hand");
            return(false);
        }
    }

    for( i = 0; i < count; i++ )
    {
        textP = cfP->linesP[entries[i].line];
        if( !replaceLine(cfP, entries[i].line, "#", 1, textP, strlen(textP), "") )
        {
            return(false);
        }
    }

    return(true);
}

// Write all of fd, retrying short writes.
static bool
writeAll(int fd, const char *dataP, size_t len)
{
ssize_t n;

    while( len > 0 )
    {
        if( (n = write(fd, dataP, len)) < 0 )
        {
            if( errno == EINTR )
            {
                continue;
            }
            return(false);
        }

        dataP += n;
        len -= n;
    }

    return(true);
}

// Copy a file, for the backup when a hard link cannot be made.
static bool
copyFile(const char *fromP, const char *toP)
{
int inFd, outFd;
ssize_t n;
bool ok;
char buffer[8192];

    if( (inFd = open(fromP, O_RDONLY)) < 0 )
    {
        return(false);
    }

    if( (outFd = open(toP, (O_WRONLY | O_CREAT | O_TRUNC), 0644)) < 0 )
    {
        close(inFd);
        return(false);
    }

    ok = true;
    while( ok && ((n = read(inFd, buffer, sizeof(buffer))) > 0) )
    {
        ok = writeAll(outFd, buffer, n);
    }

    ok = (ok && (n == 0) && !fsync(outFd));
    close(inFd);
    return( !close(outFd) && ok );
}

// Save the file: write a temporary file beside it, sync it, and rename it over the file, so a
// crash leaves either the old file or the new one. The first save of a session keeps the old
// file as <name>.bak. Returns false, with errorText set, on any failure.
bool
confSave(ConfFile *cfP)
{
char *tmpP, *bakP, *dirP, *slashP;
struct stat st;
mode_t mode;
int fd, i;
bool ok;

    tmpP = malloc(strlen(cfP->pathP) + 8);
    bakP = malloc(strlen(cfP->pathP) + 8);
    dirP = strdup(cfP->pathP);
    if( !tmpP || !bakP || !dirP )
    {
        free(tmpP);
        free(bakP);
        free(dirP);
        setError(cfP, "out of memory");
        return(false);
    }

    sprintf(tmpP, "%s.tmp", cfP->pathP);
    sprintf(bakP, "%s.bak", cfP->pathP);
    mode = (!stat(cfP->pathP, &st) ? (st.st_mode & 0777) : 0644);

    // Write and sync the new contents.
    ok = ((fd = open(tmpP, (O_WRONLY | O_CREAT | O_TRUNC), mode)) >= 0);
    for( i = 0; ok && (i < cfP->lineCount); i++ )
    {
        ok = writeAll(fd, cfP->linesP[i], strlen(cfP->linesP[i]));
        if( ok && ((i < (cfP->lineCount - 1)) || cfP->finalNewline) )
        {
            ok = writeAll(fd, "\n", 1);
        }
    }

    if( fd >= 0 )
    {
        ok = (ok && !fsync(fd));
        ok = (!close(fd) && ok);
    }

    if( !ok )
    {
        snprintf(cfP->errorText, sizeof(cfP->errorText), "writing %s: %s", tmpP, strerror(errno));
        unlink(tmpP);
    }

    // Keep the file as it was before this session's first save; a new file has nothing to keep.
    if( ok && !cfP->backedUp )
    {
        if( !stat(cfP->pathP, &st) )
        {
            unlink(bakP);
            if( link(cfP->pathP, bakP) && !copyFile(cfP->pathP, bakP) )
            {
                snprintf(cfP->errorText, sizeof(cfP->errorText), "making %s: %s", bakP, strerror(errno));
                unlink(tmpP);
                ok = false;
            }
        }

        cfP->backedUp = ok;
    }

    if( ok && rename(tmpP, cfP->pathP) )
    {
        snprintf(cfP->errorText, sizeof(cfP->errorText), "renaming %s: %s", tmpP, strerror(errno));
        unlink(tmpP);
        ok = false;
    }

    // Sync the directory, so the rename itself survives a crash.
    if( ok )
    {
        if( (slashP = strrchr(dirP, '/')) )
        {
            *(slashP + ((slashP == dirP) ? 1 : 0)) = '\0';
        }
        else
        {
            strcpy(dirP, ".");
        }

        if( (fd = open(dirP, O_RDONLY)) >= 0 )
        {
            fsync(fd);
            close(fd);
        }

        noteDiskState(cfP);
    }

    free(tmpP);
    free(bakP);
    free(dirP);
    return(ok);
}

// Whether the file on disk differs from the one loaded or last saved, by its modification
// time and size, or by having appeared or gone.
bool
confChangedOnDisk(ConfFile *cfP)
{
struct stat st;

    if( stat(cfP->pathP, &st) )
    {
        return(cfP->existed);
    }

    return( !cfP->existed || (st.st_size != cfP->loadedSize) ||
        (st.st_mtim.tv_sec != cfP->loadedMtime.tv_sec) ||
        (st.st_mtim.tv_nsec != cfP->loadedMtime.tv_nsec) );
}

// List the distinct names the file sets, in the order they first appear.
// Returns the number listed, at most maxNames.
int
confNames(ConfFile *cfP, char namesP[][CONF_MAX_VALUE + 1], int maxNames)
{
int line, count, i;
size_t lineLen, start, chunkLen, nameLen, valueStart, valueLen, tokenLen;
const char *lineP;
bool seen;

    count = 0;
    for( line = 0; line < cfP->lineCount; line++ )
    {
        lineP = cfP->linesP[line];
        lineLen = strlen(lineP);
        for( start = 0; start < lineLen; start += CONF_MAX_LINE )
        {
            chunkLen = (((lineLen - start) > CONF_MAX_LINE) ? CONF_MAX_LINE : (lineLen - start));
            if( !parseChunk(lineP + start, chunkLen, &nameLen, &valueStart, &valueLen, &tokenLen) )
            {
                continue;
            }

            seen = false;
            for( i = 0; (i < count) && !seen; i++ )
            {
                seen = ((strlen(namesP[i]) == nameLen) && !strncmp(namesP[i], lineP + start, nameLen));
            }

            if( !seen && (count < maxNames) )
            {
                memcpy(namesP[count], lineP + start, nameLen);
                namesP[count][nameLen] = '\0';
                count++;
            }
        }
    }

    return(count);
}
