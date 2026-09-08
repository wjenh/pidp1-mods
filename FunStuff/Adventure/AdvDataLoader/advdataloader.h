#ifndef ADVDATALOADER_H
#define ADVDATALOADER_H
// Shared data between advdataloader components.

#include "symtab.h"

#define DEFAULT_DRUM "/opt/pidp1-mods/pdp23drum"

// Drum layout constants.
// SAVE and WIZCOM are fixed-size blocks at the beginning of track 16 (by default),
// and the message text starts immediately after them on the same track.
#define SAVE_TRACK           16   // First track. SAVE block, WIZCOM block
#define SAVE_BLOCK_WORDS     512  // records use 377, room for expansion
#define WIZCOM_BLOCK_WORDS   512  // WIZCOM records use ~251
#define WIZCOM_BASE_OFFSET   SAVE_BLOCK_WORDS
#define DRUM_START_WORDS     (SAVE_BLOCK_WORDS + WIZCOM_BLOCK_WORDS)
#define DEFAULT_START_TRACK  SAVE_TRACK
#define WORDS_PER_TRACK       4096
#define NUM_TRACKS              32

#define MAX_MSGS       768     // number of text blocks we can build
#define MAX_TEXT       2048    // maximum characters allowed in one message block
#define MAX_NAME        128    // maximum name length for names we still copy into fixed buffers

#define MAXROOMS        512
#define MAXEXITS         29
#define EXITWORDS         2
#define HEADERWORDS       6
#define RECORDSIZE       64

// Exit entries are two words: 6 + 2*29 = 64, so MAXEXITS fills the
// record exactly and cannot be raised without changing RECORDSIZE.
//
// Do NOT re-derive the sizing from adven.dat's ROW counts. The port
// stores one entry per (motion word, cascade row), so a conditional row
// costs an entry for every word whose cascade reaches it, and a LOC's
// entry count can be far above its row count. Measured with
// Claude/SupportCode/adv_exit_budget.py: LOC 19 needs 25 entries from 8
// rows, LOC 15 needs 21 from 6, LOC 108 needs 19 from 3. 25 is the
// highest requirement in the game; 29 leaves four over it. The cost of
// each extra exit is one bank-1 word in adventure.am1's dmoCand
// candidate buffer.
//
// Exit-entry packing, DEC bit numbering (bit 0 is the word's MSB). The
// GET_* macros emitDrumLayout() writes into the generated defines file
// unpack exactly these fields; the two must be changed together.
//   word 0:   0:6  direction code    EXIT_DIR_SHIFT / EXIT_DIR_MASK
//             7:11 condition ID      EXIT_COND_SHIFT / EXIT_COND_MASK
//            12:17 rand threshold    EXIT_THRESH_MASK, 0 unless weighted
//   word 1:   0:9  message index     EXIT_MSGIDX_SHIFT, 1-based, 0 = none
//            10:17 destination room  EXIT_DEST_MASK, 0 if the row names none
//
// Seven direction bits is the floor, not a convenience: adven.dat's
// travel table uses 75 distinct motion codes with a highest value of 77,
// so no renumbering fits them in six.
#define EXIT_DIR_SHIFT      11
#define EXIT_DIR_MASK     0x7F
#define EXIT_COND_SHIFT      6
#define EXIT_COND_MASK    0x1F
#define EXIT_THRESH_MASK  0x3F
#define EXIT_MSGIDX_SHIFT    8
#define EXIT_MSGIDX_MASK 0x3FF
#define EXIT_DEST_MASK    0xFF

#define COND_RAND_ID      4
#define RAND_DOMAIN      32

// Message-only exit rows -- a motion that prints a message and leaves
// the player where they are, with no destination. adven.dat spells these
// N>500 (the destination IS a message number); the port needs them for
// the plain refusals and for the msg-56/126 random bounces at Witt's End,
// Bedquilt and Swiss Cheese.
//
// Like COND_RAND_ID these are internal condition IDs, never written as a
// `cond` name in the corpus and never present in condTable[], so they
// need no condFlagAddrs entry -- doMove special-cases both before the
// dispatch table is ever indexed.
//
// COND_MSG_ID:     unconditional. destNum is 0; the message-index field
//                  names what to print.
// COND_RANDMSG_ID: weighted. destNum is 0, and the roll threshold sits
//                  in the same field a plain RAND row uses; the 10-bit
//                  message index leaves that field free.
//
// Neither row names a room, so every exit-table walker that reads a
// destination must skip both IDs -- their destination field reads 0.
// There are three: doMove's dmHit, dwMoveOne's dmoScanCheck and doBack's
// dbScan. See adventure.am1.
#define COND_MSG_ID       9
#define COND_RANDMSG_ID  10

// The four shapes a boolean condTable row can take, and how doMove tells
// them apart. No extra condition ID is needed: the destination and
// message fields of word 1 carry everything, so the same condId 1..8
// covers all four:
//
//   dest != 0, msg != 0   `exit D DEST cond C msg M`
//                         pass -> move to DEST; fail -> print M, stay.
//                         The original form; the row ends the cascade.
//
//   dest != 0, msg == 0   `exit D DEST cond C`
//                         pass -> move to DEST; fail -> SILENTLY fall
//                         through to the next entry for this direction.
//                         adven.f4 label 12's own behavior.
//
//   dest == 0, msg != 0   `exit D none cond C msg M`
//                         pass -> print M and stay; fail -> fall through
//                         silently.  The gated form of the plain
//                         `exit D none msg M` row above.
//
//   dest == 0, msg == 0   unwritable: no grammar production spells it, so
//                         it is a syntax error rather than a semantic
//                         check. A row that neither moves nor prints has
//                         no outcome whichever way the test goes, and
//                         doMove relies on it not existing -- dmBlocked
//                         assumes a message is always there to print.
//
// The reading rule is the one the message-only rows already established:
// on a row that names a destination, `msg` is the REFUSAL text; on a
// `none` row, `msg` IS the row's action. `cond` gates the row's own
// action in every case, exactly as adven.f4 label 11 does.
//
// Consequence for exit-table walkers: "names no room" is `dest == 0`,
// NOT `condId >= COND_MSG` -- a `dest == 0, msg != 0` row carries an
// ordinary boolean condition ID. dwMoveOne's dmoScanCheck and doBack's
// dbScan test the destination field itself for that reason.

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

// One row of the two-column objAlias table: a second vocabulary word for
// an object that already owns an objNames row. Only the object's INDEX
// is kept -- findObj re-derives objLoc and
// objTake from their table bases once it has the index, exactly as its
// own in-order walk does, so an alias hit and a name hit leave the
// caller's pointers in identical shape.
typedef struct {
    char *vocSymP;      // vocabulary word symbol text, e.g. "voc_lante"
    int  objIndex;      // OBJ_* index of the object this word also names
    char *objNameP;     // that object's corpus name, for the emitted comment
} Alias, *AliasP;

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
