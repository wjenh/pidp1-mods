// Drum overlays: what am1's .ovl and .osym files say about a program's overlays, and which
// overlay each window holds now.
//
// The .ovl gives the windows, the overlays in source order (their numbers, from 1), the build
// stamp and the address of the overlay manager's table; the .osym gives the overlays' symbols.
// Which overlay is in a window is the manager's resident word for it.  Its address is not in
// the .ovl, but the table in core holds it, in the last word of each overlay's entry.
// ad1 reads core live whether or not the machine runs, so the resident words and the stamp are
// read when a command first needs them and forgotten before the next one.
// A breakpoint or watch bound to an overlay is kept here too.  An emulator with conditions
// holds the binding as one, the window's resident word holding the overlay's number, and keeps
// the count; with one from before conditions, a bound breakpoint is an ordinary one at count 1
// and the count is kept here.
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ad1.h"

#define OVLHEADER       "%%am1 overlay file%%"
#define OSYMHEADER      "%%am1 overlay symtab file%%"
#define OVLVERSION      "V1"

#define ENTRYWORDS      4           // words in an overlay's table entry
#define ENTRYRESIDENT   3           // the entry word holding its window's resident-word address
#define PERSISTENT      0400000     // set in that word for a persistent overlay
#define ADDRMASK        0177777

typedef struct {
    char *nameP;
    u32 first;                  // full addresses
    u32 last;
    u32 residentAddr;           // for this command: its resident word, 0 if not found
    int resident;               // for this command: the overlay in it, 0 for none
} Window, *WindowP;

typedef struct {
    char *nameP;
    int window;                 // index in the file's windows
} Overlay, *OverlayP;           // its number is its index plus one

typedef struct {
    int numWindows;
    int numOverlays;
    WindowP windowsP;
    OverlayP overlaysP;
    u32 table;                  // the table's full address, its first word the stamp
    u32 stamp;
    bool read;                  // the resident words have been read for this command
    bool stampOk;               // for this command: the stamp in core is the .ovl's
    u32 coreStamp;              // for this command: the stamp in core
    bool warned;                // the stamp in core differs and has been reported
    u32 warnedStamp;            // the stamp reported
} FileOverlays, *FileOverlaysP;

// One per file, indexed by the file's number, as its symbols are.
static FileOverlays fileOverlays[MAXFILES];

// A breakpoint or watch bound to an overlay, by its number less one.
typedef struct {
    bool bound;
    bool inEmu;                 // the emulator holds it as a condition, and keeps the count
    int fileNo;
    int overlay;
    int count;                  // without inEmu: fires on every count-th pass of its overlay
    int curCount;               // without inEmu: passes of its overlay since it last fired
    bool stampSaid;             // the stamp in core differs and has been reported for it
    u32 saidStamp;              // the stamp reported
    char name[64];              // the symbol it was set by, empty if none
    u32 nameAddr;               // that symbol's address
} Binding, *BindingP;

static Binding boundBreaks[AD1_NUM_BREAKPOINTS];
static Binding boundWatches[AD1_NUM_WATCHES];

// The name@overlay references of the command being parsed.
static OverlayRef commandRef;

static bool readOvl(FileOverlaysP foP, FILE *fP);
static bool readOsym(int fileNo, FILE *fP);
static void freeOverlays(FileOverlaysP foP);
static void readResidency(int fileNo);
static void bind(BindingP bP, OverlayRefP refP, bool inEmu);
static char *boundName(BindingP bP);
static bool stampSays(BindingP bP, const char *whatP, int n);
static void adopt(int fileNo);
static int findWindow(FileOverlaysP foP, char *nameP);
static char *dupString(const char *sP);
static void dropNewline(char *lineP);

// Read name.ovl and name.osym beside the file's name.sym.  Neither there: nothing to do.  Only
// one, or either malformed: say so, and the file has no overlays here.
void
ovlLoad(FileInfoP infoP)
{
FileOverlaysP foP;
FILE *ovlFP;
FILE *osymFP;
char *extP;
char ovlName[1024];
char osymName[1024];

    if( !infoP->symNameP || (infoP->fileNo < 0) || (infoP->fileNo >= MAXFILES) ||
        (strlen(infoP->symNameP) >= (sizeof(ovlName) - 2)) )
    {
        return;
    }

    foP = &fileOverlays[infoP->fileNo];
    freeOverlays(foP);
    clearOverlaySymbols(infoP->fileNo);

    // resolveFiles() always ends the name in ".sym"
    strcpy(ovlName, infoP->symNameP);
    if( !(extP = strrchr(ovlName, '.')) )
    {
        return;
    }

    strcpy(extP, ".ovl");
    strcpy(osymName, ovlName);
    strcpy(osymName + (extP - ovlName), ".osym");

    ovlFP = fopen(ovlName, "r");
    osymFP = fopen(osymName, "r");

    if( !ovlFP && !osymFP )
    {
        return;
    }

    if( !ovlFP || !osymFP )
    {
        printf("Found '%s' but not '%s', no overlay symbols will be available.\n",
            (ovlFP)?ovlName:osymName, (ovlFP)?osymName:ovlName);
    }
    else if( !readOvl(foP, ovlFP) )
    {
        printf("File '%s' is not a valid overlay file, no overlay symbols will be available.\n", ovlName);
        freeOverlays(foP);
    }
    else if( !readOsym(infoP->fileNo, osymFP) )
    {
        printf("File '%s' is not a valid overlay symbol file, no overlay symbols will be available.\n",
            osymName);
        freeOverlays(foP);
        clearOverlaySymbols(infoP->fileNo);
    }
    else
    {
        printf("Loaded %d overlays in %d windows from '%s'.\n", foP->numOverlays, foP->numWindows,
            ovlName);
        adopt(infoP->fileNo);
    }

    if( ovlFP )
    {
        fclose(ovlFP);
    }

    if( osymFP )
    {
        fclose(osymFP);
    }
}

// Forget every file's overlays, and their symbols.
void
ovlClear(void)
{
int i;

    for( i = 0; i < MAXFILES; ++i )
    {
        freeOverlays(&fileOverlays[i]);
    }

    clearOverlaySymbols(NOARG);
}

// A new command: what was read from core may have changed.
void
ovlForget(void)
{
int i;

    for( i = 0; i < MAXFILES; ++i )
    {
        fileOverlays[i].read = false;
    }
}

// Is this overlay of this file in its window?  The root always is.
bool
ovlSymbolShown(int fileNo, int overlay)
{
FileOverlaysP foP;

    if( overlay == 0 )
    {
        return( true );
    }

    if( (fileNo < 0) || (fileNo >= MAXFILES) || (overlay > fileOverlays[fileNo].numOverlays) )
    {
        return( false );
    }

    foP = &fileOverlays[fileNo];
    readResidency(fileNo);
    return( foP->windowsP[foP->overlaysP[overlay - 1].window].resident == overlay );
}

// Is a listing line of this overlay to be shown?  It is when its overlay is in its window, and
// when none is, so that a window with nothing in it lists every overlay's line, as before ad1
// knew overlays.
bool
ovlLineShown(int fileNo, int overlay)
{
FileOverlaysP foP;
int resident;

    if( (overlay == 0) || (fileNo < 0) || (fileNo >= MAXFILES) ||
        (overlay > fileOverlays[fileNo].numOverlays) )
    {
        return( true );
    }

    foP = &fileOverlays[fileNo];
    readResidency(fileNo);
    resident = foP->windowsP[foP->overlaysP[overlay - 1].window].resident;
    return( (resident == 0) || (resident == overlay) );
}

// The number of a file's overlay of this name, 0 if it has none.
int
ovlNumber(int fileNo, char *nameP)
{
FileOverlaysP foP;
int i;

    if( (fileNo < 0) || (fileNo >= MAXFILES) )
    {
        return( 0 );
    }

    foP = &fileOverlays[fileNo];
    for( i = 0; i < foP->numOverlays; ++i )
    {
        if( !strcmp(foP->overlaysP[i].nameP, nameP) )
        {
            return( i + 1 );
        }
    }

    return( 0 );
}

// The name of a file's overlay, nil if there is no such overlay.
char *
ovlName(int fileNo, int overlay)
{
    if( (fileNo < 0) || (fileNo >= MAXFILES) || (overlay < 1) ||
        (overlay > fileOverlays[fileNo].numOverlays) )
    {
        return( NIL );
    }

    return( fileOverlays[fileNo].overlaysP[overlay - 1].nameP );
}

// The name of the overlay in the window holding this address, nil if the address is in no
// window or the window holds none.
char *
ovlResidentNameAt(u32 address)
{
FileOverlaysP foP;
WindowP winP;
int i;
int w;

    for( i = 0; i < MAXFILES; ++i )
    {
        foP = &fileOverlays[i];
        for( w = 0; w < foP->numWindows; ++w )
        {
            winP = &foP->windowsP[w];
            if( (address >= winP->first) && (address <= winP->last) )
            {
                readResidency(i);
                if( winP->resident )
                {
                    return( foP->overlaysP[winP->resident - 1].nameP );
                }
            }
        }
    }

    return( NIL );
}

// A new command: it has named no overlay yet.
void
ovlRefClear(void)
{
    memset(&commandRef, 0, sizeof(commandRef));
}

// The command's address names an overlay's symbol, name@overlay.
void
ovlRefNote(SymbolP symP)
{
    if( (commandRef.count > 0) &&
        ((commandRef.fileNo != symP->fileNo) || (commandRef.overlay != symP->overlay)) )
    {
        commandRef.mixed = true;
    }

    if( commandRef.count++ == 0 )
    {
        commandRef.fileNo = symP->fileNo;
        commandRef.overlay = symP->overlay;
        commandRef.address = symP->address;
        commandRef.nameP = symP->nameP;
    }
}

// The overlays the command has named, count 0 if none.
OverlayRefP
ovlRef(void)
{
    return( &commandRef );
}

// The condition that binds to the overlay refP names: its window's resident word holds its
// number.  The word's address is read from the table in core, so the program must be there.
// Returns nil, or why there is no condition.
const char *
ovlCondFor(OverlayRefP refP, Ad1Cond *condP)
{
FileOverlaysP foP;
WindowP winP;

    if( (refP->fileNo < 0) || (refP->fileNo >= MAXFILES) ||
        (refP->overlay < 1) || (refP->overlay > fileOverlays[refP->fileNo].numOverlays) )
    {
        return( "no such overlay" );
    }

    foP = &fileOverlays[refP->fileNo];
    readResidency(refP->fileNo);
    winP = &foP->windowsP[foP->overlaysP[refP->overlay - 1].window];
    if( !foP->stampOk || !winP->residentAddr )
    {
        return( "the program in core is not the one its overlay files describe" );
    }

    condP->isSet = 1;
    condP->address = winP->residentAddr;
    condP->mask = 0777777;
    condP->value = (u32)refP->overlay;
    return( NIL );
}

static void
bind(BindingP bP, OverlayRefP refP, bool inEmu)
{
    memset(bP, 0, sizeof(Binding));
    bP->bound = true;
    bP->inEmu = inEmu;
    bP->fileNo = refP->fileNo;
    bP->overlay = refP->overlay;
    if( refP->nameP && (strlen(refP->nameP) < sizeof(bP->name)) )
    {
        strcpy(bP->name, refP->nameP);
        bP->nameAddr = refP->address;
    }
}

// Bind a breakpoint to the overlay refP names.  inEmu: the emulator holds it as a condition;
// otherwise it is at count 1 there, and fires here on every count-th pass of its overlay.
void
ovlBindBreak(int number, OverlayRefP refP, int count, bool inEmu)
{
BindingP bP;

    if( (number < 1) || (number > AD1_NUM_BREAKPOINTS) )
    {
        return;
    }

    bP = &boundBreaks[number - 1];
    bind(bP, refP, inEmu);
    bP->count = (count < 1)?1:count;        // 0 is 1, as the emulator has it
}

// Forget a breakpoint's binding, 0 for every one.
void
ovlUnbindBreak(int number)
{
    if( number == 0 )
    {
        memset(boundBreaks, 0, sizeof(boundBreaks));
    }
    else if( (number >= 1) && (number <= AD1_NUM_BREAKPOINTS) )
    {
        memset(&boundBreaks[number - 1], 0, sizeof(Binding));
    }
}

// A watch is bound only as a condition the emulator holds.
void
ovlBindWatch(int number, OverlayRefP refP)
{
    if( (number >= 1) && (number <= AD1_NUM_WATCHES) )
    {
        bind(&boundWatches[number - 1], refP, true);
    }
}

// Forget a watch's binding, 0 for every one.
void
ovlUnbindWatch(int number)
{
    if( number == 0 )
    {
        memset(boundWatches, 0, sizeof(boundWatches));
    }
    else if( (number >= 1) && (number <= AD1_NUM_WATCHES) )
    {
        memset(&boundWatches[number - 1], 0, sizeof(Binding));
    }
}

static char *
boundName(BindingP bP)
{
char *ovNameP;

    if( !(ovNameP = ovlName(bP->fileNo, bP->overlay)) )
    {
        ovNameP = "?";
    }

    return( ovNameP );
}

// For the breakpoint listing, the rest of a bound breakpoint's line: its symbol and overlay, or
// its address, the count and the overlay.  The count is the emulator's, count and curCount,
// when it holds the binding, else ad1's.  Returns false, printing nothing, if the breakpoint is
// not bound.
bool
ovlPrintBinding(int number, u32 address, bool enabled, int count, int curCount)
{
BindingP bP;

    if( (number < 1) || (number > AD1_NUM_BREAKPOINTS) || !boundBreaks[number - 1].bound )
    {
        return( false );
    }

    bP = &boundBreaks[number - 1];
    if( bP->name[0] && (bP->nameAddr == address) )
    {
        printf("%s@%s", bP->name, boundName(bP));
    }
    else
    {
        printf("%06o", address);
    }

    if( !bP->inEmu )
    {
        count = bP->count;
        curCount = bP->curCount;
    }

    printf(" ,count %d ,currently %d, %s, bound to overlay %s\n", (count < 1)?1:count, curCount,
        (enabled)?"enabled":"disabled", boundName(bP));
    return( true );
}

// For the watch listing: a bound watch's address as name@overlay when it is at the symbol.
// Returns false, printing nothing, if it is not bound or not at its symbol.
bool
ovlPrintWatchAddress(int number, u32 address)
{
BindingP bP;

    if( (number < 1) || (number > AD1_NUM_WATCHES) || !boundWatches[number - 1].bound )
    {
        return( false );
    }

    bP = &boundWatches[number - 1];
    if( !bP->name[0] || (bP->nameAddr != address) )
    {
        return( false );
    }

    printf("%s@%s", bP->name, boundName(bP));
    return( true );
}

// The overlay a watch is bound to, nil if none.
char *
ovlWatchOverlay(int number)
{
    if( (number < 1) || (number > AD1_NUM_WATCHES) || !boundWatches[number - 1].bound )
    {
        return( NIL );
    }

    return( boundName(&boundWatches[number - 1]) );
}

// With the stamp in core not the files', a bound breakpoint or watch cannot fire: say so, once
// until the stamp changes, and return true.  Residency must have been read for its file.
static bool
stampSays(BindingP bP, const char *whatP, int n)
{
FileOverlaysP foP;

    foP = &fileOverlays[bP->fileNo];
    if( foP->stampOk )
    {
        bP->stampSaid = false;
        return( false );
    }

    if( !bP->stampSaid || (bP->saidStamp != foP->coreStamp) )
    {
        printf("%s %d, bound to overlay %s, cannot fire while the program in core "
            "is not the one file %d's overlay files describe.\n", whatP, n, boundName(bP),
            bP->fileNo + 1);
        bP->stampSaid = true;
        bP->saidStamp = foP->coreStamp;
    }

    return( true );
}

// A hit of watch number: false if it is bound and must not fire, the stamp in core not being
// the files'; *saidP is set if that was printed now.  The emulator holds the condition, so
// otherwise a hit is always in the bound overlay.
bool
ovlWatchHit(int number, bool *saidP)
{
BindingP bP;
bool wasSaid;

    *saidP = false;
    if( (number < 1) || (number > AD1_NUM_WATCHES) || !boundWatches[number - 1].bound )
    {
        return( true );
    }

    bP = &boundWatches[number - 1];
    readResidency(bP->fileNo);
    wasSaid = bP->stampSaid && (bP->saidStamp == fileOverlays[bP->fileNo].coreStamp);
    if( stampSays(bP, "Watch", number) )
    {
        *saidP = !wasSaid;
        return( false );
    }

    return( true );
}

// After a file's overlays are read: a breakpoint or watch the emulator holds with a condition
// that is one of this file's overlays' (an earlier ad1 set it and left with exit) is bound
// again, so it lists, and goes with the files, as one set here.
static void
adopt(int fileNo)
{
Ad1Cond bpConds[AD1_NUM_BREAKPOINTS];
Ad1Cond watchConds[AD1_NUM_WATCHES];
Ad1BpEntry bps[AD1_NUM_BREAKPOINTS];
Ad1WatchEntry watches[AD1_NUM_WATCHES];
FileOverlaysP foP;
OverlayRef ref;
BindingP bP;
const Ad1Cond *cP;
u32 address;
bool isSet;
int i;
int w;

    if( !tgtHasConds() || (tgtCondList(bpConds, watchConds) != AD1P_ST_OK) ||
        (tgtBpList(bps) != AD1P_ST_OK) || (tgtWatchList(watches) != AD1P_ST_OK) )
    {
        return;
    }

    foP = &fileOverlays[fileNo];
    readResidency(fileNo);
    for( i = 0; i < (AD1_NUM_BREAKPOINTS + AD1_NUM_WATCHES); ++i )
    {
        if( i < AD1_NUM_BREAKPOINTS )
        {
            cP = &bpConds[i];
            bP = &boundBreaks[i];
            isSet = bps[i].isSet;
            address = bps[i].address;
        }
        else
        {
            cP = &watchConds[i - AD1_NUM_BREAKPOINTS];
            bP = &boundWatches[i - AD1_NUM_BREAKPOINTS];
            isSet = watches[i - AD1_NUM_BREAKPOINTS].isSet;
            address = watches[i - AD1_NUM_BREAKPOINTS].address;
        }

        if( !isSet || !cP->isSet || bP->bound || (cP->mask != 0777777) ||
            (cP->value < 1) || (cP->value > (u32)foP->numOverlays) )
        {
            continue;
        }

        w = foP->overlaysP[cP->value - 1].window;
        if( !foP->windowsP[w].residentAddr || (foP->windowsP[w].residentAddr != cP->address) )
        {
            continue;
        }

        memset(&ref, 0, sizeof(ref));
        ref.count = 1;
        ref.fileNo = fileNo;
        ref.overlay = (int)cP->value;
        ref.address = address;
        bind(bP, &ref, true);           // no symbol: it lists as its address
        printf("%s %d is bound to overlay %s.\n", (i < AD1_NUM_BREAKPOINTS)?"Breakpoint":"Watch",
            (i < AD1_NUM_BREAKPOINTS)?(i + 1):(i - AD1_NUM_BREAKPOINTS + 1), boundName(bP));
    }

    // Read again for the next command, which may come after the program has run on.
    foP->read = false;
}

// A hit of breakpoint number at this address.  A bound breakpoint the emulator holds as a
// condition fired there in its overlay with its count reached, so it fires here when it is the
// one the emulator named, unless the stamp in core is not the files'.  Without conditions, each
// enabled bound breakpoint here counts the pass when its overlay is in its window, and fires when
// its count is reached: the emulator names only the first breakpoint at an address, and two
// overlays' entries often share one, so every one here is taken, whichever the emulator named.
// The one it named, if not bound, fires as it always has.  Returns false if no bound breakpoint
// is here.
bool
ovlBreakHit(int number, u32 address, BoundHitP hitP)
{
Ad1BpEntry bps[AD1_NUM_BREAKPOINTS];
BindingP bbP;
char *residentP;
char why[96];
bool wasSaid;
bool any;
int i;
int n;

    memset(hitP, 0, sizeof(BoundHit));
    if( tgtBpList(bps) != AD1P_ST_OK )
    {
        return( false );
    }

    any = false;
    for( i = 0; i < AD1_NUM_BREAKPOINTS; ++i )
    {
        n = (int)bps[i].number;
        if( !bps[i].isSet || !bps[i].isEnabled || (bps[i].address != (address & ADDRMASK)) ||
            (n < 1) || (n > AD1_NUM_BREAKPOINTS) || !boundBreaks[n - 1].bound ||
            (boundBreaks[n - 1].fileNo < 0) || (boundBreaks[n - 1].fileNo >= MAXFILES) )
        {
            continue;
        }

        bbP = &boundBreaks[n - 1];
        if( bbP->inEmu && (n != number) )
        {
            // Its condition or count did not stop the machine here, so the pass is not its.
            continue;
        }

        any = true;
        readResidency(bbP->fileNo);
        wasSaid = bbP->stampSaid && (bbP->saidStamp == fileOverlays[bbP->fileNo].coreStamp);
        if( stampSays(bbP, "Breakpoint", n) )
        {
            hitP->said = hitP->said || !wasSaid;
            snprintf(why, sizeof(why), "the program in core is not the one its overlay files "
                "describe");
        }
        else if( bbP->inEmu )
        {
            hitP->fires[n - 1] = true;
            continue;
        }
        else if( ovlSymbolShown(bbP->fileNo, bbP->overlay) )
        {
            if( ++bbP->curCount >= bbP->count )
            {
                bbP->curCount = 0;
                hitP->fires[n - 1] = true;
                continue;
            }

            snprintf(why, sizeof(why), "pass %d of %d", bbP->curCount, bbP->count);
        }
        else if( (residentP = ovlResidentNameAt(address)) )
        {
            snprintf(why, sizeof(why), "overlay %s is in the window", residentP);
        }
        else
        {
            snprintf(why, sizeof(why), "no overlay is in the window");
        }

        if( !hitP->unfired )
        {
            hitP->unfired = n;
            snprintf(hitP->why, sizeof(hitP->why), "bound to overlay %s, %s", boundName(bbP), why);
        }
    }

    if( any && (number >= 1) && (number <= AD1_NUM_BREAKPOINTS) && !boundBreaks[number - 1].bound )
    {
        hitP->fires[number - 1] = true;
    }

    return( any );
}

// The files are being closed, and their overlays with them, so the breakpoints and watches bound
// to an overlay are deleted: left as addresses, they would fire for every overlay.
void
ovlBreaksClosed(void)
{
int n;

    for( n = 1; n <= AD1_NUM_BREAKPOINTS; ++n )
    {
        if( boundBreaks[n - 1].bound )
        {
            tgtBpDelete((uint32_t)n);
            printf("Breakpoint %d, bound to overlay %s, is deleted with the files.\n", n,
                boundName(&boundBreaks[n - 1]));
            ovlUnbindBreak(n);
        }
    }

    for( n = 1; n <= AD1_NUM_WATCHES; ++n )
    {
        if( boundWatches[n - 1].bound )
        {
            tgtWatchDelete((uint32_t)n);
            printf("Watch %d, bound to overlay %s, is deleted with the files.\n", n,
                boundName(&boundWatches[n - 1]));
            ovlUnbindWatch(n);
        }
    }
}

// exit leaves the emulator's breakpoints, disabled.  One the emulator holds as a condition stays
// bound, and a later ad1 with the same files finds it; one bound here goes with ad1.
void
ovlBreaksLeft(void)
{
int n;

    for( n = 1; n <= AD1_NUM_BREAKPOINTS; ++n )
    {
        if( boundBreaks[n - 1].bound && !boundBreaks[n - 1].inEmu )
        {
            printf("Breakpoint %d was bound to overlay %s; it is left as an address breakpoint at "
                "count 1, disabled, which fires for every overlay if enabled again.\n", n,
                boundName(&boundBreaks[n - 1]));
        }
    }
}

// The .ovl: its header, the stamp line, and an "overlay" line for each overlay, in source
// order.  The lines of words between them are the loader's, not ours.
static bool
readOvl(FileOverlaysP foP, FILE *fP)
{
OverlayP ovP;
WindowP winP;
int w;
unsigned int stamp, table, bank, first, last;
char line[1024];
char ovName[256];
char winName[256];

    if( !fgets(line, sizeof(line), fP) || (dropNewline(line), strcmp(line, OVLHEADER)) ||
        !fgets(line, sizeof(line), fP) || (dropNewline(line), strcmp(line, OVLVERSION)) ||
        !fgets(line, sizeof(line), fP) ||
        (sscanf(line, "%*s stamp %o table %o", &stamp, &table) != 2) )
    {
        return( false );
    }

    foP->stamp = stamp;
    foP->table = table & ADDRMASK;

    while( fgets(line, sizeof(line), fP) )
    {
        if( strncmp(line, "overlay ", 8) )
        {
            continue;
        }

        if( sscanf(line, "overlay %255s window %255s bank %o first %o last %o",
                ovName, winName, &bank, &first, &last) != 5 )
        {
            return( false );
        }

        first = FULLADDR(bank, first);
        last = FULLADDR(bank, last);

        if( (w = findWindow(foP, winName)) < 0 )
        {
            if( !(winP = realloc(foP->windowsP, (foP->numWindows + 1) * sizeof(Window))) )
            {
                return( false );
            }

            foP->windowsP = winP;
            w = foP->numWindows++;
            winP = &foP->windowsP[w];
            memset(winP, 0, sizeof(Window));
            winP->nameP = dupString(winName);
            winP->first = first;
            winP->last = last;
        }
        else if( (foP->windowsP[w].first != first) || (foP->windowsP[w].last != last) )
        {
            return( false );
        }

        if( !(ovP = realloc(foP->overlaysP, (foP->numOverlays + 1) * sizeof(Overlay))) )
        {
            return( false );
        }

        foP->overlaysP = ovP;
        ovP = &foP->overlaysP[foP->numOverlays++];
        ovP->nameP = dupString(ovName);
        ovP->window = w;
    }

    return( foP->numOverlays > 0 );
}

// The .osym: its header, then "aaaaaa t name line overlay" for each symbol.  Every line is
// checked before any symbol is added, so a bad file adds none.
static bool
readOsym(int fileNo, FILE *fP)
{
long start;
int number;
int pass;
unsigned int address;
char type;
char line[1024];
char name[256];
char ovName[256];

    if( !fgets(line, sizeof(line), fP) || (dropNewline(line), strcmp(line, OSYMHEADER)) ||
        !fgets(line, sizeof(line), fP) || (dropNewline(line), strcmp(line, OVLVERSION)) ||
        !fgets(line, sizeof(line), fP) )
    {
        return( false );
    }

    start = ftell(fP);

    // The first pass checks, the second adds.
    for( pass = 0; pass < 2; ++pass )
    {
        fseek(fP, start, SEEK_SET);
        while( fgets(line, sizeof(line), fP) )
        {
            if( (sscanf(line, "%o %c %255s %*d %255s", &address, &type, name, ovName) != 4) ||
                !strchr("GX", type) || (address > ADDRMASK) ||
                !(number = ovlNumber(fileNo, ovName)) )
            {
                return( false );
            }

            if( (pass == 1) && !addSymbol(address, fileNo, number, name) )
            {
                return( false );
            }
        }
    }

    return( true );
}

// Read the table in core, once per command: the stamp, then each overlay's resident-word
// address, then each window's resident word.  A stamp that is not the .ovl's means the files
// are not the program in core, said once until the stamp changes; then, and when the table's
// addresses are not this program's, every window holds none.
static void
readResidency(int fileNo)
{
FileOverlaysP foP;
WindowP winP;
u32 stamp;
u32 address;
u32 value;
int n;
int w;

    foP = &fileOverlays[fileNo];
    if( foP->read )
    {
        return;
    }

    foP->read = true;
    for( w = 0; w < foP->numWindows; ++w )
    {
        foP->windowsP[w].residentAddr = 0;
        foP->windowsP[w].resident = 0;
    }

    stamp = tgtRead(foP->table);
    foP->coreStamp = stamp;
    foP->stampOk = (stamp == foP->stamp);
    if( !foP->stampOk )
    {
        if( !foP->warned || (foP->warnedStamp != stamp) )
        {
            printf("The program in core is not the one file %d's overlay files describe "
                "(stamp %06o, expected %06o): no overlay names are shown.\n", fileNo + 1, stamp,
                foP->stamp);
            foP->warned = true;
            foP->warnedStamp = stamp;
        }

        return;
    }

    foP->warned = false;

    for( n = 1; n <= foP->numOverlays; ++n )
    {
        address = tgtRead(foP->table + 1 + foP->numWindows + (ENTRYWORDS * (n - 1)) + ENTRYRESIDENT);
        address &= (ADDRMASK & ~PERSISTENT);
        winP = &foP->windowsP[foP->overlaysP[n - 1].window];

        if( (address < (foP->table + 1)) || (address > (foP->table + foP->numWindows)) ||
            (winP->residentAddr && (winP->residentAddr != address)) )
        {
            for( w = 0; w < foP->numWindows; ++w )
            {
                foP->windowsP[w].residentAddr = 0;
            }

            return;
        }

        winP->residentAddr = address;
    }

    // The manager zeros a resident word while it loads, so 0, or a number that is not one of
    // this window's overlays, is none.
    for( w = 0; w < foP->numWindows; ++w )
    {
        winP = &foP->windowsP[w];
        if( winP->residentAddr )
        {
            value = tgtRead(winP->residentAddr);
            if( (value >= 1) && (value <= (u32)foP->numOverlays) &&
                (foP->overlaysP[value - 1].window == w) )
            {
                winP->resident = (int)value;
            }
        }
    }
}

// The index of a file's window of this name, -1 if none.
static int
findWindow(FileOverlaysP foP, char *nameP)
{
int w;

    for( w = 0; w < foP->numWindows; ++w )
    {
        if( !strcmp(foP->windowsP[w].nameP, nameP) )
        {
            return( w );
        }
    }

    return( -1 );
}

// Free a file's windows and overlays, leaving it with none.
static void
freeOverlays(FileOverlaysP foP)
{
int i;

    for( i = 0; i < foP->numWindows; ++i )
    {
        free(foP->windowsP[i].nameP);
    }

    for( i = 0; i < foP->numOverlays; ++i )
    {
        free(foP->overlaysP[i].nameP);
    }

    free(foP->windowsP);
    free(foP->overlaysP);
    memset(foP, 0, sizeof(FileOverlays));
}

// A copy of the string in new memory, nil if there is none to be had.
static char *
dupString(const char *sP)
{
char *cP;

    if( (cP = malloc(strlen(sP) + 1)) )
    {
        strcpy(cP, sP);
    }

    return( cP );
}

// End the line at its newline, if it has one.
static void
dropNewline(char *lineP)
{
char *cP;

    if( (cP = strchr(lineP, '\n')) )
    {
        *cP = NUL;
    }
}
