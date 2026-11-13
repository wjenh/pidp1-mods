// Convert an ASR-33 teletype style tape to and from a regular text file.
//
// Usage:
//     asr33tape punch [-l N] [-f] [TEXT [TAPE]]    a text file to a tape
//     asr33tape text [TAPE [TEXT]]                 a tape to a text file
//     asr33tape -h                                 usage
//
// A missing name, or -, is standard input or output.
//
// Punch creates the same tape an ASR-33 does, blank frames of leader, every character with its eighth bit
// set CR LF after each line, blank frames of trailer.
// The text's lines can end in LF, CR LF or CR.
// A last line with no line terminator has one added.
// A NUL or a byte of 0200 or more is an error, the tape holds 7-bit ASCII.
// With -f it also warns of what FOCAL's * would read differently from the
// text, the tape is still punched.
//
// Text turns a tape back into text with the eighth bit dropped, blank frames
// and all-holes frames, a rubout, dropped, CR LF and a bare CR mapped to LF.
// Everything else is kept as it is.
//
// Exit status is 0, or 1 with a message on stderr.
//
// 13-Nov-2026 wje initial version, reworked from a discarded version.
//
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#define LEADER_DEFAULT  60          // blank frames of leader and of trailer
#define LEADER_MAX      1000        // a sanity bound on -l
#define EIGHTH_BIT      0200        // set on every character punched
#define ALL_HOLES       0377        // a character rubbed out on the punch
#define CHAR_CR         015
#define CHAR_LF         012
#define CHAR_DEL        0177        // RUBOUT
#define LINE_BUF_MAX    74          // FOCAL's line buffer; a longer line must start with a line number

typedef struct
{
    unsigned char *dataP;
    size_t len;
    size_t size;
} Buffer;

static const char *progName = "asr33tape";

static bool readAll(const char *nameP, Buffer *bufP);
static bool writeAll(const char *nameP, const Buffer *bufP);
static bool bufPut(Buffer *bufP, unsigned char c);
static const char *showName(const char *nameP, const char *stdP);
static void focalWarnings(const char *nameP, long lineNo, const unsigned char *lineP, size_t len);
static int doPunch(int argc, char **argv);
static int doText(int argc, char **argv);
static void usage(FILE *outP);

// Reads the whole of the named file, or standard input for NULL or "-",
// into bufP.
// Returns true, or false with a message on stderr.
static bool
readAll(const char *nameP, Buffer *bufP)
{
FILE *inP;
size_t got;
bool isStdin;
unsigned char *newP;

    isStdin = ((nameP == NULL) || (strcmp(nameP, "-") == 0));
    if( isStdin )
    {
        inP = stdin;
    }
    else if( !(inP = fopen(nameP, "rb")) )
    {
        fprintf(stderr, "%s: %s: %s\n", progName, nameP, strerror(errno));
        return( false ) );
    }
    bufP->dataP = NULL;
    bufP->len = 0;
    bufP->size = 0;
    for(;;)
    {
        if( bufP->len == bufP->size )
        {
            bufP->size = ((bufP->size == 0) ? 65536 : (bufP->size * 2));
            if( !(newP = realloc(bufP->dataP, bufP->size)) )
            {
                fprintf(stderr, "%s: out of memory reading %s\n", progName, showName(nameP, "standard input"));
                if( !isStdin )
                {
                    fclose(inP);
                }
                return( false );
            }
            bufP->dataP = newP;
        }
        got = fread((bufP->dataP + bufP->len), 1, (bufP->size - bufP->len), inP);
        bufP->len += got;
        if( got == 0 )
        {
            break;
        }
    }
    if( ferror(inP) )
    {
        fprintf(stderr, "%s: %s: read error\n", progName, showName(nameP, "standard input"));
        if( !isStdin )
        {
            fclose(inP);
        }
        return( false );
    }
    if( !isStdin )
    {
        fclose(inP);
    }
    return( true );
}

// Writes bufP to the named file, or standard output for NULL or "-".
// Returns true, or false with a message on stderr.
static bool
writeAll(const char *nameP, const Buffer *bufP)
{
FILE *outP;
bool isStdout;
bool ok;

    isStdout = ((nameP == NULL) || (strcmp(nameP, "-") == 0));
    if( isStdout )
    {
        outP = stdout;
    }
    else if( !(outP = fopen(nameP, "wb")) )
    {
        fprintf(stderr, "%s: %s: %s\n", progName, nameP, strerror(errno));
        return( false );
    }
    // an empty buffer still makes an empty file
    ok = ((bufP->len == 0) || (fwrite(bufP->dataP, 1, bufP->len, outP) == bufP->len));
    if( isStdout )
    {
        ok = ((fflush(outP) == 0) && ok);
    }
    else
    {
        ok = ((fclose(outP) == 0) && ok);
    }
    if( !ok )
    {
        fprintf(stderr, "%s: %s: write error\n", progName, showName(nameP, "standard output"));
    }
    return( ok );
}

// Appends one byte to bufP, growing it as needed.
// Returns true, or false with a message on stderr when memory runs out.
static bool
bufPut(Buffer *bufP, unsigned char c)
{
unsigned char *newP;

    if( bufP->len == bufP->size )
    {
        bufP->size = ((bufP->size == 0) ? 65536 : (bufP->size * 2));
        if( !(newP = realloc(bufP->dataP, bufP->size)) )
        {
            fprintf(stderr, "%s: out of memory\n", progName);
            return( false );
        }
        bufP->dataP = newP;
    }
    bufP->dataP[bufP->len] = c;
    bufP->len += 1;
    return( true );
}

// The name to print for a file argument: stdP ("standard input" or
// "standard output") stands for NULL or "-".
static const char *
showName(const char *nameP, const char *stdP)
{
    if( (nameP == NULL) || (strcmp(nameP, "-") == 0) )
    {
        return( stdP );
    }
    return( nameP );
}

// Under -f, warns on stderr of everything in one line
// that FOCAL's * reads differently from the text, one warning per.
static void
focalWarnings(const char *nameP, long lineNo, const unsigned char *lineP, size_t len)
{
size_t i;
size_t first;
bool sawArrow;
bool sawDel;
bool sawAt;
bool sawCtl;

    sawArrow = false;
    sawDel = false;
    sawAt = false;
    sawCtl = false;
    for(i = 0; i < len; i++)
    {
        if( lineP[i] == '_' )
        {
            sawArrow = true;
        }
        else if( lineP[i] == CHAR_DEL )
        {
            sawDel = true;
        }
        else if( lineP[i] == '@' )
        {
            sawAt = true;
        }
        else if( lineP[i] < 040 )
        {
            sawCtl = true;
        }
    }
    if( sawArrow )
    {
        fprintf(stderr, "%s: warning: %s, line %ld: _ (left arrow) erases the line read so far\n",
                progName, showName(nameP, "standard input"), lineNo);
    }
    if( sawDel )
    {
        fprintf(stderr, "%s: warning: %s, line %ld: DEL (RUBOUT) erases the character before it\n",
                progName, showName(nameP, "standard input"), lineNo);
    }
    if( sawAt )
    {
        fprintf(stderr, "%s: warning: %s, line %ld: @ is passed over\n",
                progName, showName(nameP, "standard input"), lineNo);
    }
    if( sawCtl )
    {
        fprintf(stderr, "%s: warning: %s, line %ld: a tab or other control character is stored as it is\n",
                progName, showName(nameP, "standard input"), lineNo);
    }
    if( (len > 0) && (lineP[0] == '*') )
    {
        fprintf(stderr, "%s: warning: %s, line %ld: a line starting with * ends the reading\n",
                progName, showName(nameP, "standard input"), lineNo);
    }

    // FOCAL keeps a line longer than its buffer only as a stored line: after
    // any blanks, its first character must be a digit, else ?10.:5.
    if( focal && (len > LINE_BUF_MAX) )
    {
        first = 0;
        while( (first < len) && (lineP[first] == ' ') )
        {
            first++;
        }
        if( (first == len) || (lineP[first] < '0') || (lineP[first] > '9') )
        {
            fprintf(stderr, "%s: warning: %s, line %ld: %zu characters and no line number (?10.:5)\n",
                    progName, showName(nameP, "standard input"), lineNo, len);
        }
    }
}

// asr33tape punch: a text file to a tape.
// Returns the exit status: 0, or 1 with a message on stderr.
static int
doPunch(int argc, char **argv)
{
Buffer in;
Buffer out;
const char *names[2];
int nNames;
int i;
long leader;
long k;
long lineNo;
char *endP;
bool focal;
bool optionsDone;
size_t pos;
size_t start;
unsigned char c;

    leader = LEADER_DEFAULT;
    focal = false;
    optionsDone = false;
    nNames = 0;
    names[0] = NULL;
    names[1] = NULL;
    for(i = 0; i < argc; i++)
    {
        if( !optionsDone && (strcmp(argv[i], "--") == 0) )
        {
            optionsDone = true;
        }
        else if( !optionsDone && (strcmp(argv[i], "-h") == 0) )
        {
            usage(stdout);
            return( 0 );
        }
        else if( !optionsDone && (strcmp(argv[i], "-f") == 0) )
        {
            focal = true;
        }
        else if( !optionsDone && (strcmp(argv[i], "-l") == 0) )
        {
            if( (i + 1) >= argc )
            {
                fprintf(stderr, "%s: -l needs a count\n", progName);
                usage(stderr);
                return( 1 );
            }
            i++;
            errno = 0;
            leader = strtol(argv[i], &endP, 10);
            if( (errno != 0) || (endP == argv[i]) || (*endP != '\0') || (leader < 0) || (leader > LEADER_MAX) )
            {
                fprintf(stderr, "%s: -l %s: not a count from 0 to %d\n", progName, argv[i], LEADER_MAX);
                return( 1 );
            }
        }
        else if( !optionsDone && (argv[i][0] == '-') && (argv[i][1] != '\0') )
        {
            fprintf(stderr, "%s: unknown option %s\n", progName, argv[i]);
            usage(stderr);
            return( 1 );
        }
        else if( nNames < 2 )
        {
            names[nNames] = argv[i];
            nNames++;
        }
        else
        {
            fprintf(stderr, "%s: too many names\n", progName);
            usage(stderr);
            return( 1 );
        }
    }

    if( !readAll(names[0], &in) )
    {
        return( 1 );
    }
    out.dataP = NULL;
    out.len = 0;
    out.size = 0;
    for(k = 0; k < leader; k++)
    {
        if( !bufPut(&out, 0) )
        {
            return( 1 );
        }
    }

    // Each line is checked, warned of, and punched with CR LF; CR LF, CR and
    // LF each end one line.
    pos = 0;
    lineNo = 0;
    while( pos < in.len )
    {
        lineNo++;
        start = pos;
        while( (pos < in.len) && (in.dataP[pos] != CHAR_CR) && (in.dataP[pos] != CHAR_LF) )
        {
            c = in.dataP[pos];
            if( c == 0 )
            {
                fprintf(stderr, "%s: %s, line %ld: a NUL; the tape holds 7-bit ASCII\n",
                        progName, showName(names[0], "standard input"), lineNo);
                return( 1 );
            }
            if( c >= EIGHTH_BIT )
            {
                fprintf(stderr, "%s: %s, line %ld: byte 0%o; the tape holds 7-bit ASCII\n",
                        progName, showName(names[0], "standard input"), lineNo, (unsigned int)c);
                return( 1 );
            }
            if( !bufPut(&out, (unsigned char)(c | EIGHTH_BIT)) )
            {
                return( 1 );
            }
            pos++;
        }
        if( focal )
        {
            focalWarnings(names[0], lineNo, (in.dataP + start), (pos - start));
        }
        if( !bufPut(&out, (CHAR_CR | EIGHTH_BIT)) || !bufPut(&out, (CHAR_LF | EIGHTH_BIT)) )
        {
            return( 1 );
        }
        if( pos < in.len )
        {
            if( (in.dataP[pos] == CHAR_CR) && ((pos + 1) < in.len) && (in.dataP[pos + 1] == CHAR_LF) )
            {
                pos++;
            }
            pos++;
        }
    }

    for(k = 0; k < leader; k++)
    {
        if( !bufPut(&out, 0) )
        {
            return( 1 );
        }
    }
    if( !writeAll(names[1], &out) )
    {
        return( 1 );
    }
    free(in.dataP);
    free(out.dataP);
    return( 0 );
}

// asr33tape text: a tape to a text file with LF line terminators.
// Returns the exit status: 0, or 1 with a message on stderr.
static int
doText(int argc, char **argv)
{
Buffer in;
Buffer out;
const char *names[2];
int nNames;
int i;
bool optionsDone;
bool afterCr;
size_t pos;
unsigned char c;

    optionsDone = false;
    nNames = 0;
    names[0] = NULL;
    names[1] = NULL;
    for(i = 0; i < argc; i++)
    {
        if( !optionsDone && (strcmp(argv[i], "--") == 0) )
        {
            optionsDone = true;
        }
        else if( !optionsDone && (strcmp(argv[i], "-h") == 0) )
        {
            usage(stdout);
            return( 0 );
        }
        else if( !optionsDone && (argv[i][0] == '-') && (argv[i][1] != '\0') )
        {
            fprintf(stderr, "%s: unknown option %s\n", progName, argv[i]);
            usage(stderr);
            return( 1 );
        }
        else if( nNames < 2 )
        {
            names[nNames] = argv[i];
            nNames++;
        }
        else
        {
            fprintf(stderr, "%s: too many names\n", progName);
            usage(stderr);
            return( 1 );
        }
    }

    if( !readAll(names[0], &in) )
    {
        return( 1 );
    }
    out.dataP = NULL;
    out.len = 0;
    out.size = 0;

    // Dropped frames are invisible, so a CR, blank frames, then LF is still
    // one line end.
    afterCr = false;
    for(pos = 0; pos < in.len; pos++)
    {
        c = in.dataP[pos];
        if( (c == 0) || (c == EIGHTH_BIT) || (c == ALL_HOLES) )
        {
            continue;
        }
        c = (unsigned char)(c & 0177);
        if( (c == CHAR_LF) && afterCr )
        {
            afterCr = false;
            continue;
        }
        afterCr = (c == CHAR_CR);
        if( !bufPut(&out, (((c == CHAR_CR) || (c == CHAR_LF)) ? CHAR_LF : c)) )
        {
            return( 1 );
        }
    }

    if( !writeAll(names[1], &out) )
    {
        return( 1 );
    }
    free(in.dataP);
    free(out.dataP);
    return( 0 );
}

// Prints the usage to outP.
static void
usage(FILE *outP)
{
    fprintf(outP,
        "usage: asr33tape punch [-l N] [-f] [TEXT [TAPE]]    a text file to a tape\n"
        "       asr33tape text [TAPE [TEXT]]                 a tape to a text file\n"
        "       asr33tape -h                                 this usage\n"
        "A missing name or a - is standard input or output.\n"
        "punch: writes each character with the eighth bit set, CR LF after each line,\n"
        "       %d blank frames of leader and of trailer (-l, default %d).\n"
        "       The text lines can end in LF, CR LF or CR.\n"
        "       A NUL or a byte of 0200 or more is refused.\n"
        "       -f warns of what FOCAL's * reads differently: _, DEL, @,\n"
        "       a line starting with *, control characters, and a line over %d\n"
        "       characters with no line number.\n"
        "text:  the eighth bit is dropped, blank and all-holes frames are dropped,\n"
        "       CR LF and CR converted to one LF.\n",
        LEADER_DEFAULT, LEADER_DEFAULT, LINE_BUF_MAX);
}

int
main(int argc, char **argv)
{
    if( argc < 2 )
    {
        usage(stderr);
        return( 1 );
    }
    if( strcmp(argv[1], "-h") == 0 )
    {
        usage(stdout);
        return( 0 );
    }
    if( strcmp(argv[1], "punch") == 0 )
    {
        return( doPunch((argc - 2), (argv + 2)) );
    }
    if( strcmp(argv[1], "text") == 0 )
    {
        return( doText((argc - 2), (argv + 2)) );
    }
    fprintf(stderr, "%s: unknown command %s\n", progName, argv[1]);
    usage(stderr);
    return( 1 );
}
