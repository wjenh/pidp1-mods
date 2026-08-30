#ifndef ADVDATALOADER_H
#define ADVDATALOADER_H
// Shared data between advdataloader components.

#include "symtab.h"

typedef int Word;      // Used to represent a pdp-1 word, but remember only 18 bits are significant.

// ---------------------------------------------------------------------
// Drum layout constants -- THE single source of truth (28-Aug-26,
// owner-directed layout rework; see DRUM-LAYOUT-RESULTS.md). This tool
// emits them into adv_drumlayout.ah for adventure.am1 to consume, and
// uses the same values for its own text/room placement, so the game
// and the drum image can never disagree.
//
// New layout (replacing the old whole-track-per-feature scheme where
// SAVE owned all of track 16, WIZCOM all of track 17, and text started
// at 18): SAVE and WIZCOM are fixed-size blocks at the FRONT of track
// 16, and the message text starts immediately after them on the same
// track. The SAVE block keeps track 16 offset 0 -- exactly where the
// old layout kept it -- so existing saved games remain readable; only
// WIZCOM moved (its record is rebuilt by MAINT / poof defaults).
// ---------------------------------------------------------------------
#define SAVE_TRACK           16   // front track: SAVE block, WIZCOM block, then text
#define SAVE_BLOCK_WORDS     512  // fixed; adventure.am1's SAVE records use ~268
#define WIZCOM_BLOCK_WORDS   512  // fixed; WIZCOM records use ~251
#define WIZCOM_BASE_OFFSET   SAVE_BLOCK_WORDS
#define DRUM_FRONT_WORDS     (SAVE_BLOCK_WORDS + WIZCOM_BLOCK_WORDS)
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
// PIRATE_FORBID -- TASK-PIRATE-FORBIDDEN-ZONE.md / OMISSIONS.md #22: the
// source's COND-bit-3 dwarf/pirate exclusion zone (18 rooms the source's
// dwarf-stuff block, and the pirate's own candidate movement, never
// enters/acts in while the player is there -- adven.f4 lines 638/657/697).
// Next free bit after SURFACE_FLAG (bit 2), per TASK-ROOM-FLAG-WORD's own
// "bits 3-17 reserved" note.
#define PIRATE_FORBID_FLAG 0040000     // DEC bit 3 in the pdp-1 word

typedef struct {
    SymNodeP symP;      // Symbol table entry for this block
    char text[MAX_TEXT]; // The text of the message: raw-joined at parse time,
                          // then normalized/escaped in place before packing.
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
    SymNodeP flagP;              // The flag associated with this attribute
    int value;                   // The value the flag has in this use, typically a 1/0 for yes/no
} Attribute, *AttributeP;

// One EXIT row within a room. Direction code, condition id and (when
// applicable) the condition's block message are all resolved
// immediately at parse time (the hard-coded dirTable/condTable and the
// messages section are always fully known by the time rooms are
// parsed). Only the destination room name is a genuine forward
// reference -- resolved after the whole file has been parsed, exactly
// like advroomloader.c's two-pass resolveRooms().
typedef struct {
    char *destNameP;           // destination room name, as written in the source
    int  destLine;              // source line number, for a post-parse error message
    int  destNum;                // resolved destination room number (post-parse)
    int  dirCode;                 // resolved immediately against the hard-coded dirTable
    int  condId;                   // 0 = unconditional, COND_RAND_ID = rand, else condTable id
    MessageBlockP condMsgP;         // resolved block message for a COND exit, else NULL
    int  randThreshold;              // valid only when condId == COND_RAND_ID
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

// One 'object' statement from the .adv objects section (SPEC-PHASE2.md).
// Every field below is already resolved/formatted to its final printable
// table-row text at parse time (objects have no forward references, unlike
// rooms -- the whole messages section is always parsed before objects), so
// emission just walks this array and prints. index is 0-based, order of
// appearance == the value baked into the generated OBJ_<NAME> define.
typedef struct {
    SymNodeP symP;              // Symbol table entry for this object's index name (e.g. KEYS)
    int  index;                 // 0-based index, order of appearance
    char *vocSymP;               // vocabulary word symbol text -- objNames row ("name" field)
    char *locTextP;               // objLoc row value, verbatim symbol or number text ("loc" field)
    int  take;                     // 0/1 -- objTake row value
    char *invMsgTextP;              // objInvMsg row value: "msg_<NAME>:1" or "0" for %none
    char *hereMsgTextP;              // objHereMsg row value: "msg_<NAME>:1" or "0" for %none
    char *treasureTextP;             // objTreasure row value: "1"/"0" for yes/no, or "0d<K>"/"0"
                                       // for an explicit integer (see addObjectDef's header comment
                                       // on why treasure isn't purely boolean)
} Object, *ObjectP;

// One 'verb' statement from the .adv verbs section (TASK-VERB-EMISSION.md).
// Mirrors the object statement's philosophy: every field is resolved/
// formatted to its final printable row text immediately at parse time
// (verbs have no forward references either -- messages are always fully
// parsed by the time verbs are), and the handler field is emitted
// VERBATIM with NO validation against any known-handler list, same as
// objects' loc/msg fields -- the generator does not interpret it, the
// assembler is the final arbiter.
typedef enum {
    VERBARG_MOVE,    // argspec was 'move <DIRNAME>' -- argTextP becomes "DIR_<DIRNAME>"
    VERBARG_NONE,    // argspec was 'none' -- argTextP becomes "0"
    VERBARG_MSGREF,  // argspec was 'msgref <msgname>' -- argTextP becomes "msg_<msgname>:1"
    VERBARG_KARG     // argspec was 'karg <n>' -- argTextP becomes the literal integer text
} VerbArgKind;

// Built by the argSpec grammar action and consumed immediately by
// addVerbDef(); not retained past that call.
typedef struct {
    VerbArgKind kind;
    char *strVal;    // direction suffix (MOVE) or message name (MSGREF), else NULL
    int  intVal;     // literal value (KARG), else unused
} VerbArg, *VerbArgP;

typedef struct {
    SymNodeP symP;              // Symbol table entry for this verb's unique key name
                                  // (e.g. NORTH, EXAMI) -- catches duplicate corpus rows
    char *vocTextP;               // word 0 of the emitted row: "voc_<vocword>:2", verbatim
    char *argTextP;                // word 1: "DIR_<X>" / "0" / "msg_<X>:1" / a literal int,
                                     // per the VerbArg the argspec grammar resolved
    char *handlerTextP;              // word 2: "<handler>:0", emitted verbatim (see comment above)
} Verb, *VerbP;

// verror()/verrorAt() report an error (printf-style) and call fail(),
// which never returns. verror() uses the parser's current line number
// (yylineno); verrorAt() takes an explicit line number for errors
// raised after the parse has completed (e.g. forward-reference
// resolution), so those errors can still point at the source line that
// caused them.
void verror(const char *msgP, ...);
void verrorAt(int lineno, const char *msgP, ...);

#endif
