/*
 * Overlays after the parse: each overlay's words, the drum layout, the
 * descriptor table and the build stamp, and the .ovl and .osym files.
 *
 * ovlPrepare() runs once, before any code generator, so that the tape, the
 * dump and the listing all take the table's words, the overlays' headers and
 * every overlay word's value from here.  An overlay word's value is saved on
 * its node, in value2.ival, as bincodegen.c saves a root word's; the listing
 * and the dump print it from there and do not evaluate it again, so a warning
 * an overlay word gives is given once.
 *
 * Docs/UsingOverlays.md describes the table, the header and the two files.
 * Am1Includes/DRUM/overlay.ah defines the same table and header words for the
 * overlay manager, by the names given in the comments below.
*/
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "am1.h"
#include "y.tab.h"

#define OVLFILELABEL    "%%am1 overlay file%%"
#define OVLFILEVERSION  "V2"
#define OSYMFILELABEL   "%%am1 overlay symtab file%%"
#define OSYMFILEVERSION "V1"

#define OVL_DRUMREAD    0400000     // the dia word's read bit, drmrd; OV_DIA_READ in overlay.ah
#define OVL_PERSISTENT  0400000     // set in an entry's resident-word address; OV_PERSIST
#define OVL_TWOPASS     0200000     // and for an overlay of a twopass window; OV_TWOPASS
#define OVL_WORDSPERLINE 8          // words on a line of the .ovl file

extern int lineno;
extern WindowP windowsP;
extern OverlayContextP overlaysP;
extern uint32_t drumReserved;

extern int evalExpr(PNodeP);
extern void verror(const char *msgP, ...);
extern void verrorlf(int lineNo, const char *fileNameP, const char *msgP, ...);

Word ovStamp;                   // the build stamp, in the table and every header
Word *ovTableWordsP;            // the table's words, OVTABLE node's value.ival of them

static int windowCount;
static PNodeP ovTableNodeP;

static void makeImages(PNodeP nodeP);
static void putWord(PNodeP nodeP, int addr, Word value);
static bool isWord(PNodeP nodeP);
static void imageText(PNodeP nodeP, FlexText flexText);
static void imageAscii(PNodeP nodeP, char *strP);
static int imageConstants(PNodeP nodeP, int addr, SymNodeP symP);
static void imageVars(PNodeListP listP);
static void layOut(void);
static bool isSwapWindow(WindowP winP);
static int windowOverlays(WindowP winP);
static bool spanFree(int track, int drum, int span, WindowP winP);
static int endsOn(int *triesP, int track);
static void makeTable(void);
static Word stampOf(void);
static Word hashWord(Word hash, Word word);
static void printOsymTree(FILE *outfP, SymNodeP symP, const char *ovNameP);

// Make every overlay's words, lay the overlays out on the drum, and make the
// table and the stamp.  stmtsP is the program's statement list.  Called only
// when the program has overlays; any error is fatal.
void
ovlPrepare(PNodeP stmtsP)
{
OverlayContextP ovP;
WindowP winP;

    for( windowCount = 0, winP = windowsP; winP; winP = winP->nextP )
    {
        ++windowCount;
    }

    // Each image holds the whole overlay, header included; the header is
    // filled in once the stamp is known.
    for( ovP = overlaysP; ovP; ovP = ovP->nextP )
    {
        ovP->length = ovP->ctx.cur_pc - ovP->windowP->first;

        if( !(ovP->imageP = (Word *)calloc(ovP->length, sizeof(Word))) )
        {
            verror("out of memory making overlay %s", ovP->nameP);
        }
    }

    makeImages(stmtsP);

    if( !ovTableNodeP )
    {
        // The parse refuses overlays with no %%ovtable; this is that rule's
        // backstop, since the table's words would have nowhere to go.
        verror("internal error: overlays and no %%%%ovtable node");
    }

    layOut();
    makeTable();
    ovStamp = stampOf();
    ovTableWordsP[0] = ovStamp;

    for( ovP = overlaysP; ovP; ovP = ovP->nextP )
    {
        ovP->imageP[0] = ovStamp;           // OV_HDR_STAMP
        ovP->imageP[1] = ovP->number;       // OV_HDR_NUMBER
    }
}

// Walk the statements, putting each overlay word into its overlay's image and
// saving its value on its node; the root's words are bincodegen.c's.
static void
makeImages(PNodeP nodeP)
{
int i;
Word value;
OverlayContextP ovP;

    for( ; nodeP; nodeP = nodeP->leftP )
    {
        if( nodeP->type == OVTABLE )
        {
            ovTableNodeP = nodeP;
        }

        if( !nodeP->overlay )
        {
            continue;
        }

        // evalExpr() reports an undefined name at lineno.
        lineno = nodeP->lineNo;

        switch( nodeP->type )
        {
        case EXPR:
        case LOCATION:
        case LCLLOCATION:
            if( isWord(nodeP->rightP) )
            {
                value = evalExpr(nodeP->rightP);
                nodeP->value2.ival = value;
                putWord(nodeP, nodeP->pc, value);
            }
            else if( nodeP->rightP )
            {
                // A label with a string on its line.
                switch( nodeP->rightP->type )
                {
                case TEXT:
                case TYPE340:
                    imageText(nodeP->rightP, nodeP->rightP->value.flexText);
                    break;

                case ASCII:
                    imageAscii(nodeP->rightP, nodeP->rightP->value.strP);
                    break;

                default:
                    break;
                }
            }
            break;

        case TEXT:
        case TYPE340:
            imageText(nodeP, nodeP->value.flexText);
            break;

        case ASCII:
            imageAscii(nodeP, nodeP->value.strP);
            break;

        case TABLE:
            // A table without an initializer is zeros, as the image starts.
            if( nodeP->rightP )
            {
                value = evalExpr(nodeP->rightP);

                for( i = 0; i < nodeP->value.ival; ++i )
                {
                    putWord(nodeP, nodeP->pc + i, value);
                }
            }
            break;

        case CONSTANTS:
            imageConstants(nodeP, nodeP->pc, nodeP->value.symP);
            break;

        case VARS:
            imageVars((PNodeListP)(nodeP->value.ptr));
            break;

        case ENDOVERLAY:
            // The pools %%endoverlay placed, constants first.
            ovP = (OverlayContextP)(nodeP->value.ptr);

            if( ovP->ctx.constSymP )
            {
                imageConstants(nodeP, ovP->ctx.constPC, ovP->ctx.constSymP);
            }

            imageVars(ovP->ctx.varNodesP);
            break;

        default:
            break;
        }
    }
}

// Put one word into the image of nodeP's overlay, at core address addr of its
// window.  The parse kept every overlay word inside its window past the header,
// so a word elsewhere is am1's own error.
static void
putWord(PNodeP nodeP, int addr, Word value)
{
int index;
OverlayContextP ovP;

    for( ovP = overlaysP; ovP && (ovP->number != nodeP->overlay); ovP = ovP->nextP )
    {
        ;
    }

    if( !ovP )
    {
        verror("internal error: a word of overlay number %d, which does not exist", nodeP->overlay);
    }

    index = addr - ovP->windowP->first;

    if( (index < OVHEADERWORDS) || (index >= ovP->length) )
    {
        verror("internal error: overlay %s's word at %04o is outside it, %04o-%04o",
            ovP->nameP, addr, ovP->windowP->first + OVHEADERWORDS, ovP->windowP->first + ovP->length - 1);
    }

    ovP->imageP[index] = value & WRDMASK;
}

// Whether a statement's operand is one word, as bincodegen.c's canReduce() has
// it: not a directive and not a string.
static bool
isWord(PNodeP nodeP)
{
    if( !nodeP )
    {
        return(false);
    }

    switch( nodeP->type )
    {
    case ORIGIN:
    case LOCAL:
    case PRIVATE:
    case ADDLOCAL:
    case ENDLOC:
    case FORCELOC:
    case TERMINATOR:
    case TEXT:
    case ASCII:
    case TYPE340:
        return(false);

    case EXPR:
    case SEPARATOR:
        return( isWord(nodeP->rightP) );

    default:
        return(true);
    }
}

// Packed flexo code, three 6-bit characters to a word, as bincodegen.c's
// writeText() packs it, its accumulator's high bits masked off as the tape
// writer drops them.  nodeP's pc is the first word's address.
static void
imageText(PNodeP nodeP, FlexText flexText)
{
int i;
int addr;
Word val;
char *bufP;

    bufP = flexText.bufP;
    addr = nodeP->pc;

    for( val = 0, i = 0; i < flexText.nchars; i++ )
    {
        if( i && !(i % 3) )
        {
            putWord(nodeP, addr++, val);
        }

        val <<= 6;
        val |= *bufP++;
    }

    if( i % 3 )         // leftovers: finish the word
    {
        while( i++ % 3 )
        {
            val <<= 6;
        }

        putWord(nodeP, addr, val);
    }
    else
    {
        putWord(nodeP, addr, val);
    }
}

// Packed ascii, two 9-bit characters to a word, the terminating NUL included,
// as bincodegen.c's writeAscii() packs it.  nodeP's pc is the first word's
// address.
static void
imageAscii(PNodeP nodeP, char *strP)
{
int i;
int addr;
Word word;

    addr = nodeP->pc;
    word = 0;
    i = 0;              // 0 for the high character, 1 for the low

    do
    {
        if( !i )
        {
            word = *strP;
        }
        else
        {
            word = (word << 9) | *strP;
            putWord(nodeP, addr++, word);
        }

        i ^= 1;
    }
    while( *strP++ );

    if( i )             // a lone last character: a zero low byte
    {
        putWord(nodeP, addr, word << 9);
    }
}

// A constant pool from addr, in the order the code generators emit it: the
// node, then its left and right subtrees.  nodeP gives the overlay.
// Returns the address after the pool's last word.
static int
imageConstants(PNodeP nodeP, int addr, SymNodeP symP)
{
    if( !symP )
    {
        return(addr);
    }

    putWord(nodeP, addr++, symP->value2);
    addr = imageConstants(nodeP, addr, symP->leftP);
    return( imageConstants(nodeP, addr, symP->rightP) );
}

// A variable pool, each variable at the address setVarsPC() gave its node,
// holding its initial value.
static void
imageVars(PNodeListP listP)
{
PNodeP nodeP;

    for( ; listP; listP = listP->nextP )
    {
        nodeP = listP->nodeP;
        putWord(nodeP, nodeP->pc, (nodeP->leftP)?evalExpr(nodeP->leftP):0);
    }
}

// Place every overlay on the drum: its track, its drum address and its span, the
// words of the track that are its own (DESIGN-AM1-OVERLAYS.md section 6).  First
// fit, so the same source gives the same layout, never on a track %%drumreserve
// names and never across a track's end.
// 1. Each swap window, in source order: its overlays at one drum address D, each on
//    a track of its own, since a swap reads one and writes another at one address.
//    A persistent overlay's span is the window's longest overlay's length, the
//    swap's count; a read-only one's is its own length.  D is the lowest of 0 and
//    the ends of the spans already placed at which all of them fit.
// 2. Every other overlay, longest first, ties in source order: its span its own
//    length, at the lowest free drum address on the lowest track that has room.
static void
layOut(void)
{
int i;
int j;
int n;
int tries;
int track;
int drum;
int longest;
int *triesP;
bool fits;
WindowP winP;
OverlayContextP ovP;
OverlayContextP failedP;
OverlayContextP *orderPP;

    for( n = 0, ovP = overlaysP; ovP; ovP = ovP->nextP )
    {
        ovP->track = -1;
        ++n;
    }

    // 0 and the end of each span: every drum address a placement tries.
    if( !(triesP = (int *)calloc(n + 1, sizeof(int))) ||
        !(orderPP = (OverlayContextP *)calloc(n, sizeof(OverlayContextP))) )
    {
        verror("out of memory laying out the overlays");
    }

    for( winP = windowsP; winP; winP = winP->nextP )
    {
        if( !isSwapWindow(winP) )
        {
            continue;
        }

        for( longest = 0, ovP = overlaysP; ovP; ovP = ovP->nextP )
        {
            if( (ovP->windowP == winP) && (ovP->length > longest) )
            {
                longest = ovP->length;
            }
        }

        for( ovP = overlaysP; ovP; ovP = ovP->nextP )
        {
            if( ovP->windowP == winP )
            {
                ovP->span = (ovP->persistent)?longest:ovP->length;
            }
        }

        tries = endsOn(triesP, -1);
        fits = false;
        failedP = NILP;

        for( i = 0; (i < tries) && !fits; ++i )
        {
            drum = triesP[i];
            fits = true;

            for( ovP = overlaysP; ovP && fits; ovP = ovP->nextP )
            {
                if( ovP->windowP != winP )
                {
                    continue;
                }

                // spanFree() keeps a track the window holds at this D from another of its overlays.
                for( track = 0; (track < DRUMTRACKS) && !spanFree(track, drum, ovP->span, winP); ++track )
                {
                    ;
                }

                if( track < DRUMTRACKS )
                {
                    ovP->track = track;
                    ovP->drum = drum;
                }
                else
                {
                    fits = false;
                    failedP = ovP;
                }
            }

            if( !fits )
            {
                // Not at this D, so none of the window's overlays is placed.
                for( ovP = overlaysP; ovP; ovP = ovP->nextP )
                {
                    if( ovP->windowP == winP )
                    {
                        ovP->track = -1;
                    }
                }
            }
        }

        if( !fits )
        {
            verrorlf(failedP->lineNo, failedP->fileP,
                "overlay %s has nowhere on the drum: window %s's %d overlays share one drum address, each on a"
                " track of its own, and at no drum address do they find room, its span of %o words among them,"
                " on tracks %%%%drumreserve leaves free",
                failedP->nameP, winP->nameP, windowOverlays(winP), failedP->span);
        }
    }

    // Longest first, ties in source order: an insertion sort keeps them.
    for( n = 0, ovP = overlaysP; ovP; ovP = ovP->nextP )
    {
        if( isSwapWindow(ovP->windowP) )
        {
            continue;
        }

        ovP->span = ovP->length;

        for( i = n++; (i > 0) && (orderPP[i - 1]->length < ovP->length); --i )
        {
            orderPP[i] = orderPP[i - 1];
        }

        orderPP[i] = ovP;
    }

    for( i = 0; i < n; ++i )
    {
        ovP = orderPP[i];

        // A free run begins at 0 or where a span ends, so the first of those that
        // fits is the lowest free drum address.
        for( drum = -1, track = 0; track < DRUMTRACKS; ++track )
        {
            tries = endsOn(triesP, track);

            for( j = 0; (j < tries) && !spanFree(track, triesP[j], ovP->span, NILP); ++j )
            {
                ;
            }

            if( j < tries )
            {
                drum = triesP[j];
                break;
            }
        }

        if( drum < 0 )
        {
            verrorlf(ovP->lineNo, ovP->fileP,
                "overlay %s has nowhere on the drum: no track %%%%drumreserve leaves free has its span of %o"
                " words free",
                ovP->nameP, ovP->span);
        }

        ovP->track = track;
        ovP->drum = drum;
    }

    free(triesP);
    free(orderPP);
}

// Whether a window's overlays are swapped: it has a persistent overlay and is not twopass.
static bool
isSwapWindow(WindowP winP)
{
OverlayContextP ovP;

    if( winP->twopass )
    {
        return(false);
    }

    for( ovP = overlaysP; ovP; ovP = ovP->nextP )
    {
        if( (ovP->windowP == winP) && ovP->persistent )
        {
            return(true);
        }
    }

    return(false);
}

// How many overlays a window has.
static int
windowOverlays(WindowP winP)
{
int n;
OverlayContextP ovP;

    for( n = 0, ovP = overlaysP; ovP; ovP = ovP->nextP )
    {
        if( ovP->windowP == winP )
        {
            ++n;
        }
    }

    return(n);
}

// Whether span words from drum address drum are free on a track: inside the track, on
// one %%drumreserve leaves free, and clear of every span placed.  With winP, a track
// that holds one of that window's overlays is not free either.
static bool
spanFree(int track, int drum, int span, WindowP winP)
{
OverlayContextP ovP;

    if( (drumReserved & (UINT32_C(1) << track)) || ((drum + span) > BANKSIZE) )
    {
        return(false);
    }

    for( ovP = overlaysP; ovP; ovP = ovP->nextP )
    {
        if( ovP->track != track )
        {
            continue;
        }

        if( (winP && (ovP->windowP == winP)) ||
            ((ovP->drum < (drum + span)) && (drum < (ovP->drum + ovP->span))) )
        {
            return(false);
        }
    }

    return(true);
}

// 0 and the end of every span placed on a track, or on any track when track is -1,
// ascending and each once, into triesP, which has room for one more than there are
// overlays.  Returns how many.
static int
endsOn(int *triesP, int track)
{
int i;
int n;
int end;
OverlayContextP ovP;

    triesP[0] = 0;
    n = 1;

    for( ovP = overlaysP; ovP; ovP = ovP->nextP )
    {
        if( (ovP->track < 0) || ((track >= 0) && (ovP->track != track)) )
        {
            continue;
        }

        end = ovP->drum + ovP->span;

        for( i = n; triesP[i - 1] > end; --i )
        {
            ;
        }

        if( triesP[i - 1] == end )
        {
            continue;
        }

        memmove(&triesP[i + 1], &triesP[i], (n - i) * sizeof(int));
        triesP[i] = end;
        ++n;
    }

    return(n);
}

// The table's words but the stamp, which is worked out over them: one word per
// window, the number of its resident overlay, 0 for none, then four per
// overlay.
static void
makeTable(void)
{
int i;
int tableAddr;
OverlayContextP ovP;

    if( !(ovTableWordsP = (Word *)calloc(ovTableNodeP->value.ival, sizeof(Word))) )
    {
        verror("out of memory making the overlay table");
    }

    tableAddr = (ovTableNodeP->bank << 12) | ovTableNodeP->pc;
    i = 1 + windowCount;                // past the stamp and the windows' words, from OV_TBL_WINDOWS

    for( ovP = overlaysP; ovP; ovP = ovP->nextP )
    {
        // OV_ENT_DIA: a read from the overlay's track, at its drum address.
        ovTableWordsP[i++] = OVL_DRUMREAD | (ovP->track << 12) | ovP->drum;
        // OV_ENT_DWC: the overlay's length, header included; a whole-bank
        // window's 4096 is 0, a whole track, as dwc takes it.
        ovTableWordsP[i++] = ovP->length & ADDRMASK;
        // OV_ENT_DCL: the window's first core address.
        ovTableWordsP[i++] = (ovP->windowP->bank << 12) | ovP->windowP->first;
        // OV_ENT_RES: the full address of the window's resident word, the
        // persistent bit, and the two-pass bit, from which the manager chooses how
        // to write the overlay back.
        ovTableWordsP[i++] = (tableAddr + 1 + ovP->windowP->index) | ((ovP->persistent)?OVL_PERSISTENT:0) |
            ((ovP->windowP->twopass)?OVL_TWOPASS:0);
    }
}

// The build stamp: a hash over every overlay's words, the header excluded, in
// overlay order, then the table's words after the stamp.  Each word w makes the
// hash h into (31h + w + 1) mod 2^18, so any one word changed changes the stamp
// (31 is odd, so its powers are invertible mod 2^18), and so does a word added
// or removed.  A stamp of 0 is made 1, so that a header read from a drum never
// written, all zeros, never matches.
static Word
stampOf(void)
{
int i;
Word hash;
OverlayContextP ovP;

    hash = 0;

    for( ovP = overlaysP; ovP; ovP = ovP->nextP )
    {
        for( i = OVHEADERWORDS; i < ovP->length; ++i )
        {
            hash = hashWord(hash, ovP->imageP[i]);
        }
    }

    for( i = 1; i < ovTableNodeP->value.ival; ++i )
    {
        hash = hashWord(hash, ovTableWordsP[i]);
    }

    return( (hash)?hash:1 );
}

// One step of the stamp's hash.
static Word
hashWord(Word hash, Word word)
{
    return( ((hash * 31) + word + 1) & WRDMASK );
}

// Print the drum layout, one line per overlay, each line begun with prefixP,
// for the listing ("// ") and -S ("").
void
ovlPrintLayout(FILE *outfP, const char *prefixP)
{
OverlayContextP ovP;

    fprintf(outfP, "%sOverlays on the drum, stamp %06o:\n", prefixP, ovStamp);

    for( ovP = overlaysP; ovP; ovP = ovP->nextP )
    {
        fprintf(outfP, "%soverlay %s, number %d: track %o, drum %04o-%04o, window %s in bank %d, %o words, %s\n",
            prefixP, ovP->nameP, ovP->number, ovP->track, ovP->drum, ovP->drum + ovP->span - 1,
            ovP->windowP->nameP, ovP->windowP->bank, ovP->length,
            (!ovP->persistent)?"read-only":((ovP->windowP->twopass)?"persistent, two-pass":"persistent"));
    }
}

// What the table word at index holds, for the listing: whose word it is and
// which.
void
ovlTableWordName(int index, char *bufP, size_t size)
{
int i;
WindowP winP;
OverlayContextP ovP;
static const char *entryNamesP[OVENTRYWORDS] = { "dia, read", "dwc, length", "dcl, core", "resident word" };

    if( index == 0 )
    {
        snprintf(bufP, size, "the build stamp");
        return;
    }

    for( i = 1, winP = windowsP; winP; winP = winP->nextP, ++i )
    {
        if( i == index )
        {
            snprintf(bufP, size, "window %s: its resident overlay, 0 for none", winP->nameP);
            return;
        }
    }

    for( ovP = overlaysP; ovP; ovP = ovP->nextP, i += OVENTRYWORDS )
    {
        if( index < (i + OVENTRYWORDS) )
        {
            snprintf(bufP, size, "overlay %s: %s", ovP->nameP, entryNamesP[index - i]);
            return;
        }
    }

    snprintf(bufP, size, "past the table's end");
}

// Write the .ovl file: a heading, whose stamp line gives the table's full address
// so that a loader can find the stamp on the tape, then each overlay's line and
// its words, the header included, in octal, OVL_WORDSPERLINE to a line.  The
// loader puts each on its track at its drum address and pads the rest of its span
// with zeros.  sourceP is the source's name, without its directory.
void
ovlWriteOvl(FILE *outfP, const char *sourceP)
{
int i;
OverlayContextP ovP;

    fprintf(outfP, "%s\n", OVLFILELABEL);
    fprintf(outfP, "%s\n", OVLFILEVERSION);
    fprintf(outfP, "%s stamp %06o table %06o\n", sourceP, ovStamp,
        (ovTableNodeP->bank << 12) | ovTableNodeP->pc);

    for( ovP = overlaysP; ovP; ovP = ovP->nextP )
    {
        fprintf(outfP, "overlay %s window %s bank %o first %o last %o track %o drum %o span %o length %o mode %s\n",
            ovP->nameP, ovP->windowP->nameP, ovP->windowP->bank, ovP->windowP->first,
            ovP->windowP->last, ovP->track, ovP->drum, ovP->span, ovP->length, (ovP->persistent)?"rw":"ro");

        for( i = 0; i < ovP->length; ++i )
        {
            fprintf(outfP, "%06o%s", ovP->imageP[i],
                (((i + 1) % OVL_WORDSPERLINE) && ((i + 1) < ovP->length))?" ":"\n");
        }
    }
}

// Write the .osym file: the overlays' own symbols in the .sym file's line form,
// "aaaaaa t name line", with the overlay's name after it.  An overlay's names for
// its bank's root globals are the root's, in the .sym file.  filenameP is the
// name the .sym file gives.
void
ovlWriteOsym(FILE *outfP, const char *filenameP)
{
OverlayContextP ovP;

    fprintf(outfP, "%s\n", OSYMFILELABEL);
    fprintf(outfP, "%s\n", OSYMFILEVERSION);
    fprintf(outfP, "%s\n", filenameP);

    for( ovP = overlaysP; ovP; ovP = ovP->nextP )
    {
        printOsymTree(outfP, ovP->ctx.globalSymP, ovP->nameP);
    }
}

// One overlay's symbols, in name order as the .sym file has them.
static void
printOsymTree(FILE *outfP, SymNodeP symP, const char *ovNameP)
{
    if( !symP )
    {
        return;
    }

    printOsymTree(outfP, symP->leftP, ovNameP);

    if( (symP->flags & SYMF_RESOLVED) && !(symP->flags & SYMF_OVALIAS) )
    {
        fprintf(outfP, "%06o %s %s %d %s\n", (symP->bank << 12) + symP->value,
            (symP->flags & SYMF_EXPORTED)?"X":"G", symP->name, symP->lineno, ovNameP);
    }

    printOsymTree(outfP, symP->rightP, ovNameP);
}
