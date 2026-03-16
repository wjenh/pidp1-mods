// Process various combinations of filenames and try to set up the list/source and symbol data.

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

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

extern bool loadSymbols(FileInfoP infoP);

// Have we mapped a file and is it still open?
bool
isFileMapped(int fileNo)
{
FileInfoP infoP;

    if( fileNo >= numFiles )
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
// Infer the ohter names from whatever is given.
// Create a FileInfo and set the names in it.
// The allocated names are set in the passed arguments if not null.
// Returns the FileInfoP for all cases other than a nil or empty nameP, return nil in that case.
FileInfoP
resolveFiles(char *nameP)
{
char *extP;
char *cP;
FileInfoP infoP;

char name[256];
char am1str[256];
char lststr[256];
char symstr[256];

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
    else if( !strcmp(extP, ".rim") )
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
// Clear the name of the one not opened, if set.
// Return true if succesful, else false.
bool
openSourceFile(FileInfoP infoP)
{
    if( infoP->fP )
    {
        return(true);           // already open
    }

    if( infoP->lstNameP && (infoP->fP = fopen(infoP->lstNameP, "r")) )
    {
        if( infoP->am1NameP )
        {
            free(infoP->am1NameP);
        }

        infoP->am1NameP = NIL;
        return( true );
    }

    if( infoP->am1NameP && (infoP->fP = fopen(infoP->am1NameP, "r")) )
    {
        if( infoP->lstNameP )
        {
            free(infoP->lstNameP);
        }

        infoP->lstNameP = NIL;
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

        free(infoP);
        files[i] = NIL;
    }

    numFiles = 0;
    memMapped = false;

    for( i = 0; i < MEMBANKS; ++i )
    {
        if( (bankP = memMap[i]) )
        {
            for( entryP  = *bankP++, j = 0; j < MEMSIZE; ++j )
            {
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

// Try to load line mappings from a source file.
// If it's a .lst file, we can construct the memory->line mapping also.
// Otherwise, all we have is the line number mapping.
// Return true if something loaded, else false.
bool
loadFileMap(FileInfoP infoP)
{
int i;
int bank;
int address;
int numLines;
bool fromLst;
long offset;
MapEntryP memP, newP;
MapEntryPP bankP;
char *cP, *cP2;
char tmpbuf[1024];

    if( !infoP->fP )
    {
        return(false);
    }
    
    // if the file is the list file, the name will have a ptr.
    fromLst = (infoP->lstNameP)?true:false;

    // read through the file keeping track of the offset of the start of each line.
    for( numLines = 0; numLines < MAXLINES; )
    {
        offset = ftell(infoP->fP);
        infoP->lineMap[numLines++] = offset;

        if( !fgets(tmpbuf, sizeof(tmpbuf), infoP->fP) )
        {
            break;
        }

        if( fromLst )
        {
            // Pick up the address for building the memory map.
            // Line will be of the form:
            // [ ]+lineno: address (rest of line)
            // Line numbers are always decimal, addresses octal.
            // Any line not of the above form is ignored.
            // Note that the line number is that of the line in the original source,
            // not in the listing file.
            // So, we ignore it and use the current listing file line number.
            if( (cP = strchr(tmpbuf,':')) )
            {
                *cP++ = NUL;

                // cP now pointing to the space after the :
                address = strtol(cP, &cP2, 8);

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

                    if( !bankP[address] )
                    {
                        // First one
                        bankP[address] = newP;    // if muliple users of addr, take only the first
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

    curLine = -1;
    return(true);
}
