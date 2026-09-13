/*
 * Mount a tape image on a Type 550 microtape drive.
 *
 * Usage: mtp [-f mapfile] <drive-number> <filename>
 * where:
 * -f mapfile   - update mapfile instead of /opt/pidp1-mods/microtapes.txt
 * drive-number - the drive, 1-8 (decimal, as microtapes.txt numbers them)
 * filename     - the tape to mount, a relative name is relative to /opt/pidp1-mods,
                  appending ",locked" mounts it write-locked
 *
 * The drive's line in the mount file is replaced or added as needed.
 * The change takes effect immediately, a program will see it when the drive is next assigned.
 * See Docs/UsingType550Microtape.md.
 *
 * If the mapping file doesn't exist, it wll be created.
 *
 * Exit status is 0 if the list was written, 1 otherwise.
 *
 * Revision history:
 *
 * 13/09/2026 wje - initial version
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>

#define BASE_DIR        "/opt/pidp1-mods"   // where the emulator resolves a relative image name
#define DEFAULT_LIST    BASE_DIR "/microtapes.txt"
#define UNITS           8                   // drives 1-8
#define PATH_MAX_LEN    256                 // the emulator's limit on an image path (MT_PATH_MAX)
#define LIST_MAX_BYTES  16384               // the emulator reads no more of the list than this

static char *readFile(const char *pathP, size_t *lenP, bool *existsP);
static int lineDrive(const char *lineP, size_t len);
static bool checkName(const char *nameP);
static void noteImage(const char *nameP);
static bool writeList(const char *listP, const char *textP, size_t len, mode_t mode);
static void usage(void);

int
main(int argc, char **argv)
{
const char *listArgP;
const char *nameP;
char listPath[PATH_MAX];
char newLine[PATH_MAX_LEN + 64];
char *oldP;
char *newP;
char *endP;
const char *cP;
const char *nlP;
const char *wasP;
size_t oldLen;
size_t newLen;
size_t wasLen;
size_t len;
struct stat st;
mode_t mode;
long drive;
bool exists;
int dropped;
int opt;

    listArgP = DEFAULT_LIST;
    while( (opt = getopt(argc, argv, "f:")) != -1 )
    {
        if( opt == 'f' )
        {
            listArgP = optarg;
        }
        else
        {
            usage();
        }
    }

    if( (argc - optind) != 2 )
    {
        usage();
    }

    // The drive, decimal 1-8.
    errno = 0;
    drive = strtol(argv[optind], &endP, 10);
    if( (endP == argv[optind]) || (*endP != 0) || (errno != 0) || (drive < 1) || (drive > UNITS) )
    {
        fprintf(stderr, "A drive number must be 1-8.\n");
        return(1);
    }

    nameP = argv[optind + 1];
    if( !checkName(nameP) )
    {
        return(1);
    }

    // Replace the list itself, not a symbolic link to it.
    if( realpath(listArgP, listPath) == NULL )
    {
        if( errno != ENOENT )
        {
            fprintf(stderr, "Can't open or create %s: %s\n", listArgP, strerror(errno));
            return(1);
        }

        snprintf(listPath, sizeof(listPath), "%s", listArgP);      // a new list
    }

    if( (oldP = readFile(listPath, &oldLen, &exists)) == NULL )
    {
        return(1);
    }

    mode = 0644;
    if( exists && (stat(listPath, &st) == 0) )
    {
        mode = (st.st_mode & 07777);
    }

    // Every line kept but the drive's, whose first occurrance becomes the new one.
    // There really shouldn't be multiples, but it's harmless, the first is always used.
    snprintf(newLine, sizeof(newLine), "%ld %s\n", drive, nameP);
    if( (newP = malloc(oldLen + strlen(newLine) + 2)) == NULL )
    {
        fprintf(stderr, "mount: out of memory\n");
        return(1);
    }

    newLen = 0;
    wasP = NULL;
    wasLen = 0;
    dropped = 0;
    for( cP = oldP; cP < (oldP + oldLen); cP = (nlP ? (nlP + 1) : (oldP + oldLen)) )
    {
        nlP = memchr(cP, '\n', (size_t)((oldP + oldLen) - cP));
        len = (size_t)((nlP ? nlP : (oldP + oldLen)) - cP);

        if( lineDrive(cP, len) == drive )
        {
            if( wasP == NULL )
            {
                wasP = cP;
                wasLen = len;
                memcpy(newP + newLen, newLine, strlen(newLine));
                newLen += strlen(newLine);
            }
            else
            {
                ++dropped;                  // a later line would have overridden the new one
            }
            continue;
        }

        memcpy(newP + newLen, cP, len);
        newLen += len;
        newP[newLen++] = '\n';              // a last line without one gets one
    }

    if( wasP == NULL )
    {
        memcpy(newP + newLen, newLine, strlen(newLine));
        newLen += strlen(newLine);
    }

    if( newLen > LIST_MAX_BYTES )
    {
        fprintf(stderr, "The file to mount's name is too long!\n");
        return(1);
    }

    if( !writeList(listPath, newP, newLen, mode) )
    {
        return(1);
    }

    noteImage(nameP);
    free(oldP);
    free(newP);
    return(0);
}

// Reads the file at pathP.
// A missing file reads as empty, setting *existsP false.
// Returns the bytes in a malloc()d buffer, else NULL.
static char *
readFile(const char *pathP, size_t *lenP, bool *existsP)
{
FILE *fP;
char *bufP;
size_t size;
size_t len;
size_t got;

    *lenP = 0;
    *existsP = false;
    size = 4096;
    if( (bufP = malloc(size + 1)) == NULL )
    {
        fprintf(stderr, "Uut of memory!\n");
        return(NULL);
    }

    if( (fP = fopen(pathP, "r")) == NULL )
    {
        if( errno == ENOENT )
        {
            bufP[0] = 0;
            return(bufP);
        }

        free(bufP);
        return(NULL);
    }

    *existsP = true;
    len = 0;
    while( (got = fread(bufP + len, 1, (size - len), fP)) > 0 )
    {
        len += got;
        if( len == size )
        {
            size *= 2;
            if( (bufP = realloc(bufP, size + 1)) == NULL )
            {
                fprintf(stderr, "Out of memory!\n");
                fclose(fP);
                return(NULL);
            }
        }
    }

    if( ferror(fP) )
    {
        fprintf(stderr, "Read error reading  %s.\n", pathP);
        fclose(fP);
        free(bufP);
        return(NULL);
    }

    fclose(fP);
    bufP[len] = 0;
    *lenP = len;
    return(bufP);
}

// Returns the drive number a line of the list is for.
// Returns 0 for a blank line, a comment, or a line the emulator would reject as malformed.
static int
lineDrive(const char *lineP, size_t len)
{
char head[64];
char *cP;
char *endP;
long drive;

    while( (len > 0) && ((lineP[len - 1] == '\r') || (lineP[len - 1] == ' ') || (lineP[len - 1] == '\t')) )
    {
        --len;
    }

    // The drive number and the white space after it are all that matter.
    if( len >= sizeof(head) )
    {
        len = (sizeof(head) - 1);
    }

    memcpy(head, lineP, len);
    head[len] = 0;

    for( cP = head; (*cP == ' ') || (*cP == '\t'); ++cP )
    {
        // skip leading white space
    }

    if( (*cP == 0) || (*cP == '#') )
    {
        return(0);
    }

    errno = 0;
    drive = strtol(cP, &endP, 10);
    if( (endP == cP) || ((*endP != ' ') && (*endP != '\t')) || (errno != 0) || (drive < 1) || (drive > UNITS) )
    {
        return(0);
    }

    return( (int)drive );
}

// Checks that nameP can be written as a line of the list and read back as the same name,
// not empty, no line break, no white space at either end, and short enough for the IOT itself.
// Returns true if valid, else reports why on stderr and returns false.
static bool
checkName(const char *nameP)
{
const char *commaP;
size_t len;
size_t pathLen;

    len = strlen(nameP);
    if( len == 0 )
    {
        fprintf(stderr, "The file name is empty.\n");
        return(false);
    }

    if( strpbrk(nameP, "\n\r") )
    {
        fprintf(stderr, "The file name contains a line break.\n");
        return(false);
    }

    if( (nameP[0] == ' ') || (nameP[0] == '\t') || (nameP[len - 1] == ' ') || (nameP[len - 1] == '\t') )
    {
        fprintf(stderr, "The file name begins or ends with white space, not allowed.\n");
        return(false);
    }

    pathLen = len;
    if( ((commaP = strrchr(nameP, ',')) != NULL) && (strcmp(commaP, ",locked") == 0) )
    {
        pathLen = (size_t)(commaP - nameP);
    }

    if( pathLen == 0 )
    {
        fprintf(stderr, "There is no file name before \",locked\".\n");
        return(false);
    }

    if( pathLen >= PATH_MAX_LEN )
    {
        fprintf(stderr, "The file name is %zu characters, only %d is allwoed.\n", pathLen,
            (PATH_MAX_LEN - 1));
        return(false);
    }

    return(true);
}

// Says what the emulator will do with the image named nameP.
// Advice only on stdout; a missing image is not an error.
// No return value.
static void
noteImage(const char *nameP)
{
char path[PATH_MAX_LEN + sizeof(BASE_DIR) + 1];
const char *commaP;
size_t pathLen;
bool locked;

    pathLen = strlen(nameP);
    locked = false;
    if( ((commaP = strrchr(nameP, ',')) != NULL) && (strcmp(commaP, ",locked") == 0) )
    {
        pathLen = (size_t)(commaP - nameP);
        locked = true;
    }

    if( nameP[0] == '/' )
    {
        snprintf(path, sizeof(path), "%.*s", (int)pathLen, nameP);
    }
    else
    {
        snprintf(path, sizeof(path), "%s/%.*s", BASE_DIR, (int)pathLen, nameP);
    }

    if( access(path, F_OK) == 0 )
    {
        return;
    }

    if( locked )
    {
        printf("%s does not exist, a lock for a missing image mounts nothing.\n", path);
    }
    else
    {
        printf("%s does not exist, a blank tape is being created.\n", path);
    }
}

// Writes len bytes of textP as the file listP, with the given mode to a tmp file in
// the same directory first, then renamed over listP.
// Returns true on success, otherwise reports the error on stderr,
// removes the temporary file and leaves listP as it was and returns false.
static bool
writeList(const char *listP, const char *textP, size_t len, mode_t mode)
{
char tmp[PATH_MAX + 16];
FILE *fP;
int fd;
bool ok;

    snprintf(tmp, sizeof(tmp), "%s.XXXXXX", listP);
    if( (fd = mkstemp(tmp)) < 0 )
    {
        fprintf(stderr, "Can't create tmp file for %s: %s\n", listP, strerror(errno));
        return(false);
    }

    if( (fP = fdopen(fd, "w")) == NULL )
    {
        fprintf(stderr, "Error opening %s: %s\n", tmp, strerror(errno));
        close(fd);
        unlink(tmp);
        return(false);
    }

    ok = ((fwrite(textP, 1, len, fP) == len) && (fflush(fP) == 0) && (fchmod(fd, mode) == 0)
        && (fsync(fd) == 0));
    ok = ((fclose(fP) == 0) && ok);

    if( !ok || (rename(tmp, listP) != 0) )
    {
        fprintf(stderr, "Can't write to %s: %s\n", listP, strerror(errno));
        unlink(tmp);
        return(false);
    }

    return(true);
}

static void
usage(void)
{
    fprintf(stderr, "Usage: mtp [-f mapfile] <drive-number> <filename>\n"
        "  Mounts filename on microtape drive drive-number (1-8),\n"
        "  updating %s unless -f names another.\n"
        "  A relative filename is relative to /opt/pidp1-mods;\n"
        "  \",locked\" on the end mounts it write-locked.\n"
        "  The mount takes effect on the next IOT mse.\n", DEFAULT_LIST);
    exit(1);
}
