#ifndef ADVDATALOADER_H
#define ADVDATALOADER_H
// Shared data between advdataloader components.

#include "symtab.h"

#define DEFAULT_DRUM "/opt/pidp1-mods/pdp23drum"

// Drum layout constants.
// SAVE and WIZCOM are fixed-size blocks at the beginning of track 16 (by default),
// and the message text starts immediately after them on the same track.
#define SAVE_TRACK           16   // First track. SAVE block, WIZCOM block
#define SAVE_BLOCK_WORDS     512  // records use ~268, room for expansion
#define WIZCOM_BLOCK_WORDS   512  // WIZCOM records use ~251
#define WIZCOM_BASE_OFFSET   SAVE_BLOCK_WORDS
#define DRUM_START_WORDS     (SAVE_BLOCK_WORDS + WIZCOM_BLOCK_WORDS)
#define DEFAULT_START_TRACK  SAVE_TRACK
#define WORDS_PER_TRACK       4096
#define NUM_TRACKS              32

#define MAX_MSGS       768     // number of text blocks we can build (see advtextloader.c MAX_BLOCKS)
#define MAX_TEXT       2048    // maximum characters allowed in one message block
#define MAX_NAME        128    // maximum name length for names we still copy into fixed buffers

#define MAXROOMS        512
#define MAXEXITS         19
#define EXITWORDS         3
#define HEADERWORDS       6
#define RECORDSIZE       64

#define COND_RAND_ID      4
#define RAND_DOMAIN      32

#define DARK_FLAG    0400000     // DEC bit 0 in the pdp-1 word
#define DWARF_FLAG   0200000     // DEC bit 1 in the pdp-1 word
#define SURFACE_FLAG 0100000     // DEC bit 2 in the pdp-1 word
#define PIRATE_FORBID_FLAG 0040000     // DEC bit 3 in the pdp-1 word

typedef int Word;      // Used to represent a pdp-1 word, but remember only 18 bits are significant.

#ifndef ADV_DEFINES_ONLY

typedef struct {
    SymNodeP symP;       // Symbol table entry for this block
    char text[MAX_TEXT]; // The text of the message
    int  track;          // and its location and size on the drum
    int  offset;
    int  nWords;
    int  padCount;       // 0-2, unused character slots in the last packed word
} MessageBlock, *MessageBlockP;

typedef struct {
    SymNodeP symP;      // Symbol table entry for this flag
    int value;          // The value the flag was defined with, usually a bitmask
} Flag, *FlagP;

typedef struct attribute {
    struct attribute *nextP;    // Attributes are a linked list, null if no more
    SymNodeP flagP;             // The flag associated with this attribute
    int value;                  // The value the flag has in this use, typically a 1/0 for yes/no
} Attribute, *AttributeP;

typedef struct {
    char *destNameP;            // destination room name, as written in the source
    int  destLine;              // source line number, for a post-parse error message
    int  destNum;               // resolved destination room number (post-parse)
    int  dirCode;               // resolved immediately against the hard-coded dirTable
    int  condId;                // 0 = unconditional, COND_RAND_ID = rand, else condTable id
    MessageBlockP condMsgP;     // resolved block message for a COND exit, else NULL
    int  randThreshold;         // valid only when condId == COND_RAND_ID
} Exit, *ExitP;

typedef struct {
    SymNodeP symP;              // Symbol table entry for this room
    int  num;                   // assigned room number, 1-based, by order of appearance
    MessageBlockP longMsgP;     // Long message for this room
    MessageBlockP shortMsgP;    // Short message for this room
    AttributeP attributesP;     // List of attributes for this room (flag name/value pairs)
    Exit exits[MAXEXITS];
    int  nexits;
} Room, *RoomP;

typedef struct {
    SymNodeP symP;      // Symbol table entry for this object's index name (e.g. KEYS)
    int  index;         // 0-based index, order of appearance
    char *vocSymP;      // vocabulary word symbol text -- objNames row ("name" field)
    char *locTextP;     // objLoc row value, verbatim symbol or number text ("loc" field)
    int  take;          // 0/1 -- objTake row value
    char *invMsgTextP;  // objInvMsg row value: "msg_<NAME>:1" or "0" for %none
    char *hereMsgTextP; // objHereMsg row value: "msg_<NAME>:1" or "0" for %none
    char *treasureTextP;// objTreasure row value: "1"/"0" for yes/no, or "0d<K>"/"0"
} Object, *ObjectP;

typedef enum {
    VERBARG_MOVE,    // argspec was 'move <DIRNAME>' -- argTextP becomes "DIR_<DIRNAME>"
    VERBARG_NONE,    // argspec was 'none' -- argTextP becomes "0"
    VERBARG_MSGREF,  // argspec was 'msgref <msgname>' -- argTextP becomes "msg_<msgname>:1"
    VERBARG_KARG     // argspec was 'karg <n>' -- argTextP becomes the literal integer text
} VerbArgKind;

typedef struct {
    VerbArgKind kind;
    char *strVal;    // direction suffix (MOVE) or message name (MSGREF), else NULL
    int  intVal;     // literal value (KARG), else unused
} VerbArg, *VerbArgP;

typedef struct {
    SymNodeP symP;      // Symbol table entry for this verb's unique key name
    char *vocTextP;     // word 0 of the emitted row: "voc_<vocword>:2", verbatim
    char *argTextP;     // word 1: "DIR_<X>" / "0" / "msg_<X>:1" / a literal int,
    char *handlerTextP; // word 2: "<handler>:0", emitted verbatim
} Verb, *VerbP;

void verror(const char *msgP, ...);
void verrorAt(int lineno, const char *msgP, ...);
#endif

#endif
