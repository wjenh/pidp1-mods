// Process various combinations of filenames and try to set up the list/source and symbol data.
// 24-Jun-2026 wje remove clearing of filename in openSourceFile(), no idea why it was done
// 20-Aug-2026 wje updae for new cpp replacement

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <sys/stat.h>

#include "ad1.h"

int numFiles;
FileInfoP files[MAXFILES];

static bool memMapped;

extern int curLine;

// Map addresses to file and line numbers.
// Since multiple source lines can use the same address, we keep them all in a list
MapEntryPP memMap[MEMBANKS];  // we don't allocate until we need to

bool openSourceFile(FileInfoP infoP);
FileInfoP resolveFiles(char *nameP);
bool loadFileMap(FileInfoP infoP);
bool findRim(FileInfoP infoP, char *rsltP);
FileInfoP getFileInfoP(int fileNo);
static bool isEndOverlay(char *lineP);
static bool isWordEntry(char *lineP, int *addressP);
static void loadOrigins(FileInfoP infoP, int *wordLinesP, int *wordAddrsP, int numWords);
static void freeOrigins(FileInfoP infoP);

extern bool loadSymbols(FileInfoP infoP);
extern void clearSymbols(void);
extern void forgetSources(void);

// Have we mapped a file and is it still open?
bool
isFileMapped(int fileNo)
{
FileInfoP infoP;

    if( (fileNo >= numFiles) || (fileNo < 0) )
    {
        return( false );
    }

    infoP = files[fileNo];
    return( infoP && (infoP->fP != NIL) );
}

// Is memory also mapped?
bool
isMemMapped()
{
    return(memMapped);
}

// Allocate a new FileInfo, add it to the files table, resolve the filenames, and open the source file.
// Returns the new FileInfoP if successful, else nil.
FileInfoP
newFile(char *nameP)
{
FileInfoP infoP;

    if( numFiles >= MAXFILES )
    {
        printf("The limit of %d open files has been reached, file not opened.\n", MAXFILES);
        return( NIL );
    }

    if( (infoP = resolveFiles(nameP)) )
    {
        if( openSourceFile(infoP) )
        {
            infoP->fileNo = numFiles;
            files[numFiles++] = infoP;
        }
        else
        {
            // We will return the infoP, but not put it in the file table, maybe the sym file can be opened.
            printf("Can't open file '%s' with any .am1 or .lst extension, source not available.\n", nameP);
        }

        if( loadFileMap(infoP) )
        {
            if( infoP->lstNameP )
            {
                printf("Source and addresses loaded from '%s'.\n",infoP->lstNameP);
            }
            else
            {
                printf("Source loaded from '%s', no list by address or symbol is available.\n", infoP->am1NameP);
            }
        }
        else
        {
            printf("No file '%s' found with a .lst or .am1 extension, source operations are not available.\n", nameP);
        }

        loadSymbols(infoP);

        // Only a file in the table has a number of its own for its overlays.
        if( getFileInfoP(infoP->fileNo) == infoP )
        {
            ovlLoad(infoP);
        }

        return( infoP );
    }
    else
    {
        return( NIL );
    }
}

// Derive the possible filenames for the 3 possible files.
// A filename could have been given as:
// filename
// filename.am1
// filename.sym
// filename.lst
// Infer the other names from whatever is given.
// Create a FileInfo and set the names in it.
// The allocated names are set in the passed arguments if not null.
// Returns the FileInfoP for all cases other than a nil or empty nameP, return nil in that case.
FileInfoP
resolveFiles(char *nameP)
{
char *extP;
char *cP;
FileInfoP infoP;

char name[1024];
char am1str[1024];
char lststr[1024];
char symstr[1024];

    if( !nameP || (*nameP == NUL) )
    {
        return( NIL );
    }

    infoP = (FileInfoP)calloc(1, sizeof(FileInfo));

    strcpy(name, nameP);        // copy because we modify it
    nameP = name;

    // Differentiate between relative path dots and an extension dot,
    // search backwards for a slash and if found, ignore it and all before it for finding the extension.
    if( (cP = strrchr(nameP,'/')) )
    {
        ++cP;
    }
    else
    {
        cP = nameP;
    }

    if( !(extP = strrchr(cP, '.')) )
    {
        extP = nameP + strlen(nameP);
        strcpy(extP, ".am1");
        strcpy(am1str, nameP);

        strcpy(extP, ".lst");
        strcpy(lststr, nameP);

        strcpy(extP, ".sym");
        strcpy(symstr, nameP);
    }
    else if( !strcmp(extP, ".rim") || !strcmp(extP, ".bin") )
    {
        strcpy(extP, ".am1");
        strcpy(am1str, nameP);

        strcpy(extP, ".lst");
        strcpy(lststr, nameP);

        strcpy(extP, ".sym");
        strcpy(symstr, nameP);
    }
    else if( !strcmp(extP, ".am1") )
    {
        strcpy(am1str, nameP);

        strcpy(extP, ".lst");
        strcpy(lststr, nameP);

        strcpy(extP, ".sym");
        strcpy(symstr, nameP);
    }
    else if( !strcmp(extP, ".lst") )
    {
        strcpy(lststr, nameP);

        strcpy(extP, ".am1");
        strcpy(am1str, nameP);

        strcpy(extP, ".sym");
        strcpy(symstr, nameP);
    }
    else if( !strcmp(extP, ".sym") )
    {
        strcpy(symstr, nameP);

        strcpy(extP, ".am1");
        strcpy(am1str, nameP);

        strcpy(extP, ".lst");
        strcpy(lststr, nameP);
    }

    infoP->am1NameP = (char *)malloc(strlen(am1str)+1);
    strcpy(infoP->am1NameP, am1str);
    infoP->lstNameP = (char *)malloc(strlen(lststr)+1);
    strcpy(infoP->lstNameP, lststr);
    infoP->symNameP = (char *)malloc(strlen(symstr)+1);
    strcpy(infoP->symNameP, symstr);

    return( infoP );
}

// Try to open a source file, either a .lst or a .am1.
// Return true if successful, else false.
bool
openSourceFile(FileInfoP infoP)
{
    if( infoP->fP )
    {
        return(true);           // already open
    }

    if( infoP->lstNameP && (infoP->fP = fopen(infoP->lstNameP, "r")) )
    {
        return( true );
    }

    if( infoP->am1NameP && (infoP->fP = fopen(infoP->am1NameP, "r")) )
    {
        return( true );
    }

    return( false );
}

// Free the space allocated for all the file entries,
// reset the pointers to nil.
// Also deallocates any memory maps.
void
closeFiles()
{
int i, j;
FileInfoP infoP;
MapEntryPP bankP;
MapEntryP entryP, nextP;;

    for( i = 0; i < numFiles; ++i )
    {
        if( !(infoP = files[i]) )
        {
            continue;       // shouldn't happen
        }

        if( infoP->fP )
        {
            fclose(infoP->fP);
        }

        if( infoP->am1NameP )
        {
            free( infoP->am1NameP );
        }

        if( infoP->lstNameP )
        {
            free( infoP->lstNameP );
        }

        if( infoP->symNameP )
        {
            free( infoP->symNameP );
        }

        freeOrigins(infoP);
        free(infoP);
        files[i] = NIL;
    }

    numFiles = 0;
    memMapped = false;
    forgetSources();
    ovlClear();
    clearSymbols();     // the next file is number 0 again, so no old symbol may stay

    for( i = 0; i < MEMBANKS; ++i )
    {
        if( (bankP = memMap[i]) )
        {
            for( j = 0; j < MEMSIZE; ++j )
            {
                entryP = bankP[j];
                while( entryP )
                {
                    nextP = entryP->nextP;
                    free(entryP);
                    entryP = nextP;
                }
            }

            free( memMap[i] );
            memMap[i] = NIL;
        }
    }
}

// Is this listing line the %%endoverlay directive: "lineno: address", then the directive?
static bool
isEndOverlay(char *lineP)
{
char *cP;

    if( !(cP = strchr(lineP, ':')) )
    {
        return( false );
    }

    for( ++cP; (*cP == ' ') || ((*cP >= '0') && (*cP <= '7')); ++cP )
    {
        ;
    }

    return( !strncmp(cP, "%%endoverlay", 12) );
}

// Try to load line mappings from a source file.
// If it's a .lst file, we can construct the memory->line mapping also.
// Otherwise, all we have is the line number mapping.
// In a listing of a program with overlays, the lines from an overlay's heading,
// "// Overlay NAME, number N, ...", to its %%endoverlay line are tagged with N.
// Return true if something loaded, else false.
bool
loadFileMap(FileInfoP infoP)
{
int bank;
int address;
int numLines;
int overlay;
int number;
int lineOverlay;
bool fromLst;
long offset;
MapEntryP memP, newP;
MapEntryPP bankP;
char *cP, *cP2;
char tmpbuf[1024];
int *wordLinesP = NIL;      // the listing lines that hold a word, and their addresses, for the .map
int *wordAddrsP = NIL;
int numWords = 0;
int wordsSize = 0;
struct stat statBuf;

    if( !infoP->fP )
    {
        return(false);
    }

    // if the file is the list file, the name will have a ptr.
    fromLst = (infoP->lstNameP)?true:false;

    // read through the file keeping track of the offset of the start of each line.
    overlay = 0;
    for( numLines = 0; numLines < MAXLINES; )
    {
        offset = ftell(infoP->fP);
        infoP->lineMap[numLines++] = offset;

        if( !fgets(tmpbuf, sizeof(tmpbuf), infoP->fP) )
        {
            break;
        }

        if( fromLst && !strncmp(tmpbuf, "// Overlay ", 11) &&
            (sscanf(tmpbuf, "// Overlay %*[^,], number %d,", &number) == 1) )
        {
            overlay = number;
            continue;
        }

        // The %%endoverlay line is the overlay's own, the lines after it the root's.
        lineOverlay = overlay;
        if( overlay && isEndOverlay(tmpbuf) )
        {
            overlay = 0;
        }

        if( fromLst )
        {
            // Each word a line holds is noted for the .map, whose records pair with them in order.
            // A line can hold two: a label and the first word of a type340 string on the same line.
            for( cP = tmpbuf; *cP; ++cP )
            {
                if( ((cP == tmpbuf) || (cP[-1] == ' ')) && isWordEntry(cP, &address) )
                {
                    if( numWords >= wordsSize )
                    {
                        wordsSize = (wordsSize)?(wordsSize * 2):1024;
                        wordLinesP = (int *)realloc(wordLinesP, wordsSize * sizeof(int));
                        wordAddrsP = (int *)realloc(wordAddrsP, wordsSize * sizeof(int));
                    }

                    wordLinesP[numWords] = numLines;
                    wordAddrsP[numWords++] = address;
                }
            }

            // Pick up the address for building the memory map.
            // Line will be of the form:
            // [ ]+lineno: address (rest of line)
            // Line numbers are always decimal, addresses octal.
            // Any line not of the above form is ignored.
            // Note that the line number is that of the line in the original source,
            // not in the listing file.
            // So, we ignore it and use the current listing file line number.
            if( (cP = strchr(tmpbuf,':')) && (*(cP + 1) == ' ') && isdigit(*(cP + 2)) )
            {
                *cP++ = NUL;

                // cP now pointing to the space after the :
                address = strtol(cP + 1, &cP2, 8);

                if( *cP2 != ' ' )
                {
                    // Not a memory allocating line, skip
                    continue;
                }

                memMapped = true;                   // we have at least one
                bank = BANKOF(address);
                address = ADDRESSOF(address);
                if( (bank >= 0) && (bank < MEMBANKS) )
                {
                    if( !(bankP = memMap[bank]) )    // not allocated yet
                    {
                        bankP = (MapEntryPP)calloc(MEMSIZE, sizeof(MapEntryP));
                        memMap[bank] = bankP;
                        memMapped = true;
                    }

                    newP = (MapEntryP)calloc(1, sizeof(MapEntry));
                    newP->lineNo = numLines;
                    newP->fileNo = infoP->fileNo;
                    newP->overlay = lineOverlay;

                    if( !bankP[address] )
                    {
                        // First one
                        bankP[address] = newP;    // if multiple users of addr, take only the first
                    }
                    else
                    {
                        // Get to the end of the list
                        for( memP = bankP[address]; memP->nextP; memP = memP->nextP )
                        {
                            ;
                        }

                        memP->nextP = newP;
                    }
                }
            }
        }
    }
    
    infoP->numLines = numLines;

    // The origins, only when the file read is the .lst itself.
    // penSourceFile() uses the .am1 when there is no .lst, though the name is kept either way.
    if( fromLst && (stat(infoP->lstNameP, &statBuf) == 0) )
    {
        loadOrigins(infoP, wordLinesP, wordAddrsP, numWords);
    }

    free(wordLinesP);
    free(wordAddrsP);

    curLine = -1;
    return(true);
}

// Does a word's entry in the listing start here: "lineno: address word ", the line number
// decimal, the address and the word, six octal digits each.
// An entry with an address and no word, such as an origin, does not have a data word.
static bool
isWordEntry(char *lineP, int *addressP)
{
char *cP;
int i;

    cP = lineP;
    if( !isdigit((unsigned char)*cP) )
    {
        return( false );
    }

    while( isdigit((unsigned char)*cP) )
    {
        ++cP;
    }

    if( (cP[0] != ':') || (cP[1] != ' ') )
    {
        return( false );
    }

    cP += 2;
    for( i = 0; i < 14; ++i )
    {
        if( (i == 6) || (i == 13) )
        {
            if( cP[i] != ' ' )
            {
                return( false );
            }
        }
        else if( (cP[i] < '0') || (cP[i] > '7') )
        {
            return( false );
        }
    }

    *addressP = (int)strtol(cP, NIL, 8);
    return( true );
}

// Read the .map am1 wrote with the .lst, if there is one, and give each listing line the
// macro chain of its word.
// Its W and P records pair, in order, with the listing's word entries whose lines and addresses
// wordLinesP and wordAddrsP hold.
// If they do not pair address for address, the .map is not this listing's and nothing is taken from it.
static void
loadOrigins(FileInfoP infoP, int *wordLinesP, int *wordAddrsP, int numWords)
{
FILE *fP;
char *lineP = NIL;
size_t lineSize = 0;
char mapName[1024];
char *cP, *tokP, *endP;
char *whyP = NIL;
int recNo, fileNo, address, level, len, nameAt, n;
OriginP *originsPP;
OriginP originP;

    strncpy(mapName, infoP->lstNameP, sizeof(mapName) - 5);
    mapName[sizeof(mapName) - 5] = NUL;
    if( (cP = strrchr(mapName, '.')) && !strcmp(cP, ".lst") )
    {
        *cP = NUL;
    }
    strcat(mapName, ".map");

    if( !(fP = fopen(mapName, "r")) )
    {
        return;
    }

    originsPP = (OriginP *)calloc(infoP->numLines + 1, sizeof(OriginP));
    infoP->originsPP = originsPP;

    // The header: the label, the version, the source.
    if( (getline(&lineP, &lineSize, fP) < 0) || strcmp(lineP, "%%am1 origin map%%\n") ||
        (getline(&lineP, &lineSize, fP) < 0) || strcmp(lineP, "V1\n") ||
        (getline(&lineP, &lineSize, fP) < 0) )
    {
        whyP = "not an origin map of a version ad1 reads";
    }

    for( recNo = 0; !whyP && (getline(&lineP, &lineSize, fP) >= 0); )
    {
        if( (len = strlen(lineP)) && (lineP[len - 1] == '\n') )
        {
            lineP[--len] = NUL;
        }

        switch( lineP[0] )
        {
        case 'F':
            // F <n> <file>: the numbers come 0 up
            if( (sscanf(lineP, "F %d %n", &fileNo, &nameAt) != 1) || (fileNo != infoP->numMapFiles) ||
                (nameAt >= len) )
            {
                whyP = "a bad file record";
                break;
            }

            infoP->mapFilesPP = (char **)realloc(infoP->mapFilesPP, (fileNo + 1) * sizeof(char *));
            infoP->mapFilesPP[fileNo] = strdup(lineP + nameAt);
            ++infoP->numMapFiles;
            break;

        case 'P':
        case 'W':
            // W <address> <overlay> <file n> <line> [<macro> <file n> <line>]...
            // P <address>
            if( recNo >= numWords )
            {
                whyP = "more words than the listing";
                break;
            }

            tokP = strtok(lineP + 1, " ");
            address = (tokP)?(int)strtol(tokP, &endP, 8):-1;
            if( !tokP || *endP || (address != wordAddrsP[recNo]) )
            {
                whyP = "a word at an address the listing does not have there";
                break;
            }

            // the overlay, file and line: the listing shows them already
            for( n = 0; (lineP[0] == 'W') && (n < 3) && strtok(NIL, " "); ++n )
            {
                ;
            }

            if( (lineP[0] == 'W') && (n < 3) )
            {
                whyP = "a bad word record";
                break;
            }

            originP = NIL;
            for( level = 0; (lineP[0] == 'W') && (tokP = strtok(NIL, " ")); ++level )
            {
                if( !originP )
                {
                    originP = (OriginP)calloc(1, sizeof(Origin));
                }

                originP->levelsP = (OriginLevelP)realloc(originP->levelsP, (level + 1) * sizeof(OriginLevel));
                originP->levelsP[level].macroP = strdup(tokP);
                originP->count = level + 1;

                cP = strtok(NIL, " ");
                endP = strtok(NIL, " ");
                if( !cP || !endP )
                {
                    whyP = "a bad macro chain";
                    break;
                }

                fileNo = atoi(cP);
                originP->levelsP[level].fileP = ((fileNo >= 0) && (fileNo < infoP->numMapFiles))?
                    infoP->mapFilesPP[fileNo]:NIL;
                originP->levelsP[level].lineNo = atoi(endP);
            }

            // a line's first word with a chain gives the line its chain
            if( originP && !originsPP[wordLinesP[recNo]] )
            {
                originsPP[wordLinesP[recNo]] = originP;
            }
            else if( originP )
            {
                for( level = 0; level < originP->count; ++level )
                {
                    free(originP->levelsP[level].macroP);
                }

                free(originP->levelsP);
                free(originP);
            }

            ++recNo;
            break;

        case 'S':
            // the symbols' files: not used yet
            break;

        default:
            whyP = "a record ad1 does not know";
            break;
        }
    }

    if( !whyP && (recNo != numWords) )
    {
        whyP = "fewer words than the listing";
    }

    free(lineP);
    fclose(fP);

    if( whyP )
    {
        printf("Macro origins from '%s' not used: %s.\n", mapName, whyP);
        freeOrigins(infoP);
    }
}

// Free what loadOrigins() kept.
static void
freeOrigins(FileInfoP infoP)
{
int i, j;

    if( infoP->originsPP )
    {
        for( i = 0; i <= infoP->numLines; ++i )
        {
            if( infoP->originsPP[i] )
            {
                for( j = 0; j < infoP->originsPP[i]->count; ++j )
                {
                    free(infoP->originsPP[i]->levelsP[j].macroP);
                }

                free(infoP->originsPP[i]->levelsP);
                free(infoP->originsPP[i]);
            }
        }

        free(infoP->originsPP);
        infoP->originsPP = NIL;
    }

    for( i = 0; i < infoP->numMapFiles; ++i )
    {
        free(infoP->mapFilesPP[i]);
    }

    free(infoP->mapFilesPP);
    infoP->mapFilesPP = NIL;
    infoP->numMapFiles = 0;
}

// See if there ia a FileInfo for the given file number.
// If so, return it.
// If not, return null.
FileInfoP
getFileInfoP(int fileNo)
{
    if( (numFiles < 1) || (fileNo >= numFiles) || (fileNo < 0) )
    {
        return(NULL);
    }

    return( files[fileNo] );
}

// See if a rim or bin file can be found based on the filename in infoP.
// If so, copy the name into rsltP and return true.
// If not, return false.
// The result buffer is also used as a scratch buffer, ignore its contents unless true is returned.
bool
findRimFile(FileInfoP infoP, char *rsltP)
{
char *cP;
struct stat statBuf;

    strcpy(rsltP, infoP->am1NameP);
    cP = strchr(rsltP, '.') + 1;    // we know it's valid
    strcpy(cP,"rim");

    if( !stat(rsltP, &statBuf) )
    {
        return(true);
    }

    strcpy(cP,"bin");

    if( !stat(rsltP, &statBuf) )
    {
        return(true);
    }

    return(false);          // sorry
}
