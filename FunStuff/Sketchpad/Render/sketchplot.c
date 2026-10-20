/*
 * sketchplot -- draw a Sketchpad picture saved on a Microtape as a PNG file.
 *
 * Reads one picture (P1 to P16) from a Type 550 Microtape image file, checks it as Sketchpad's
 * Load does, and draws what Sketchpad's screen shows of it after a load, in Locate's mode at
 * x1 with Show and Forces off: the lines, the arcs, the instances, and the texts and numbers'
 * digits, in the Type 340's characters. The arcs are drawn true, where the screen shows them
 * as chords, at most 1.6 units off at the largest radius. A damaged image's counts and indices
 * are held to what the picture holds, so it draws nothing that is not there. The PNG is one
 * bit deep, written with stored deflate blocks, so no library is needed. The picture's layout
 * is Sketchpad's tape module's (sptape.ac); the image file's is UsingType550Microtape.md's
 * ("The image file").
 *
 * Usage: sketchplot [-d drive] [-p picture] [-t image] [-s size] [-i] out.png
 *        sketchplot [-d drive] [-p picture] [-t image] --dump
 * A drive's image file is its line in /opt/pidp1-mods/microtapes.txt.
 *
 * 15-Oct-2026, Claude: written.
 * 18-Oct-2026, Claude: tape format 14, sixteen pictures, an instance's size at any scale.
 * 18-Oct-2026, Claude: damaged images refused or drawn safely.
 * 18-Oct-2026, Claude: production cleanup, comments only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <getopt.h>

#define MOUNTLIST "/opt/pidp1-mods/microtapes.txt"
#define TAPEDIR "/opt/pidp1-mods/Microtapes/"
#define LOCKED ",locked"

#define WORDMASK 0777777
#define BLKWORDS 258
#define TAPEBLOCKS 576
#define NDRIVES 8

// The picture on the tape, format 14, the one the load accepts, its header's words, and the
// stream's words and blocks. Picture p (0 to 15) has a slot of TPSLOT blocks, P1 to P8 from
// block 1, P9 to P16 from TPDIRBLK + 1, the directory's block between them; the picture is one
// stream over the first TPPBLOCKS blocks of its slot, the header first and last.
#define TPMAGIC 0525253
#define TPFORMAT 016
#define TPSTREAM 8151
#define TPHDRWORDS 4
#define TPPBLOCKS 32
#define TPSLOT 35
#define TPHALF 8
#define TPDIRBLK (1 + (TPHALF * TPSLOT))
#define NPICTURES 16
#define NAMEW 9

// Each drawn part's first word in the stream (sptape.ac, tpSaveRuns), and the trailing header's.
// The constraint table and the ties lie between the header and the name, the cycle's settings
// and the masters' constraints between the name and the texts; nothing here draws them.
#define SNAME 485
#define STEXTS 890
#define SDIGITS 2336
#define SPICTURE 2561
#define STRAILER ((TPPBLOCKS * 256) - TPHDRWORDS)

// Sketchpad's table sizes, and the largest radius it gives an arc.
#define MAXPTS 256
#define MAXLINES 264
#define MAXINST 32
#define NMASTER 4
#define MWORDS 128
#define MAXARCS 19
#define NTEXT 32
#define NMTEXT 32
#define TXRUN 17
#define NSCAL 32
#define NOPOINT 0777777
#define ARCRMAX 1294

// The picture's runs (sptape.ac, TPPICTURE): each table's first word from SPICTURE, in order.
#define SPOINTX 0
#define SPOINTY 256
#define SLINEFROM 512
#define SLINETO 776
#define SIX 1040
#define SIY 1072
#define SISCALE 1104
#define SBANK2 1136
#define SLINECOUNT 1332
#define SPOINTCOUNT 1333
#define SMASTERS 1334

// Bank 2's run, the master directory, the instances' masters, the arc table.
#define B2MLEN 0
#define B2INSTCOUNT 48
#define B2IMASTER 49
#define B2ARCCOUNT 81
#define B2ARCTC 82

// The texts' run (TPTEXTS), from STEXTS.
#define TXN 0
#define MTN 1
#define TXX 6
#define TXY 38
#define TXSC 70
#define TXCH 102
#define TXW 134
#define TXRUNAT 166
#define MTM 710
#define MTDX 742
#define MTDY 774
#define MTSC 806
#define MTCH 838
#define MTW 870
#define MTRUNAT 902
#define TPTWORDS 1446

// The digits' runs (TPDIGITS), from SDIGITS.
#define DTXAN 0
#define DMTAN 129
#define DMTVAL 161
#define TPSWORDS 225

// The 340's vector words, character codes, and a character's size at x1.
#define VEND 0400000
#define VVISIBLE 0200000
#define VDOWN 0100000
#define VLEFT 0200
#define CUP 035
#define CLOW 036
#define CEND 037
#define CMINUS 055
#define CZERO 060
#define CHADV 7
#define CHHIGH 7
#define GLYPHROWS 9
#define GLYPHTOP 6

// What Sketchpad shows of the texts, at most TEXTBUDGET characters in all, each text counting
// its characters and TXOWN more; scales up to x8.
#define TEXTBUDGET 180
#define TXOWN 2
#define MAXSCALE 3
#define MAXDIGITS 99999

// An instance's scale word on the tape. Its low two bits are the 340's scale k, the sixteen
// above them a multiplier code c, the instance's size 2^k times c / 2^15, c = 0 meaning exactly
// 2^k.
// Sketchpad saves c = 0; a size between the powers of two is for a later Sketchpad.
#define SCALEK 3
#define SCALECSHIFT 2
#define SCALECUNIT 32768.0
#define SCREEN 1024

// Results of the check, as Sketchpad's Load reports them.
#define PICOK 0
#define PICEMPTY 1
#define PICBAD 2

#define MINSIZE 16
#define MAXSIZE 8192
#define DEFSIZE 1024
#define STOREDMAX 65535

// A picture's tables, decoded.
typedef struct
{
    int format;
    int words;
    int generation;
    char name[NAMEW * 3 + 1];
    int pointCount;
    int pointX[MAXPTS];
    int pointY[MAXPTS];
    int lineCount;
    int lineFrom[MAXLINES];
    int lineTo[MAXLINES];
    int arcCount;
    int arcCenter[MAXARCS];
    int arcStart[MAXARCS];
    int arcEnd[MAXARCS];
    int arcDir[MAXARCS];
    int arcSteps[MAXARCS];
    int arcR32[MAXARCS];
    int instCount;
    int instMaster[MAXINST];
    int instX[MAXINST];
    int instY[MAXINST];
    uint32_t instScale[MAXINST];
    int masterLen[NMASTER];
    uint32_t masters[NMASTER * MWORDS];
    int txN;
    int txX[NTEXT];
    int txY[NTEXT];
    int txSc[NTEXT];
    int txCh[NTEXT];
    int txW[NTEXT];
    int txAn[NTEXT];
    uint32_t txRun[NTEXT * TXRUN];
    int mtN;
    int mtM[NMTEXT];
    int mtDX[NMTEXT];
    int mtDY[NMTEXT];
    int mtSc[NMTEXT];
    int mtCh[NMTEXT];
    int mtW[NMTEXT];
    int mtAn[NMTEXT];
    int mtVal[NMTEXT];
    uint32_t mtRun[NMTEXT * TXRUN];
} Picture;

typedef Picture *PictureP;

// The 340's characters, captured from the emulator's display. For set 1 (upper shift) and set
// 2 (lower shift), each code's dots, a row a word from the top (y = 6) down to y = -2, bit 4
// the leftmost column (x = 0). Every drawn character advances CHADV; the shifts and the end
// code draw nothing.
static const uint8_t glyphs[2][64][GLYPHROWS] =
{
    {
        { 000, 016, 025, 033, 025, 016, 000, 000, 000 },    // 00 blob
        { 016, 021, 021, 037, 021, 021, 021, 000, 000 },    // 01 A
        { 036, 021, 021, 036, 021, 021, 036, 000, 000 },    // 02 B
        { 016, 021, 020, 020, 020, 021, 016, 000, 000 },    // 03 C
        { 036, 021, 021, 021, 021, 021, 036, 000, 000 },    // 04 D
        { 037, 020, 020, 037, 020, 020, 037, 000, 000 },    // 05 E
        { 037, 020, 020, 037, 020, 020, 020, 000, 000 },    // 06 F
        { 016, 021, 020, 027, 021, 021, 016, 000, 000 },    // 07 G
        { 021, 021, 021, 037, 021, 021, 021, 000, 000 },    // 10 H
        { 016, 004, 004, 004, 004, 004, 016, 000, 000 },    // 11 I
        { 001, 001, 001, 001, 001, 021, 016, 000, 000 },    // 12 J
        { 021, 022, 024, 030, 024, 022, 021, 000, 000 },    // 13 K
        { 020, 020, 020, 020, 020, 020, 037, 000, 000 },    // 14 L
        { 021, 033, 025, 021, 021, 021, 021, 000, 000 },    // 15 M
        { 021, 031, 025, 023, 021, 021, 021, 000, 000 },    // 16 N
        { 016, 021, 021, 021, 021, 021, 016, 000, 000 },    // 17 O
        { 036, 021, 021, 036, 020, 020, 020, 000, 000 },    // 20 P
        { 016, 021, 021, 021, 025, 023, 017, 000, 000 },    // 21 Q
        { 036, 021, 021, 036, 024, 022, 021, 000, 000 },    // 22 R
        { 016, 021, 020, 016, 001, 021, 016, 000, 000 },    // 23 S
        { 037, 004, 004, 004, 004, 004, 004, 000, 000 },    // 24 T
        { 021, 021, 021, 021, 021, 021, 016, 000, 000 },    // 25 U
        { 021, 021, 021, 021, 021, 012, 004, 000, 000 },    // 26 V
        { 021, 021, 021, 021, 025, 033, 021, 000, 000 },    // 27 W
        { 021, 012, 004, 004, 004, 012, 021, 000, 000 },    // 30 X
        { 021, 012, 004, 004, 004, 004, 004, 000, 000 },    // 31 Y
        { 037, 001, 002, 037, 010, 020, 037, 000, 000 },    // 32 Z
        { 000, 000, 000, 000, 000, 000, 000, 000, 000 },    // 33 not drawn
        { 000, 000, 000, 000, 000, 000, 000, 000, 000 },    // 34 not drawn
        { 000, 000, 000, 000, 000, 000, 000, 000, 000 },    // 35 not drawn
        { 000, 000, 000, 000, 000, 000, 000, 000, 000 },    // 36 not drawn
        { 000, 000, 000, 000, 000, 000, 000, 000, 000 },    // 37 not drawn
        { 000, 000, 000, 000, 000, 000, 000, 000, 000 },    // 40 space
        { 004, 004, 004, 004, 004, 000, 004, 000, 000 },    // 41 !
        { 012, 012, 012, 000, 000, 000, 000, 000, 000 },    // 42 "
        { 012, 012, 037, 012, 037, 012, 012, 000, 000 },    // 43 #
        { 016, 025, 024, 016, 005, 025, 016, 000, 000 },    // 44 $
        { 037, 031, 002, 004, 010, 023, 023, 000, 000 },    // 45 %
        { 010, 024, 024, 010, 025, 026, 015, 000, 000 },    // 46 &
        { 006, 006, 002, 000, 000, 000, 000, 000, 000 },    // 47 '
        { 002, 004, 010, 010, 010, 004, 002, 000, 000 },    // 50 (
        { 010, 004, 002, 002, 002, 004, 010, 000, 000 },    // 51 )
        { 000, 025, 016, 004, 016, 025, 000, 000, 000 },    // 52 *
        { 000, 004, 004, 037, 004, 004, 000, 000, 000 },    // 53 +
        { 000, 000, 000, 014, 014, 004, 010, 000, 000 },    // 54 ,
        { 000, 000, 000, 037, 000, 000, 000, 000, 000 },    // 55 -
        { 000, 000, 000, 000, 000, 014, 014, 000, 000 },    // 56 .
        { 000, 001, 002, 004, 010, 020, 000, 000, 000 },    // 57 /
        { 016, 021, 023, 025, 031, 021, 016, 000, 000 },    // 60 0
        { 004, 014, 004, 004, 004, 004, 016, 000, 000 },    // 61 1
        { 016, 021, 001, 016, 020, 020, 037, 000, 000 },    // 62 2
        { 016, 021, 001, 006, 001, 021, 016, 000, 000 },    // 63 3
        { 002, 006, 012, 037, 002, 002, 002, 000, 000 },    // 64 4
        { 037, 020, 020, 016, 001, 021, 016, 000, 000 },    // 65 5
        { 016, 021, 020, 036, 021, 021, 016, 000, 000 },    // 66 6
        { 037, 021, 002, 004, 010, 020, 020, 000, 000 },    // 67 7
        { 016, 021, 021, 016, 021, 021, 016, 000, 000 },    // 70 8
        { 016, 021, 021, 017, 001, 021, 016, 000, 000 },    // 71 9
        { 000, 000, 014, 014, 000, 014, 014, 000, 000 },    // 72 :
        { 014, 014, 000, 014, 014, 004, 010, 000, 000 },    // 73 ;
        { 002, 004, 010, 020, 010, 004, 002, 000, 000 },    // 74 <
        { 000, 000, 037, 000, 037, 000, 000, 000, 000 },    // 75 =
        { 010, 004, 002, 001, 002, 004, 010, 000, 000 },    // 76 >
        { 016, 021, 001, 006, 004, 004, 004, 000, 000 },    // 77 ?
    },
    {
        { 000, 016, 025, 033, 025, 016, 000, 000, 000 },    // 00 blob
        { 000, 000, 016, 022, 022, 022, 015, 000, 000 },    // 01 a
        { 020, 020, 036, 021, 021, 021, 036, 000, 000 },    // 02 b
        { 000, 000, 016, 021, 020, 021, 016, 000, 000 },    // 03 c
        { 001, 001, 017, 021, 021, 021, 017, 000, 000 },    // 04 d
        { 000, 000, 016, 021, 037, 020, 016, 000, 000 },    // 05 e
        { 006, 011, 010, 034, 010, 010, 010, 000, 000 },    // 06 f
        { 000, 000, 016, 021, 021, 021, 017, 001, 016 },    // 07 g
        { 020, 020, 036, 021, 021, 021, 021, 000, 000 },    // 10 h
        { 004, 000, 014, 004, 004, 004, 016, 000, 000 },    // 11 i
        { 002, 000, 006, 002, 002, 012, 004, 000, 000 },    // 12 j
        { 020, 020, 022, 024, 034, 022, 021, 000, 000 },    // 13 k
        { 014, 004, 004, 004, 004, 004, 016, 000, 000 },    // 14 l
        { 000, 000, 032, 025, 025, 025, 025, 000, 000 },    // 15 m
        { 000, 000, 026, 031, 021, 021, 021, 000, 000 },    // 16 n
        { 000, 000, 016, 021, 021, 021, 016, 000, 000 },    // 17 o
        { 000, 000, 036, 021, 021, 021, 036, 020, 020 },    // 20 p
        { 000, 000, 017, 021, 021, 021, 017, 001, 001 },    // 21 q
        { 000, 000, 026, 031, 020, 020, 020, 000, 000 },    // 22 r
        { 000, 000, 017, 020, 016, 001, 036, 000, 000 },    // 23 s
        { 010, 010, 034, 010, 010, 011, 006, 000, 000 },    // 24 t
        { 000, 000, 021, 021, 021, 023, 015, 000, 000 },    // 25 u
        { 000, 000, 021, 021, 021, 012, 004, 000, 000 },    // 26 v
        { 000, 000, 021, 025, 025, 025, 012, 000, 000 },    // 27 w
        { 000, 000, 021, 012, 004, 012, 021, 000, 000 },    // 30 x
        { 000, 000, 021, 021, 021, 021, 017, 001, 016 },    // 31 y
        { 000, 000, 037, 002, 016, 010, 037, 000, 000 },    // 32 z
        { 000, 000, 000, 000, 000, 000, 000, 000, 000 },    // 33 not drawn
        { 000, 000, 000, 000, 000, 000, 000, 000, 000 },    // 34 not drawn
        { 000, 000, 000, 000, 000, 000, 000, 000, 000 },    // 35 not drawn
        { 000, 000, 000, 000, 000, 000, 000, 000, 000 },    // 36 not drawn
        { 000, 000, 000, 000, 000, 000, 000, 000, 000 },    // 37 not drawn
        { 000, 000, 000, 000, 000, 000, 000, 000, 000 },    // 40 space
        { 037, 037, 037, 037, 037, 037, 037, 000, 000 },    // 41 unassigned, a block
        { 037, 037, 037, 037, 037, 037, 037, 000, 000 },    // 42 unassigned, a block
        { 010, 025, 002, 000, 000, 000, 000, 000, 000 },    // 43 ~
        { 037, 037, 037, 037, 037, 037, 037, 000, 000 },    // 44 unassigned, a block
        { 037, 037, 037, 037, 037, 037, 037, 000, 000 },    // 45 unassigned, a block
        { 004, 016, 025, 004, 004, 004, 004, 000, 000 },    // 46 up arrow
        { 000, 004, 002, 037, 002, 004, 000, 000, 000 },    // 47 right arrow
        { 004, 004, 004, 004, 025, 016, 004, 000, 000 },    // 50 down arrow
        { 000, 004, 010, 037, 010, 004, 000, 000, 000 },    // 51 left arrow
        { 000, 020, 010, 004, 002, 001, 000, 000, 000 },    // 52 backslash
        { 016, 010, 010, 010, 010, 010, 016, 000, 000 },    // 53 [
        { 016, 002, 002, 002, 002, 002, 016, 000, 000 },    // 54 ]
        { 002, 004, 004, 010, 004, 004, 002, 000, 000 },    // 55 {
        { 010, 004, 004, 002, 004, 004, 010, 000, 000 },    // 56 }
        { 037, 037, 037, 037, 037, 037, 037, 000, 000 },    // 57 unassigned, a block
        { 000, 000, 000, 000, 000, 000, 037, 000, 000 },    // 60 _
        { 037, 037, 037, 037, 037, 037, 037, 000, 000 },    // 61 unassigned, a block
        { 004, 004, 004, 004, 004, 004, 004, 000, 000 },    // 62 |
        { 037, 037, 037, 037, 037, 037, 037, 000, 000 },    // 63 unassigned, a block
        { 037, 037, 037, 037, 037, 037, 037, 000, 000 },    // 64 unassigned, a block
        { 037, 037, 037, 037, 037, 037, 037, 000, 000 },    // 65 unassigned, a block
        { 010, 004, 002, 000, 000, 000, 000, 000, 000 },    // 66 `
        { 004, 012, 021, 000, 000, 000, 000, 000, 000 },    // 67 ^
        { 037, 037, 037, 037, 037, 037, 037, 000, 000 },    // 70 unassigned, a block
        { 037, 037, 037, 037, 037, 037, 037, 000, 000 },    // 71 unassigned, a block
        { 000, 000, 000, 000, 000, 000, 000, 000, 000 },    // 72 not drawn
        { 000, 000, 000, 000, 000, 000, 000, 000, 000 },    // 73 not drawn
        { 037, 037, 037, 037, 037, 037, 037, 000, 000 },    // 74 unassigned, a block
        { 037, 037, 037, 037, 037, 037, 037, 000, 000 },    // 75 unassigned, a block
        { 037, 037, 037, 037, 037, 037, 037, 000, 000 },    // 76 unassigned, a block
        { 000, 000, 000, 000, 000, 000, 000, 000, 000 },    // 77 not drawn
    }
};

static uint32_t tape[TAPEBLOCKS * BLKWORDS];
static Picture pic;
static uint8_t *pixP;
static int size;
static double toPixel;
static int shownChars;
static uint32_t crcTable[256];

// A tape word as a signed value, in the PDP-1's 1's complement.
static int
signedWord(uint32_t word)
{
    word &= WORDMASK;
    if( word & 0400000 )
    {
        return( -(int)((~word) & WORDMASK) );
    }
    return( (int)word );
}

// A tape word that indexes a table, as signedWord gives it but with minus zero, NOPOINT, made
// -1, since signedWord makes it 0, which would name the table's first entry.
static int
indexWord(uint32_t word)
{
    if( (word & WORDMASK) == NOPOINT )
    {
        return( -1 );
    }
    return( signedWord(word) );
}

// The PDP-1's add, 1's complement with the end-around carry, a -0 result made +0.
static uint32_t
onesAdd(uint32_t a, uint32_t b)
{
uint32_t sum;

    sum = (a + b);
    if( sum > WORDMASK )
    {
        sum = ((sum + 1) & WORDMASK);
    }
    if( sum == WORDMASK )
    {
        sum = 0;
    }
    return( sum );
}

// Whether block b's 258 words total zero, as every block written whole does.
static bool
blockGood(int block)
{
uint32_t sum;
int k;

    sum = 0;
    for( k = 0; k < BLKWORDS; k++ )
    {
        sum = onesAdd(sum, tape[(block * BLKWORDS) + k]);
    }
    return( sum == 0 );
}

// Data word k (0 to 255) of block b.
static uint32_t
dataWord(int block, int k)
{
    return( tape[(block * BLKWORDS) + 1 + k] );
}

// Read an image file whole; blocks past its end stay blank, as on a fresh tape. A tape image is
// a whole number of blocks, at most TAPEBLOCKS of them (UsingType550Microtape.md).
// Returns false, having said why, when the file cannot be read or is not a tape image.
static bool
readImage(const char *pathP)
{
FILE *fP;
uint8_t bytes[4];
size_t got;
int n;
bool whole;
bool failed;

    memset(tape, 0, sizeof(tape));
    if( !(fP = fopen(pathP, "rb")) )
    {
        fprintf(stderr, "sketchplot: cannot open %s\n", pathP);
        return( false );
    }
    whole = true;
    for( n = 0; n < (TAPEBLOCKS * BLKWORDS); n++ )
    {
        got = fread(bytes, 1, 4, fP);
        if( got != 4 )
        {
            whole = (got == 0);
            break;
        }
        tape[n] = ((bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | ((uint32_t)bytes[3] << 24))
                   & WORDMASK);
    }
    if( (n % BLKWORDS) != 0 )
    {
        whole = false;
    }
    if( (n == (TAPEBLOCKS * BLKWORDS)) && (fgetc(fP) != EOF) )
    {
        whole = false;
    }
    failed = (ferror(fP) != 0);
    fclose(fP);
    if( failed )
    {
        fprintf(stderr, "sketchplot: cannot read %s\n", pathP);
        return( false );
    }
    if( !whole )
    {
        fprintf(stderr, "sketchplot: %s is not a tape image\n", pathP);
        return( false );
    }
    return( true );
}

// Find drive d's image file in the mount list. Its line is the drive number in decimal, spaces,
// then the path to the end of the line, ",locked" dropped, relative to the Microtapes directory
// unless it starts with '/'. Returns false, having said why, when the drive has no tape.
static bool
driveImage(int drive, char *pathP, size_t room)
{
FILE *fP;
char line[4096];
char *p;
char *endP;
long d;
size_t len;

    if( !(fP = fopen(MOUNTLIST, "r")) )
    {
        fprintf(stderr, "sketchplot: no %s, so drive %d has no tape\n", MOUNTLIST, drive);
        return( false );
    }
    while( fgets(line, sizeof(line), fP) )
    {
        len = strlen(line);
        while( (len > 0) && ((line[len - 1] == '\n') || (line[len - 1] == '\r')) )
        {
            line[--len] = '\0';
        }
        p = line;
        while( (*p == ' ') || (*p == '\t') )
        {
            p++;
        }
        if( (*p == '\0') || (*p == '#') )
        {
            continue;
        }
        d = strtol(p, &endP, 10);
        if( (endP == p) || (d != drive) || ((*endP != ' ') && (*endP != '\t')) )
        {
            continue;
        }
        p = endP;
        while( (*p == ' ') || (*p == '\t') )
        {
            p++;
        }
        len = strlen(p);
        if( (len >= strlen(LOCKED)) && (strcmp(p + (len - strlen(LOCKED)), LOCKED) == 0) )
        {
            p[len - strlen(LOCKED)] = '\0';
        }
        if( *p == '\0' )
        {
            continue;
        }
        if( *p == '/' )
        {
            snprintf(pathP, room, "%s", p);
        }
        else
        {
            snprintf(pathP, room, "%s%s", TAPEDIR, p);
        }
        fclose(fP);
        return( true );
    }
    fclose(fP);
    fprintf(stderr, "sketchplot: drive %d has no tape (no line in %s)\n", drive, MOUNTLIST);
    return( false );
}

// Picture p's first block (sptape.ac, tpFirst), P9 on past the directory's block.
static int
slotFirst(int p)
{
    if( p < TPHALF )
    {
        return( 1 + (p * TPSLOT) );
    }
    return( TPDIRBLK + 1 + ((p - TPHALF) * TPSLOT) );
}

// Word k of the stream from block first, the 256 data words of each block in turn.
static uint32_t
streamWord(int first, int k)
{
    return( dataWord(first + (k / 256), k % 256) );
}

// Check picture p (0 to 15) as Sketchpad's Load does before it reads a word into its tables.
// It checks the header, this format and its stream's size, the trailing header the same, then every
// block's sum. Returns PICOK, PICEMPTY or PICBAD.
static int
checkPicture(int p)
{
int first;
int k;

    first = slotFirst(p);
    if( (streamWord(first, 0) != TPMAGIC) || (streamWord(first, 1) != TPFORMAT)
        || (streamWord(first, 2) != TPSTREAM) )
    {
        return( PICEMPTY );
    }
    for( k = 0; k < TPHDRWORDS; k++ )
    {
        if( streamWord(first, STRAILER + k) != streamWord(first, k) )
        {
            return( PICEMPTY );
        }
    }
    for( k = 0; k < TPPBLOCKS; k++ )
    {
        if( !blockGood(first + k) )
        {
            return( PICBAD );
        }
    }
    return( PICOK );
}

// A text as characters, for --dump, set 1's letters, digits and signs as ASCII, set 2's letters
// in lower case, anything else '?'.
static void
textString(const uint32_t *runP, int nWords, char *outP)
{
int w;
int j;
int code;
int set;

    set = 0;
    for( w = 0; w < nWords; w++ )
    {
        for( j = 0; j < 3; j++ )
        {
            code = ((runP[w] >> (12 - (6 * j))) & 077);
            if( code == CEND )
            {
                *outP = '\0';
                return;
            }
            if( (code == CUP) || (code == CLOW) )
            {
                set = ((code == CUP) ? 0 : 1);
                continue;
            }
            if( (code >= 1) && (code <= 032) )
            {
                *outP++ = (char)(((set == 0) ? 'A' : 'a') + (code - 1));
            }
            else if( (set == 0) && (code >= 040) )
            {
                *outP++ = (char)code;
            }
            else
            {
                *outP++ = '?';
            }
        }
    }
    *outP = '\0';
}

// Decode picture p, already checked, into pic.
static void
decodePicture(int p)
{
int first;
int data;
int text;
int digit;
int k;
int j;
int b2;
uint32_t nameRun[NAMEW];

    memset(&pic, 0, sizeof(pic));
    first = slotFirst(p);
    data = SPICTURE;
    text = STEXTS;
    digit = SDIGITS;
    pic.format = streamWord(first, 1);
    pic.words = streamWord(first, 2);
    pic.generation = streamWord(first, 3);
    for( k = 0; k < NAMEW; k++ )
    {
        nameRun[k] = streamWord(first, SNAME + k);
    }
    textString(nameRun, NAMEW, pic.name);

    pic.pointCount = signedWord(streamWord(first, data + SPOINTCOUNT));
    pic.lineCount = signedWord(streamWord(first, data + SLINECOUNT));
    for( k = 0; k < MAXPTS; k++ )
    {
        pic.pointX[k] = signedWord(streamWord(first, data + SPOINTX + k));
        pic.pointY[k] = signedWord(streamWord(first, data + SPOINTY + k));
    }
    for( k = 0; k < MAXLINES; k++ )
    {
        pic.lineFrom[k] = indexWord(streamWord(first, data + SLINEFROM + k));
        pic.lineTo[k] = indexWord(streamWord(first, data + SLINETO + k));
    }
    for( k = 0; k < MAXINST; k++ )
    {
        pic.instX[k] = signedWord(streamWord(first, data + SIX + k));
        pic.instY[k] = signedWord(streamWord(first, data + SIY + k));
        pic.instScale[k] = streamWord(first, data + SISCALE + k);
        pic.instMaster[k] = indexWord(streamWord(first, data + SBANK2 + B2IMASTER + k));
    }
    pic.instCount = signedWord(streamWord(first, data + SBANK2 + B2INSTCOUNT));
    for( k = 0; k < NMASTER; k++ )
    {
        pic.masterLen[k] = signedWord(streamWord(first, data + SBANK2 + B2MLEN + k));
    }
    for( k = 0; k < (NMASTER * MWORDS); k++ )
    {
        pic.masters[k] = streamWord(first, data + SMASTERS + k);
    }
    pic.arcCount = signedWord(streamWord(first, data + SBANK2 + B2ARCCOUNT));
    for( k = 0; k < MAXARCS; k++ )
    {
        b2 = (data + SBANK2 + B2ARCTC + k);
        pic.arcCenter[k] = indexWord(streamWord(first, b2));
        pic.arcStart[k] = indexWord(streamWord(first, b2 + MAXARCS));
        pic.arcEnd[k] = indexWord(streamWord(first, b2 + (2 * MAXARCS)));
        pic.arcDir[k] = signedWord(streamWord(first, b2 + (3 * MAXARCS)));
        pic.arcSteps[k] = signedWord(streamWord(first, b2 + (4 * MAXARCS)));
        pic.arcR32[k] = signedWord(streamWord(first, b2 + (5 * MAXARCS)));
    }

    pic.txN = signedWord(streamWord(first, text + TXN));
    pic.mtN = signedWord(streamWord(first, text + MTN));
    for( k = 0; k < NTEXT; k++ )
    {
        pic.txX[k] = signedWord(streamWord(first, text + TXX + k));
        pic.txY[k] = signedWord(streamWord(first, text + TXY + k));
        pic.txSc[k] = signedWord(streamWord(first, text + TXSC + k));
        pic.txCh[k] = signedWord(streamWord(first, text + TXCH + k));
        pic.txW[k] = signedWord(streamWord(first, text + TXW + k));
        for( j = 0; j < TXRUN; j++ )
        {
            pic.txRun[(k * TXRUN) + j] = streamWord(first, text + TXRUNAT + (k * TXRUN) + j);
        }
        pic.txAn[k] = streamWord(first, digit + DTXAN + k);
    }
    for( k = 0; k < NMTEXT; k++ )
    {
        pic.mtM[k] = indexWord(streamWord(first, text + MTM + k));
        pic.mtDX[k] = signedWord(streamWord(first, text + MTDX + k));
        pic.mtDY[k] = signedWord(streamWord(first, text + MTDY + k));
        pic.mtSc[k] = signedWord(streamWord(first, text + MTSC + k));
        pic.mtCh[k] = signedWord(streamWord(first, text + MTCH + k));
        pic.mtW[k] = signedWord(streamWord(first, text + MTW + k));
        for( j = 0; j < TXRUN; j++ )
        {
            pic.mtRun[(k * TXRUN) + j] = streamWord(first, text + MTRUNAT + (k * TXRUN) + j);
        }
        pic.mtAn[k] = streamWord(first, digit + DMTAN + k);
        pic.mtVal[k] = signedWord(streamWord(first, digit + DMTVAL + k));
    }
}

// A table count held to its table's size, so a damaged word cannot index past it.
static int
held(int count, int most)
{
    if( count < 0 )
    {
        return( 0 );
    }
    return( (count > most) ? most : count );
}

// A value at an instance's scale, as Sketchpad shows a master's digits, doubled once a step,
// its magnitude held at 99999 once a double would pass it, the sign kept.
static int
digitsAtScale(int value, int scale)
{
int magnitude;
int s;

    magnitude = abs(value);
    for( s = 0; s < scale; s++ )
    {
        if( magnitude >= ((MAXDIGITS + 1) / 2) )
        {
            magnitude = MAXDIGITS;
            break;
        }
        magnitude *= 2;
    }
    return( (value < 0) ? -magnitude : magnitude );
}

// A value at an instance's size, factor, its step being scale. It is digitsAtScale at a power
// of two, otherwise the value times the size, rounded, its magnitude held at 99999.
static int
digitsAtSize(int value, int scale, double factor)
{
double magnitude;

    if( factor == (double)(1 << scale) )
    {
        return( digitsAtScale(value, scale) );
    }
    magnitude = (fabs((double)value) * factor);
    if( magnitude > MAXDIGITS )
    {
        magnitude = MAXDIGITS;
    }
    return( (value < 0) ? -(int)lround(magnitude) : (int)lround(magnitude) );
}

// A value as Sketchpad writes digits, an upper shift, a minus sign when negative, the digits
// with no leading zeros, then end codes through the run. Returns the characters, the shift
// and the end codes not counted.
static int
digitsRun(int value, uint32_t *runP)
{
int codes[(TXRUN * 3) + 1];
int n;
int chars;
int k;
char digits[12];

    n = 0;
    codes[n++] = CUP;
    if( value < 0 )
    {
        codes[n++] = CMINUS;
    }
    snprintf(digits, sizeof(digits), "%d", abs(value));
    for( k = 0; digits[k] != '\0'; k++ )
    {
        codes[n++] = (CZERO + (digits[k] - '0'));
    }
    chars = (n - 1);
    while( n < (TXRUN * 3) )
    {
        codes[n++] = CEND;
    }
    for( k = 0; k < TXRUN; k++ )
    {
        runP[k] = ((codes[3 * k] << 12) | (codes[(3 * k) + 1] << 6) | codes[(3 * k) + 2]);
    }
    return( chars );
}

// Light the pixel at (x, y), pixel coordinates from the top left.
static void
setPixel(int x, int y)
{
    if( (x >= 0) && (x < size) && (y >= 0) && (y < size) )
    {
        pixP[(y * size) + x] = 1;
    }
}

// A screen x or y (y up) as a pixel's column or row (y down), rounded to the nearest, since an
// arc's points fall between units.
static int
pixelX(double x)
{
    return( (int)floor((x * toPixel) + 0.5) );
}

static int
pixelY(double y)
{
    return( (size - 1) - (int)floor((y * toPixel) + 0.5) );
}

// A line between two pixels, Bresenham's.
static void
pixelLine(int x0, int y0, int x1, int y1)
{
int dx;
int dy;
int sx;
int sy;
int err;
int e2;

    dx = abs(x1 - x0);
    dy = -abs(y1 - y0);
    sx = ((x0 < x1) ? 1 : -1);
    sy = ((y0 < y1) ? 1 : -1);
    err = (dx + dy);
    for( ;; )
    {
        setPixel(x0, y0);
        if( (x0 == x1) && (y0 == y1) )
        {
            break;
        }
        e2 = (2 * err);
        if( e2 >= dy )
        {
            err += dy;
            x0 += sx;
        }
        if( e2 <= dx )
        {
            err += dx;
            y0 += sy;
        }
    }
}

// A line between two screen points.
static void
screenLine(double x0, double y0, double x1, double y1)
{
    pixelLine(pixelX(x0), pixelY(y0), pixelX(x1), pixelY(y1));
}

// Whether a point index names a point of the picture. Sketchpad keeps its points packed, the
// first pointCount of the table, so a word past them is stale.
static bool
pointOk(int k)
{
    return( (k >= 0) && (k < held(pic.pointCount, MAXPTS)) );
}

// Whether a master index names a master with words in it. Define makes none empty, and Inst
// and Change give no instance an empty one.
static bool
masterOk(int m)
{
    return( (m >= 0) && (m < NMASTER) && (pic.masterLen[m] > 0) );
}

// Arc k, drawn true, about its center at the start's distance, from the start round to the
// end's direction the way the arc turns, a full turn when the end is the start; then on to
// the end itself, as the screen's last chord does should it lie off that radius.
static void
drawArc(int k)
{
double cx;
double cy;
double r;
double a0;
double a1;
double sweep;
double step;
double a;
double px;
double py;
double nx;
double ny;
int n;
int i;
int dir;

    if( !pointOk(pic.arcCenter[k]) || !pointOk(pic.arcStart[k]) || !pointOk(pic.arcEnd[k]) )
    {
        return;
    }
    dir = pic.arcDir[k];
    if( (dir != 1) && (dir != -1) )
    {
        return;
    }
    cx = pic.pointX[pic.arcCenter[k]];
    cy = pic.pointY[pic.arcCenter[k]];
    px = (pic.pointX[pic.arcStart[k]] - cx);
    py = (pic.pointY[pic.arcStart[k]] - cy);
    r = sqrt((px * px) + (py * py));

    // Sketchpad gives no arc a radius past ARCRMAX (a unit more allows for its rounding), and a
    // damaged one's chords at the largest size would number tens of millions.
    if( (r < 0.5) || (r > (ARCRMAX + 1)) )
    {
        return;
    }
    a0 = atan2(py, px);
    a1 = atan2(pic.pointY[pic.arcEnd[k]] - cy, pic.pointX[pic.arcEnd[k]] - cx);
    if( pic.arcEnd[k] == pic.arcStart[k] )
    {
        sweep = (2.0 * M_PI);
    }
    else
    {
        sweep = ((dir > 0) ? (a1 - a0) : (a0 - a1));
        while( sweep <= 0.0 )
        {
            sweep += (2.0 * M_PI);
        }
        while( sweep > (2.0 * M_PI) )
        {
            sweep -= (2.0 * M_PI);
        }
    }

    // Steps of at most half a pixel along the arc.
    step = (0.5 / (r * toPixel));
    n = (int)ceil(sweep / step);
    if( n < 1 )
    {
        n = 1;
    }
    px = (cx + (r * cos(a0)));
    py = (cy + (r * sin(a0)));
    for( i = 1; i <= n; i++ )
    {
        a = (a0 + (dir * sweep * ((double)i / n)));
        nx = (cx + (r * cos(a)));
        ny = (cy + (r * sin(a)));
        screenLine(px, py, nx, ny);
        px = nx;
        py = ny;
    }
    if( pic.arcEnd[k] != pic.arcStart[k] )
    {
        screenLine(px, py, pic.pointX[pic.arcEnd[k]], pic.pointY[pic.arcEnd[k]]);
    }
}

// Instance i's 340 scale k, from its scale word.
static int
instStep(int i)
{
    return( (int)(pic.instScale[i] & SCALEK) );
}

// Instance i's size, 2^k, times c / 2^15 when its multiplier code c is not 0.
static double
instSize(int i)
{
uint32_t c;

    c = (pic.instScale[i] >> SCALECSHIFT);
    if( c == 0 )
    {
        return( (double)(1 << instStep(i)) );
    }
    return( (1 << instStep(i)) * (c / SCALECUNIT) );
}

// Instance i, its master's vector words from its origin, each step times its size, as the
// instance's slot calls the master with save in vector mode.
static void
drawInstance(int i)
{
int m;
int k;
double x;
double y;
int dx;
int dy;
double mult;
uint32_t word;

    m = pic.instMaster[i];
    if( !masterOk(m) )
    {
        return;
    }
    mult = instSize(i);
    x = pic.instX[i];
    y = pic.instY[i];
    for( k = 0; k < held(pic.masterLen[m], MWORDS); k++ )
    {
        word = pic.masters[(m * MWORDS) + k];
        dy = ((word >> 8) & 0177);
        dx = (word & 0177);
        if( word & VDOWN )
        {
            dy = -dy;
        }
        if( word & VLEFT )
        {
            dx = -dx;
        }
        if( word & VVISIBLE )
        {
            screenLine(x, y, x + (dx * mult), y + (dy * mult));
        }
        x += (dx * mult);
        y += (dy * mult);
        if( word & VEND )
        {
            break;
        }
    }
}

// One character's dots from (x, y) at scale, each joined to its neighbors in the matrix so
// that a character's strokes read whole at every scale, as they do on the screen at x1.
static void
drawGlyph(int set, int code, int x, int y, int scale)
{
const uint8_t *rowsP;
int row;
int col;
int dRow;
int dCol;
int r2;
int c2;
int mult;
double gx;
double gy;

    rowsP = glyphs[set][code];
    mult = (1 << scale);
    for( row = 0; row < GLYPHROWS; row++ )
    {
        for( col = 0; col < 5; col++ )
        {
            if( !(rowsP[row] & (020 >> col)) )
            {
                continue;
            }
            gx = (x + (col * mult));
            gy = (y + ((GLYPHTOP - row) * mult));
            setPixel(pixelX(gx), pixelY(gy));
            for( dRow = 0; dRow <= 1; dRow++ )
            {
                for( dCol = -1; dCol <= 1; dCol++ )
                {
                    if( (dRow == 0) && (dCol <= 0) )
                    {
                        continue;
                    }
                    r2 = (row + dRow);
                    c2 = (col + dCol);
                    if( (r2 >= GLYPHROWS) || (c2 < 0) || (c2 > 4) || !(rowsP[r2] & (020 >> c2)) )
                    {
                        continue;
                    }
                    screenLine(gx, gy, x + (c2 * mult), y + ((GLYPHTOP - r2) * mult));
                }
            }
        }
    }
}

// A text's run from (x, y), its first character's lower left, at scale, as the 340's
// character mode draws it. The shifts choose the set, the end code ends it.
static void
drawRun(const uint32_t *runP, int nWords, int x, int y, int scale)
{
int w;
int j;
int code;
int set;

    set = 0;
    for( w = 0; w < nWords; w++ )
    {
        for( j = 0; j < 3; j++ )
        {
            code = ((runP[w] >> (12 - (6 * j))) & 077);
            if( code == CEND )
            {
                return;
            }
            if( (code == CUP) || (code == CLOW) )
            {
                set = ((code == CUP) ? 0 : 1);
                continue;
            }
            if( (code == 033) || (code == 034) )
            {
                continue;
            }
            drawGlyph(set, code, x, y, scale);
            x += (CHADV << scale);
        }
    }
}

// A text's box, CHADV a character wide and CHHIGH high at x1, doubling each scale.
static int
boxWidth(int chars, int scale)
{
    return( (chars * CHADV) << scale );
}

static int
boxHeight(int scale)
{
    return( CHHIGH << scale );
}

// Show a text as Sketchpad's text list does, passed over when its box leaves the screen or
// its characters would take the texts past TEXTBUDGET.
static void
showText(const uint32_t *runP, int nWords, int x, int y, int scale, int chars, bool dump)
{
int cost;
char text[(TXRUN * 3) + 1];

    cost = (shownChars + chars + TXOWN);
    if( cost > TEXTBUDGET )
    {
        return;
    }
    if( (x < 0) || ((x + boxWidth(chars, scale)) >= SCREEN) || (y < 0)
        || ((y + boxHeight(scale)) >= SCREEN) )
    {
        return;
    }
    shownChars = cost;
    if( dump )
    {
        textString(runP, nWords, text);
        printf("shown %d %d x%d \"%s\"\n", x, y, (1 << scale), text);
        return;
    }
    drawRun(runP, nWords, x, y, scale);
}

// The texts as Sketchpad shows them. The canvas's first; then for each instance its master's,
// at its origin plus each text's offset at its size, at the text's scale plus the instance's,
// at most x8, a master's digits showing their value at the instance's size about their center.
static void
showTexts(bool dump)
{
int i;
int t;
int s;
double factor;
int sc;
int x;
int y;
int chars;
int nWords;
uint32_t run[TXRUN];

    shownChars = 0;
    for( t = 0; t < held(pic.txN, NTEXT); t++ )
    {
        showText(&pic.txRun[t * TXRUN], held(pic.txW[t], TXRUN), pic.txX[t], pic.txY[t],
                 held(pic.txSc[t], MAXSCALE), held(pic.txCh[t], TXRUN * 3), dump);
    }
    for( i = 0; i < held(pic.instCount, MAXINST); i++ )
    {
        if( !masterOk(pic.instMaster[i]) )
        {
            continue;
        }
        s = instStep(i);
        factor = instSize(i);
        for( t = 0; t < held(pic.mtN, NMTEXT); t++ )
        {
            if( pic.mtM[t] != pic.instMaster[i] )
            {
                continue;
            }
            x = pic.mtDX[t];
            y = pic.mtDY[t];
            sc = held(pic.mtSc[t], MAXSCALE);
            if( pic.mtAn[t] != NOPOINT )
            {
                x += (boxWidth(held(pic.mtCh[t], TXRUN * 3), sc) >> 1);
                y += (boxHeight(sc) >> 1);
            }
            x = ((int)lround(x * factor) + pic.instX[i]);
            y = ((int)lround(y * factor) + pic.instY[i]);
            sc = held(sc + s, MAXSCALE);
            if( pic.mtAn[t] != NOPOINT )
            {
                chars = digitsRun(digitsAtSize(pic.mtVal[t], s, factor), run);
                nWords = TXRUN;
                x -= (boxWidth(chars, sc) >> 1);
                y -= (boxHeight(sc) >> 1);
            }
            else
            {
                memcpy(run, &pic.mtRun[t * TXRUN], sizeof(run));
                chars = held(pic.mtCh[t], TXRUN * 3);
                nWords = held(pic.mtW[t], TXRUN);
            }
            showText(run, nWords, x, y, sc, chars, dump);
        }
    }
}

// Print the decoded tables, one item a line.
static void
dumpPicture(int p)
{
int k;
char text[(TXRUN * 3) + 1];

    printf("picture P%d format %d words %d generation %06o\n", p + 1, pic.format, pic.words,
           pic.generation);
    printf("name \"%s\"\n", pic.name);
    printf("points %d\n", pic.pointCount);
    for( k = 0; k < held(pic.pointCount, MAXPTS); k++ )
    {
        printf("point %d %d %d\n", k, pic.pointX[k], pic.pointY[k]);
    }
    printf("lines %d\n", pic.lineCount);
    for( k = 0; k < held(pic.lineCount, MAXLINES); k++ )
    {
        printf("line %d %d %d\n", k, pic.lineFrom[k], pic.lineTo[k]);
    }
    printf("arcs %d\n", pic.arcCount);
    for( k = 0; k < held(pic.arcCount, MAXARCS); k++ )
    {
        printf("arc %d %d %d %d %d %d %d\n", k, pic.arcCenter[k], pic.arcStart[k], pic.arcEnd[k],
               pic.arcDir[k], pic.arcSteps[k], pic.arcR32[k]);
    }
    for( k = 0; k < NMASTER; k++ )
    {
        printf("master %d %d\n", k, pic.masterLen[k]);
    }
    printf("instances %d\n", pic.instCount);
    for( k = 0; k < held(pic.instCount, MAXINST); k++ )
    {
        printf("instance %d %d %d %d %d", k, pic.instMaster[k], pic.instX[k], pic.instY[k],
               instStep(k));
        if( pic.instScale[k] >> SCALECSHIFT )
        {
            printf(" size %.6f", instSize(k));
        }
        printf("\n");
    }
    printf("texts %d\n", pic.txN);
    for( k = 0; k < held(pic.txN, NTEXT); k++ )
    {
        textString(&pic.txRun[k * TXRUN], TXRUN, text);
        printf("text %d %d %d %d %d %s \"%s\"\n", k, pic.txX[k], pic.txY[k], pic.txSc[k],
               pic.txCh[k], (pic.txAn[k] == NOPOINT) ? "plain" : "digits", text);
    }
    printf("mtexts %d\n", pic.mtN);
    for( k = 0; k < held(pic.mtN, NMTEXT); k++ )
    {
        textString(&pic.mtRun[k * TXRUN], TXRUN, text);
        printf("mtext %d %d %d %d %d %d %s %d \"%s\"\n", k, pic.mtM[k], pic.mtDX[k], pic.mtDY[k],
               pic.mtSc[k], pic.mtCh[k], (pic.mtAn[k] == NOPOINT) ? "plain" : "digits",
               pic.mtVal[k], text);
    }
    showTexts(true);
}

// Draw the picture into pixP.
static void
drawPicture(void)
{
int k;
int a;
int b;

    for( k = 0; k < held(pic.lineCount, MAXLINES); k++ )
    {
        a = pic.lineFrom[k];
        b = pic.lineTo[k];
        if( pointOk(a) && pointOk(b) )
        {
            screenLine(pic.pointX[a], pic.pointY[a], pic.pointX[b], pic.pointY[b]);
        }
    }
    for( k = 0; k < held(pic.arcCount, MAXARCS); k++ )
    {
        drawArc(k);
    }
    for( k = 0; k < held(pic.instCount, MAXINST); k++ )
    {
        drawInstance(k);
    }
    showTexts(false);
}

// The PNG's CRC-32 (ISO 3309), table driven.
static void
crcInit(void)
{
uint32_t c;
int n;
int k;

    for( n = 0; n < 256; n++ )
    {
        c = (uint32_t)n;
        for( k = 0; k < 8; k++ )
        {
            c = ((c & 1) ? (0xedb88320U ^ (c >> 1)) : (c >> 1));
        }
        crcTable[n] = c;
    }
}

// The CRC of len bytes at bufP added to crc.
static uint32_t
crcAdd(uint32_t crc, const uint8_t *bufP, size_t len)
{
size_t k;

    for( k = 0; k < len; k++ )
    {
        crc = (crcTable[(crc ^ bufP[k]) & 0xff] ^ (crc >> 8));
    }
    return( crc );
}

// A 32-bit word, most significant byte first, as PNG wants every number.
static void
putBig(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// One chunk, its length, type, data and the CRC of the type and data.
// Returns false when the write fails.
static bool
writeChunk(FILE *fP, const char *typeP, const uint8_t *dataP, uint32_t len)
{
uint8_t head[8];
uint8_t tail[4];
uint32_t crc;

    putBig(head, len);
    memcpy(head + 4, typeP, 4);
    crc = crcAdd(0xffffffffU, head + 4, 4);
    crc = crcAdd(crc, dataP, len);
    putBig(tail, crc ^ 0xffffffffU);
    return( (fwrite(head, 1, 8, fP) == 8) && ((len == 0) || (fwrite(dataP, 1, len, fP) == len))
            && (fwrite(tail, 1, 4, fP) == 4) );
}

// Write the picture as a PNG, one bit deep, gray, each row's filter none, the rows in a zlib
// stream of stored deflate blocks. Returns false, having said why, when the file cannot be
// written.
static bool
writePng(const char *pathP, bool inverse)
{
FILE *fP;
uint8_t ihdr[13];
uint8_t *rawP;
uint8_t *zP;
size_t rowBytes;
size_t rawLen;
size_t zLen;
size_t done;
size_t blockLen;
size_t z;
uint32_t a;
uint32_t b;
int x;
int y;
bool lit;
bool ok;

    rowBytes = (1 + ((size + 7) / 8));
    rawLen = (rowBytes * size);
    zLen = (2 + (5 * ((rawLen + STOREDMAX - 1) / STOREDMAX)) + rawLen + 4);
    rawP = calloc(rawLen, 1);
    zP = malloc(zLen);
    if( !rawP || !zP )
    {
        fprintf(stderr, "sketchplot: out of memory\n");
        free(rawP);
        free(zP);
        return( false );
    }
    for( y = 0; y < size; y++ )
    {
        for( x = 0; x < size; x++ )
        {
            lit = (pixP[(y * size) + x] != 0);
            if( lit != inverse )
            {
                rawP[(y * rowBytes) + 1 + (x / 8)] |= (uint8_t)(0x80 >> (x % 8));
            }
        }
    }

    // The zlib stream, deflate with a 32K window and no dictionary, stored blocks, Adler-32.
    z = 0;
    zP[z++] = 0x78;
    zP[z++] = 0x01;
    for( done = 0; done < rawLen; done += blockLen )
    {
        blockLen = (((rawLen - done) > STOREDMAX) ? STOREDMAX : (rawLen - done));
        zP[z++] = (uint8_t)(((done + blockLen) == rawLen) ? 1 : 0);
        zP[z++] = (uint8_t)(blockLen & 0xff);
        zP[z++] = (uint8_t)(blockLen >> 8);
        zP[z++] = (uint8_t)(~blockLen & 0xff);
        zP[z++] = (uint8_t)((~blockLen >> 8) & 0xff);
        memcpy(zP + z, rawP + done, blockLen);
        z += blockLen;
    }
    a = 1;
    b = 0;
    for( done = 0; done < rawLen; done++ )
    {
        a = ((a + rawP[done]) % 65521);
        b = ((b + a) % 65521);
    }
    putBig(zP + z, (b << 16) | a);
    z += 4;

    putBig(ihdr, size);
    putBig(ihdr + 4, size);
    ihdr[8] = 1;
    ihdr[9] = 0;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    if( !(fP = fopen(pathP, "wb")) )
    {
        fprintf(stderr, "sketchplot: cannot write %s\n", pathP);
        free(rawP);
        free(zP);
        return( false );
    }
    ok = ((fwrite("\x89PNG\r\n\x1a\n", 1, 8, fP) == 8) && writeChunk(fP, "IHDR", ihdr, 13)
          && writeChunk(fP, "IDAT", zP, (uint32_t)z) && writeChunk(fP, "IEND", NULL, 0));
    if( fclose(fP) != 0 )
    {
        ok = false;
    }
    if( !ok )
    {
        fprintf(stderr, "sketchplot: cannot write %s\n", pathP);
        remove(pathP);
    }
    free(rawP);
    free(zP);
    return( ok );
}

// The usage text, to stderr.
static void
usage(void)
{
    fprintf(stderr,
            "usage: sketchplot [-d drive] [-p picture] [-t image] [-s size] [-i] out.png\n"
            "       sketchplot [-d drive] [-p picture] [-t image] --dump\n"
            "  -d n      drive 1 to 8 (default 1), its image from %s\n"
            "  -p n      picture P1 to P16 (default 1)\n"
            "  -t image  read this image file instead of a drive's\n"
            "  -s n      the PNG n pixels square (default 1024, one pixel a screen unit)\n"
            "  -i        black on white, for printing\n"
            "  --dump    print the picture's tables and write no PNG\n", MOUNTLIST);
}

// Returns 0 with the PNG written (or the tables printed), 1 when the picture is refused or a
// file cannot be read or written, 2 for a usage error.
int
main(int argc, char **argv)
{
static const struct option longOpts[] =
{
    { "dump", no_argument, NULL, 'D' },
    { NULL, 0, NULL, 0 }
};
int opt;
int drive;
int picture;
int result;
bool inverse;
bool dump;
char *imageP;
char *outP;
char *endP;
char path[4096];

    drive = 1;
    picture = 1;
    size = DEFSIZE;
    inverse = false;
    dump = false;
    imageP = NULL;
    while( (opt = getopt_long(argc, argv, "d:p:t:s:i", longOpts, NULL)) != -1 )
    {
        switch( opt )
        {
        case 'd':
            drive = (int)strtol(optarg, &endP, 10);
            if( (*endP != '\0') || (drive < 1) || (drive > NDRIVES) )
            {
                fprintf(stderr, "sketchplot: -d wants a drive from 1 to %d\n", NDRIVES);
                return( 2 );
            }
            break;
        case 'p':
            picture = (int)strtol(optarg, &endP, 10);
            if( (*endP != '\0') || (picture < 1) || (picture > NPICTURES) )
            {
                fprintf(stderr, "sketchplot: -p wants a picture from 1 to %d\n", NPICTURES);
                return( 2 );
            }
            break;
        case 't':
            imageP = optarg;
            break;
        case 's':
            size = (int)strtol(optarg, &endP, 10);
            if( (*endP != '\0') || (size < MINSIZE) || (size > MAXSIZE) )
            {
                fprintf(stderr, "sketchplot: -s wants a size from %d to %d\n", MINSIZE, MAXSIZE);
                return( 2 );
            }
            break;
        case 'i':
            inverse = true;
            break;
        case 'D':
            dump = true;
            break;
        default:
            usage();
            return( 2 );
        }
    }
    outP = NULL;
    if( optind < argc )
    {
        outP = argv[optind++];
    }
    if( (optind < argc) || (!dump && !outP) || (dump && outP) )
    {
        usage();
        return( 2 );
    }

    if( !imageP )
    {
        if( !driveImage(drive, path, sizeof(path)) )
        {
            return( 1 );
        }
        imageP = path;
    }
    if( !readImage(imageP) )
    {
        return( 1 );
    }
    result = checkPicture(picture - 1);
    if( result != PICOK )
    {
        fprintf(stderr, "sketchplot: P%d of %s: %s\n", picture, imageP,
                (result == PICEMPTY) ? "EMPTY" : "BAD BLOCK");
        return( 1 );
    }
    decodePicture(picture - 1);
    if( dump )
    {
        dumpPicture(picture - 1);
        return( 0 );
    }

    toPixel = ((double)size / SCREEN);
    if( !(pixP = calloc((size_t)size * size, 1)) )
    {
        fprintf(stderr, "sketchplot: out of memory\n");
        return( 1 );
    }
    drawPicture();
    crcInit();
    result = (writePng(outP, inverse) ? 0 : 1);
    free(pixP);
    return( result );
}
