/*
 * This is shared by ovlload and drumlist so the two read one format one way, and by ovlload,
 * ad1 and fastload so the three write the drum one way.
 *
 * 13-Oct-2026 wje/Claude moved out of ovlload.c
 * 18-Oct-2026 wje cleanup
 * 21-Oct-2026 Claude the drum writer moved out of ovlload.c
 */

#ifndef OVLFILE_H
#define OVLFILE_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define OVL_TRACKS 32
#define OVL_TRACKWORDS 4096
#define OVL_DRUMWORDS (OVL_TRACKS * OVL_TRACKWORDS)
#define OVL_WORDMASK 0777777
#define OVL_HEADERWORDS 2           // the stamp and the overlay's number
#define OVL_NAMESIZE 64
#define OVL_ERRSIZE 512             // the size of the buffer ovlRead() reports a fault in

// What ovlCompare() finds for one overlay.
#define OVL_CURRENT 0
#define OVL_DIFFERS 1               // a word past the header
#define OVL_STAMP_DIFFERS 2         // a header word: another build's overlay, or none

// drumupdater marks a track it loaded with this in its last three words.
#define DRUMUPDATER_MARK 0707070
#define DRUMUPDATER_MARKFIRST 07775

// Adventure's save block begins at word 0 of track 16 (decimal), with this magic.
#define ADVENTURE_SAVE_TRACK 16
#define ADVENTURE_SAVE_MAGIC 31344

// A Z-machine story, as zloader installs it from track 0: two bytes a word, the first
// in the high byte.  The header fields are byte offsets (Z-machine Standard 1.1, S11).
#define ZH_VERSION 0x00
#define ZH_HIGHMEM_BASE 0x04
#define ZH_INITIAL_PC 0x06
#define ZH_STATIC_BASE 0x0E
#define ZH_SERIAL 0x12
#define ZH_SERIAL_BYTES 6
#define ZH_FILE_LENGTH 0x1A
#define ZH_HEADER_BYTES 0x40

typedef struct
{
    char name[OVL_NAMESIZE];
    char window[OVL_NAMESIZE];
    int bank;
    int first;
    int last;
    int track;
    int length;
    bool persistent;
    int lineNo;
    int32_t *wordsP;                // length words, the two header words first
} Overlay;

typedef struct
{
    int stamp;
    int tableAddr;
    int count;
    Overlay *overlaysP;
} OvlFile;

bool ovlRead(const char *pathP, OvlFile *ofP, char *errP);
void ovlFree(OvlFile *ofP);
int ovlCompare(const Overlay *ovP, const int32_t *drumP, int *addrP, int32_t *gotP, int32_t *wantP);
bool drumupdaterMarked(const int32_t *drumP, int track);
bool adventureSaved(const int32_t *drumP);
int storyTracks(const int32_t *drumP);
int storyByte(const int32_t *drumP, int byteOff);

// The drum writer. Its faults and warnings go to errFP, each beginning with "progName: ";
// what it did goes to stdout. A tool calls ovlSetProgram() first; until then, faults and
// warnings go to stderr as "ovlfile: ".
void ovlSetProgram(const char *progNameP, FILE *errFP);
bool ovlReadDrum(const char *pathP, int32_t *drumP, long *fileWordsP);
bool ovlStampMatches(const OvlFile *ofP, const char *tapeP, bool haveWord, uint32_t word);
bool ovlWriteDrum(const char *pathP, const OvlFile *ofP, long fileWords, bool eachOverlay);
void ovlWarnOverwritten(const OvlFile *ofP, const int32_t *drumP);
char *ovlNameForTape(const char *tapeP);

#endif
