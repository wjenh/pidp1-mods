/*
 * Mount a tape image on a Type 550 microtape drive.
 *
 * Usage: mtp [-f mapfile] <drive-number> <filename>
 *        mtp [-f mapfile] -u <drive-number>
 * where:
 * -f mapfile   - update mapfile instead of /opt/pidp1-mods/microtapes.txt
 * -u           - unmount: remove the drive's line(s) instead of mounting a tape
 * drive-number - the drive, 1-8 (decimal, as microtapes.txt numbers them)
 * filename     - the tape to mount, a relative name is relative to /opt/pidp1-mods,
 *                appending ",locked" mounts it write-locked
 *
 * The drive's line in the mount file is replaced or added as needed.
 * With -u every line for the drive is removed, which unmounts its tape.
 * The change takes effect immediately, a program will see it when the drive is next assigned.
 * See Docs/UsingType550Microtape.md.
 *
 * If the mapping file doesn't exist, it will be created.
 * With -u it is not: a drive with no line is already unmounted.
 *
 * Exit status is 0 if the list was written, 1 otherwise.
 * With -u it is also 0 if the drive had no line, the list is then left untouched.
 *
 * Revision history:
 *
 * 13/09/2026 wje initial version
 * 13/09/2026 Claude -u unmounts a drive
 * 16/09/2026 wje -l lists the mounted drives
 * 17/09/2026 wje minor cleanup, no functionality change
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
const char *driveArgP;
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
size_t len;
struct stat st;
mode_t mode;
long drive;
bool exists;
bool unmount;
bool list;
int dropped;
int removed;
int opt;
FILE *fP;

    listArgP = DEFAULT_LIST;
    driveArgP = NULL;
    list = unmount = false;
    while( (opt = getopt(argc, argv, "f:ul")) != -1 )
    {
        switch( opt )
        {
        case 'f':
            listArgP = optarg;
            break;

        case 'u':
            unmount = true;
            break;

        case 'l':
            list = true;
            break;

        default:
            usage();
        }
    }

    // One or the other
    if( list && unmount )
    {
        usage();
    }

    if( list )
    {
        if( optind != argc )
        {
            usage();
        }

        if( !(fP = fopen(listArgP, "r")) )
        {
            printf("There is no tape mount file '%s'.\n", listArgP);
            exit(1);
        }

        while( fgets(newLine, sizeof(newLine), fP) )
        {
            fputs(newLine, stdout);
        }

        fclose(fP);
        exit(0);
    }

    // Mounting takes a drive and a filename; -u takes a drive, no filename.
    if( (argc - optind) < 1 )
    {
        usage();
    }

    // The drive, decimal 1-8.
    errno = 0;
    driveArgP = argv[optind++];
    drive = strtol(driveArgP, &endP, 10);
    if( (endP == driveArgP) || (*endP != 0) || (errno != 0) || (drive < 1) || (drive > UNITS) )
    {
        fprintf(stderr, "A drive number must be 1-8.\n");
        return(1);
    }

    if( !unmount && ((argc - optind) != 1) )
    {
        usage();
    }

    if( !unmount )
    {
        nameP = argv[optind];
        if( !checkName(nameP) )
        {
            return(1);
        }
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

    // Every line kept but the drive's, whose first occurrence becomes the new one.
    // There really shouldn't be multiples, but the emulator uses the last line for a drive,
    // so any later ones are dropped, else they would override the new line.
    // Unmounting keeps every line but the drive's too, and puts nothing in their place.
    newLine[0] = 0;
    if( !unmount )
    {
        snprintf(newLine, sizeof(newLine), "%ld %s\n", drive, nameP);
    }

    if( (newP = malloc(oldLen + strlen(newLine) + 2)) == NULL )
    {
        fprintf(stderr, "Out of memory!\n");
        return(1);
    }

    newLen = 0;
    wasP = NULL;
    dropped = 0;
    removed = 0;
    for( cP = oldP; cP < (oldP + oldLen); cP = (nlP ? (nlP + 1) : (oldP + oldLen)) )
    {
        nlP = memchr(cP, '\n', (size_t)((oldP + oldLen) - cP));
        len = (size_t)((nlP ? nlP : (oldP + oldLen)) - cP);

        if( lineDrive(cP, len) == drive )
        {
            if( unmount )
            {
                ++removed;                  // every line for the drive goes, not just the first
            }
            else if( wasP == NULL )
            {
                wasP = cP;
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

    if( unmount )
    {
        // No line for the drive, so it is already unmounted: that is success, and the list
        // (or its absence) is left exactly as it was.
        if( removed == 0 )
        {
            printf("Drive %ld has no line in %s, it is not mounted.\n", drive, listPath);
            free(oldP);
            free(newP);
            return(0);
        }
    }
    else if( wasP == NULL )
    {
        memcpy(newP + newLen, newLine, strlen(newLine));
        newLen += strlen(newLine);
    }

    // Unmounting only shrinks the list, so only a mount can make it too long.
    if( !unmount && (newLen > LIST_MAX_BYTES) )
    {
        fprintf(stderr, "The map file is too big!\nTrim out anything that doesn't belong there.\n");
        return(1);
    }

    if( !writeList(listPath, newP, newLen, mode) )
    {
        return(1);
    }

    if( !unmount )
    {
        noteImage(nameP);
    }

    free(oldP);
    free(newP);
    return(0);
}

// Reads the file at pathP.
// A missing file reads as empty, setting *existsP false.
// Returns the bytes in a malloc()d buffer, else reports why on stderr and returns NULL.
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
        fprintf(stderr, "Out of memory!\n");
        return(NULL);
    }

    if( (fP = fopen(pathP, "r")) == NULL )
    {
        if( errno == ENOENT )
        {
            bufP[0] = 0;
            return(bufP);
        }

        // It is there but can't be read (permission, say): say why before free() can touch errno.
        fprintf(stderr, "Can't open %s: %s\n", pathP, strerror(errno));
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
        fprintf(stderr, "Read error reading %s.\n", pathP);
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
        fprintf(stderr, "The file name is %zu characters, only %d are allowed.\n", pathLen,
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
        printf("%s does not exist, a lock for a missing tape does nothing.\n", path);
    }
    else
    {
        printf("%s does not exist, a blank tape will be created on first use.\n", path);
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
    fprintf(stderr,
        "Usage: mtp [-f mapfile] <drive-number> <filename>\n"
        "       mtp [-f mapfile] -u <drive-number>\n"
        "       mtp [-u mapfile] -l\n"
        "  Mounts filename on microtape drive drive-number (1-8),\n"
        "  updating %s unless -f names another.\n"
        "  A relative filename is relative to /opt/pidp1-mods;\n"
        "  \",locked\" on the end mounts it write-locked.\n"
        "  -u unmounts the drive instead, removing its line.\n"
        "  -l lists all mounted drives.\n"
        "  The change takes effect on the next IOT mse.\n", DEFAULT_LIST);
    exit(1);
}
