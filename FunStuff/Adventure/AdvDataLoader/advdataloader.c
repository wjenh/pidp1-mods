/*
 * This program processes a definition file for all of the game text, vocabulary,
 * objects, and rooms to generate include files for the adventure program
 * to use, as well as preloading the Type 23 drum with the text and room definitions.
 *
 * The definition file sections, in order, are:
 *   messages movement flags actions rooms objects verbs
 * "messages" is required; every section after it is optional.
 *
 * Usage: advdataloader [-s starttrack] [-c] srcfile
 * 22-Aug-26 wje initial version
 * 28-Aug-26 wje add full generation
 * 29-Aug-26 wje change to emit a track image, don't do a drum update
 * 31-Aug-26 wje bundle all the separate .ah files into one, silly to separate them
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/types.h>

#include "advdataloader.h"
#include "symtab.h"

#define DEFAULT_TRACK_FILE   "advtracks.drm"

// Include files that contain only definitions and do not allocate memory are named .ah,
// those that allocate memory are named .ac.
#define DEFINES_OUTFILE "adv_defines.ah"

#define MSGTAB_OUTFILE  "adv_msgtab.ac"
#define VERBTAB_OUTFILE "adv_verbtab.ac"
#define SURFACEBITMAP_OUTFILE "adv_surfacebitmap.ac"

#define MAX_OBJECTS 512     // number of objects we can store
#define MAX_VERBS   512     // number of verb rows we can store (~178 today)

#ifndef NULL
#define NULL (void *)0
#endif

int trackImageFd;             // the drum track image, -1 when not open
char *filenameP;        // definition input file name
char *baseNameP;        // its basename, for generated-file header comments

// All of our symbol tables. Each symbol's ->ptr, where used, points
// back at the full record (MessageBlockP / RoomP / ObjectP) so a later
// lookup by name gets more than just the ->ival ever could.
SymNodeP msgSymsP;
SymNodeP flagSymsP;
SymNodeP actionSymsP;
SymNodeP directionSymsP;
SymNodeP roomSymsP;
SymNodeP verbSymsP;
SymNodeP objSymsP;

int numMsgs;
MessageBlock msgBlocks[MAX_MSGS];

int numRooms;
Room rooms[MAXROOMS];
static RoomP currentRoomP;     // the room currently being parsed, NULL otherwise

static Object objects[MAX_OBJECTS];
static int numObjects;

static Verb verbs[MAX_VERBS];
static int numVerbs;
static int startTrack;

char msgTextBuf[MAX_TEXT];     // accumulates one message's joined raw lines (see joinMsgLine())

// ---------------------------------------------------------------------
// Direction/condition tables: hard-coded for phase 1 (SPEC-PHASE1.md
// "Room records" -- moving either into the definition file is a
// phase-2+ owner decision). Copied verbatim, including comments, from
// AdvRoomLoader/advroomloader.c -- these two tables MUST stay in sync
// with that file (until/unless a later phase makes one of them the
// single source of truth).
// ---------------------------------------------------------------------
typedef struct {
    const char *nameP;
    int code;
} DirEnt;

typedef struct {
    const char *nameP;
    int id;
} CondEnt;

// Direction word, matched adventure.am1's DIR_* numeric code.
// Must stay in sync with adventure.am1's #define DIR_NORTH etc.
static const DirEnt dirTable[] = {
    { "NORTH", 1 },
    { "SOUTH", 2 },
    { "EAST", 3 },
    { "WEST", 4 },
    { "IN", 5 },
    { "OUT", 6 },
    { "UP", 7 },
    { "DOWN", 8 },
    { "NE", 9 },
    { "SE", 10 },
    { "SW", 11 },
    { "NW", 12 },
    /* Stage 21: fissure/rod cluster motion words. Must stay in sync with
     * adventure.am1's DIR_* #defines. */
    { "FORWARD", 13 },
    { "JUMP", 14 },
    { "OVER", 15 },
    { "ACROSS", 16 },
    { "CROSS", 17 },
    { "HALL", 18 },
    { "PASSAGE", 19 },
    { "TUNNEL", 19 },  /* true source synonym, shares DIR_PASSAGE (19) */
    { "CLIMB", 20 },
    { "CRAWL", 21 },
    /* Stage 22 (STAGE22-TASK.md): SECRE, adven.dat motion word 66,
     * R_MTKING's exit into the dragon/RUG cluster. Must stay in sync
     * with adventure.am1's DIR_SECRE #define. */
    { "SECRE", 22 },
    /* Stage 23 (STAGE23-TASK.md Parts A/B/C): the troll-bridge/volcano
     * cluster's new motion words. UPWAR/LEFT/RIGHT are true synonyms of
     * already-existing UP/NE/SE and reuse those codes directly (see
     * adventure.am1's voc_upwar/voc_left/voc_right) rather than needing
     * entries here. Must stay in sync with adventure.am1's DIR_BEDQU/
     * DIR_FORK/DIR_VIEW/DIR_BARRE #defines. */
    { "BEDQU", 23 },
    { "FORK", 24 },
    { "VIEW", 25 },
    { "BARRE", 26 },
    /* Stage 25 (STAGE25-TASK.md): ORIEN, adven.dat motion word 72,
     * R_SWISSCHEESE's real entrance into the Oriental Room/VASE puzzle.
     * Must stay in sync with adventure.am1's DIR_ORIEN #define. */
    { "ORIEN", 27 },
    /* Stage 26 (STAGE26-TASK.md): CAVER and DARK, adven.dat motion words
     * 73 and 22 -- the Oriental Room/Misty Cavern/Alcove cluster's real
     * entrance and the Plover Room's real Dark-Room entrance. Must stay
     * in sync with adventure.am1's DIR_CAVER/DIR_DARK #defines. */
    { "CAVER", 28 },
    { "DARK", 29 },
    /* Stage 29 (STAGE29-TASK.md): GIANT, adven.dat motion word 27 --
     * the Giant Room cluster's own "return to the Giant Room" synonym
     * word, used at LOC 88/93/95. Must stay in sync with
     * adventure.am1's DIR_GIANT #define. */
    { "GIANT", 30 },
    { NULL, 0 }
};

// Condition name -> condition ID.
// Must stay in the same order as adventure.am1's condFlagAddrs table.
// 0 is reserved for "unconditional" and never appears here.
// 4 (COND_RAND_ID) is reserved for the RAND special case and never
// appears here either -- Stage 21's new conditions start at 5.
static const CondEnt condTable[] = {
    { "GRATE_OPEN", 1 },
    { "SNAKE", 2 },
    { "NUGGET_TRAP", 3 },
    { "FISSURE_BRIDGE", 5 },
    { "FISSURE_NO_BRIDGE", 6 },
    /* Stage 23: gates R_CHASM_SW/R_CHASM_NE's ACROSS/CROSS/NE-or-SW rows
     * (troll gone -> allowed silent no-op, troll present -> blocked with
     * msg_TROLL_REFUSES). Must stay in sync with adventure.am1's
     * condFlagAddrs table and its trollGone var. */
    { "TROLL_GONE", 7 },
    /* Stage 29: gates R_DOORPASSAGE's NORTH/CAVER rows to R_WATERFALL
     * (RUSTY DOOR oiled -> allowed, not oiled -> blocked with
     * msg_DOOR_RUSTY). Must stay in sync with adventure.am1's
     * condFlagAddrs table and its doorOiled var. */
    { "DOOR_OILED", 8 },
    { NULL, 0 }
};

static int dirCodeForName(const char *nameP);
static int condIdForName(const char *nameP);

void addMessage(char *labelP, char *textP);
void beginMessage(void);
void joinMsgLine(char *lineP);
void addFlag(char *nameP, int value);
void addDirection(char *nameP, int value);
void addAction(char *nameP, int value);
void registerRoom(char *nameP);
void finishRoom(void);
void setRoomLongMsg(char *nameP);
void setRoomShortMsg(char *nameP);
void addRoomFlagAttr(char *flagNameP, bool value);
void addRoomExit(char *dirNameP, char *destNameP);
void addRoomExitCond(char *dirNameP, char *destNameP, char *condNameP, char *msgNameP);
void addRoomExitRand(char *dirNameP, char *destNameP, int percent);
void addObjectDef(char *nameP, char *vocSymP, char *locTextP, bool take,
    char *invMsgNameP, char *hereMsgNameP, char *treasureTextP);
void fail(void);

static void translateEscapes(MessageBlockP blockP);
static void normalizeText(MessageBlockP blockP);
static int packSixbit(const char *text, Word *outP, int *padCountP);
static void computeMessagePlacement(int startTrack);
static void resolveRoomExits(void);
static void buildRoomRecord(RoomP rP, Word *outWords);
static void doWrite(int roomBaseTrack);
static int doCompare(const char *path, int roomBaseTrack);
static void emitMsgtab(FILE *outP, int startTrack);
static void emitRoomtab(FILE *outP, int roomBaseTrack, int tracksNeeded, int maxTrack);
static void emitSurfaceBitmap(FILE *outP);
static void emitObjDefs(FILE *outP);
static void emitObjTables(char *dirnameP, FILE *deffP);
static void emitVerbTab(char *dirnameP);
static void emitDrumLayout(FILE *outP);
static void usage(void);

extern int yydebug;
extern int yy_flex_debug;
extern int yylineno;
extern FILE *yyin;     // lex input file fP

extern int yyparse();

int
main(int argc, char **argv)
{
int opt;
int i;
int maxMsgTrack;
int roomBaseTrack;
int tracksNeeded;
int diffs;
char *dirP;
bool compareMode;
FILE *outP;
FILE *defOutP;
char outPath[1024];

    yy_flex_debug = 0;
    yydebug = 0;
    compareMode = false;

    symInit(&msgSymsP);
    symInit(&flagSymsP);
    symInit(&actionSymsP);
    symInit(&directionSymsP);
    symInit(&roomSymsP);
    symInit(&verbSymsP);
    symInit(&objSymsP);

    startTrack = DEFAULT_START_TRACK;
    dirP = "";                  // by default, files go in the current dir

    while( (opt = getopt(argc, argv, "s:o:cyl")) != -1 )
    {
        switch( opt )
        {
        case 's':
            startTrack = atoi(optarg);
            break;
        case 'o':
            dirP = optarg;
            break;
        case 'c':
            compareMode = true;
            break;
        case 'y':
            yydebug = 1;
            break;
        case 'l':
            yy_flex_debug = 1;
            break;

        default:
            usage();
        }
    }

    if( optind >= argc )
    {
        usage();
    }

    trackImageFd = -1;
    filenameP = argv[optind];

    // The basename is what generated-file header comments cite -- a full
    // path would embed one machine's directory layout into files that are
    // regenerated on different machines (and churn on every cwd change).
    if( (baseNameP = strrchr(filenameP, '/')) != NULL )
    {
        ++baseNameP;
    }
    else
    {
        baseNameP = filenameP;
    }

    if( (startTrack < 0) || (startTrack >= NUM_TRACKS) )
    {
        fprintf(stderr, "Start track must be 0-%d\n", NUM_TRACKS - 1);
        fail();
    }

    if(!(yyin = fopen(filenameP, "r")))
    {
        fprintf(stderr, "advdataloader: can't open source file '%s'\n", filenameP);
        fail();
    }

    if( yyparse() )
    {
        fprintf(stderr, "Compilation failed.\n");
        fail();
    }

    fclose(yyin);
    yyin = NULL;

    if( numMsgs == 0 )
    {
        fprintf(stderr, "advdataloader: no messages found in '%s'\n", filenameP);
        fail();
    }

    // Everything from here down completes and validates entirely in
    // memory before the drum image is touched (SKELETON-EVAL 2.3).
    computeMessagePlacement(startTrack);
    resolveRoomExits();

    // Room base track = one past the highest track any message uses,
    // matching advroomloader.c's maxMsgTrack()+1 -- computed here from
    // our own in-memory placement instead of re-parsing a .ah file.
    maxMsgTrack = msgBlocks[0].track;
    for( i = 1; i < numMsgs; ++i )
    {
        if( msgBlocks[i].track > maxMsgTrack )
        {
            maxMsgTrack = msgBlocks[i].track;
        }
    }
    roomBaseTrack = maxMsgTrack + 1;

    if( (roomBaseTrack < 0) || (roomBaseTrack >= NUM_TRACKS) )
    {
        fprintf(stderr,
            "advdataloader: room table track %d (one past msgtab's highest track, %d) "
            "exceeds drum capacity (tracks are 0-%d)\n",
            roomBaseTrack, maxMsgTrack, NUM_TRACKS - 1);
        fail();
    }

    tracksNeeded = (int)(((long)numRooms * RECORDSIZE + WORDS_PER_TRACK - 1) / WORDS_PER_TRACK);
    if( tracksNeeded < 1 )
    {
        tracksNeeded = 1;   // numRooms == 0 would otherwise ask for 0 tracks
    }

    if( (roomBaseTrack + tracksNeeded - 1) >= NUM_TRACKS )
    {
        fprintf(stderr,
            "advdataloader: %d rooms * %d words needs %d track(s) starting at track %d "
            "-- exceeds drum capacity (tracks are 0-%d)\n",
            numRooms, RECORDSIZE, tracksNeeded, roomBaseTrack, NUM_TRACKS - 1);
        fail();
    }

    if( compareMode )
    {
        diffs = doCompare(DEFAULT_TRACK_FILE, roomBaseTrack);
        return( diffs ? 1 : 0 );
    }

    doWrite(roomBaseTrack);

    // All of the defines go into one file
    sprintf(outPath,"%s%s%s", dirP, (*dirP)?"/":"", DEFINES_OUTFILE);

    if( !(defOutP = fopen(outPath, "w")) )
    {
        fprintf(stderr, "Can't create output file '%s'\n", DEFINES_OUTFILE);
        fail();
    }
    emitRoomtab(defOutP, roomBaseTrack, tracksNeeded, maxMsgTrack);

    sprintf(outPath,"%s%s%s", dirP, (*dirP)?"/":"", MSGTAB_OUTFILE);
    if( !(outP = fopen(outPath, "w")) )
    {
        fprintf(stderr, "Can't create output file '%s'\n", MSGTAB_OUTFILE);
        fail();
    }
    emitMsgtab(outP, startTrack);
    fclose(outP);
    
    sprintf(outPath,"%s%s%s", dirP, (*dirP)?"/":"", SURFACEBITMAP_OUTFILE);
    if( !(outP = fopen(outPath, "w")) )
    {
        fprintf(stderr, "Can't create output file '%s'\n", SURFACEBITMAP_OUTFILE);
        fail();
    }
    emitSurfaceBitmap(outP);
    fclose(outP);

    // Objects section is optional (SPEC-PHASE1.md); its seven
    // generated files are only written when it's actually used
    // (SPEC-PHASE2.md "Emission only when the objects section is
    // non-empty").
    if( numObjects > 0 )
    {
        emitObjTables(dirP, defOutP);
    }

    // Verbs section is likewise optional; adv_verbtab.ac is only
    // written when it's actually used (TASK-VERB-EMISSION.md, same
    // policy as objects).
    if( numVerbs > 0 )
    {
        emitVerbTab(dirP);
    }

    emitDrumLayout(defOutP);

    printf("Wrote %d text blocks to tracks %d-%d\n", numMsgs, startTrack, maxMsgTrack);

    if( tracksNeeded > 1 )
    {
        printf("Wrote %d room records (%d words each) to tracks %d-%d\n",
            numRooms, RECORDSIZE, roomBaseTrack, roomBaseTrack + tracksNeeded - 1);
    }
    else
    {
        printf("Wrote %d room records (%d words each) to track %d\n",
            numRooms, RECORDSIZE, roomBaseTrack);
    }

    if( numObjects > 0 )
    {
        printf("Wrote %d object definitions to '%s' + 6 table .ac files\n", numObjects, DEFINES_OUTFILE);
    }

    if( numVerbs > 0 )
    {
        printf("Wrote %d verb rows to '%s'\n", numVerbs, VERBTAB_OUTFILE);
    }

    fclose(defOutP);
    return(0);
}

// ---------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------

// Start accumulating a new message's text (called when 'message NAME' is reduced).
void
beginMessage(void)
{
    msgTextBuf[0] = 0;
}

// Join one more raw source line into the message currently being
// accumulated. Lines are joined with a single space, unconditionally,
// between non-empty lines -- the join space after a line ending in a
// '\n' escape is swallowed later by translateEscapes(), exactly as the
// retired advtextloader's pipeline did. A genuinely blank raw line
// (shouldn't reach here -- the lexer already skips those) contributes
// nothing.
//
// History (28-Aug-26, after the legacy loaders' retirement): this used
// to also emulate advtextloader.c's 1024-byte fgets buffer, which
// silently split any >1023-byte physical line and re-joined the pieces
// with a phantom space -- an accidental artifact the phase-1
// byte-identity rail required reproducing (the corpus's HELP message
// tripped it, printing one mid-word space). With byte-identity to the
// retired tools no longer a requirement, the emulation is gone: long
// lines join naturally, and HELP prints without the phantom space.
void
joinMsgLine(char *lineP)
{
    if( lineP[0] == 0 )
    {
        return;
    }

    if( msgTextBuf[0] )
    {
        strncat(msgTextBuf, " ", MAX_TEXT - strlen(msgTextBuf) - 1);
    }

    strncat(msgTextBuf, lineP, MAX_TEXT - strlen(msgTextBuf) - 1);
}

// Register the message just closed (block close = ENDMSG). textP is
// the fully joined raw text (still containing literal "\n" two-char
// escapes, untouched) -- translateEscapes()/normalizeText()/packSixbit()
// run later, during placement, exactly matching advtextloader.c's order.
void
addMessage(char *labelP, char *textP)
{
MessageBlockP blockP;
SymNodeP symP;

    if( numMsgs >= MAX_MSGS )
    {
        verror("Too many messages, the limit is %d.\n", MAX_MSGS);
    }

    symP = symMake(labelP);
    if( !symAdd(&msgSymsP, symP) )
    {
        verror("Message '%s' has already been defined.\n", labelP);
    }

    blockP = &msgBlocks[numMsgs++];
    blockP->symP = symP;
    blockP->track = 0;
    blockP->offset = 0;
    blockP->nWords = 0;
    blockP->padCount = 0;

    strncpy(blockP->text, textP, MAX_TEXT - 1);
    blockP->text[MAX_TEXT - 1] = 0;
    // Every printed message ends on its own line: bake the newline
    // into the text itself at block close (advtextloader.c).
    strncat(blockP->text, "\n", MAX_TEXT - strlen(blockP->text) - 1);

    symP->ptr = blockP;    // so a later lookup-by-name gets the block directly
}

// ---------------------------------------------------------------------
// Movement / flags / actions -- simple named-value symbol tables.
// ---------------------------------------------------------------------

void
addFlag(char *nameP, int value)
{
SymNodeP symP;

    symP = symMake(nameP);
    symP->ival = value;
    if( !symAdd(&flagSymsP, symP) )
    {
        verror("Flag '%s' has already been defined.\n", nameP);
    }
}

// The movement section is parsed and validated against the hard-coded
// dirTable[] above (names/values must match exactly, SPEC-PHASE1.md),
// but dirTable[] -- not this symbol table -- is what actually gets used
// to pack exits, so a mismatch here is caught immediately rather than
// silently diverging from what the packed data will say.
void
addDirection(char *nameP, int value)
{
SymNodeP symP;
int i;
bool found;

    symP = symMake(nameP);
    symP->ival = value;
    if( !symAdd(&directionSymsP, symP) )
    {
        verror("Direction '%s' has already been defined.\n", nameP);
    }

    found = false;
    for( i = 0; dirTable[i].nameP; ++i )
    {
        if( strcmp(dirTable[i].nameP, nameP) == 0 )
        {
            found = true;
            if( dirTable[i].code != value )
            {
                verror("Direction '%s' = %d does not match the hard-coded dirTable value %d.\n",
                    nameP, value, dirTable[i].code);
            }
            break;
        }
    }

    if( !found )
    {
        verror("Direction '%s' is not in the hard-coded dirTable (advdataloader.c).\n", nameP);
    }
}

void
addAction(char *nameP, int value)
{
SymNodeP symP;

    symP = symMake(nameP);
    symP->ival = value;
    if( !symAdd(&actionSymsP, symP) )
    {
        verror("Action '%s' has already been defined.\n", nameP);
    }
}

// ---------------------------------------------------------------------
// Rooms
// ---------------------------------------------------------------------

static int
dirCodeForName(const char *nameP)
{
int i;

    for( i = 0; dirTable[i].nameP; ++i )
    {
        if( strcmp(dirTable[i].nameP, nameP) == 0 )
        {
            return( dirTable[i].code );
        }
    }

    return( 0 );    // 0 = not found, never a legal direction code
}

static int
condIdForName(const char *nameP)
{
int i;

    for( i = 0; condTable[i].nameP; ++i )
    {
        if( strcmp(condTable[i].nameP, nameP) == 0 )
        {
            return( condTable[i].id );
        }
    }

    return( -1 );   // -1 = not found, no ID has this value.
}

// Register a new room (room NAME reduction), numbered by order of
// appearance, 1-based. Becomes the "current room" that the rest of the
// roomspec actions (flag/longmsg/shortmsg/exit) apply to.
void
registerRoom(char *nameP)
{
SymNodeP symP;
RoomP roomP;

    if( numRooms >= MAXROOMS )
    {
        verror("Too many rooms, the limit is %d.\n", MAXROOMS);
    }

    symP = symMake(nameP);
    if( !symAdd(&roomSymsP, symP) )
    {
        verror("Room '%s' has already been defined.\n", nameP);
    }

    roomP = &rooms[numRooms++];
    memset(roomP, 0, sizeof(*roomP));
    roomP->symP = symP;
    roomP->num = numRooms;     // 1-based, by order of appearance

    symP->ptr = roomP;
    currentRoomP = roomP;
}

// Finish the room just closed (END reduction): validate the required
// LONG message was given, and default SHORT to LONG if omitted
// (legacy "SHORT SAME" behavior).
void
finishRoom(void)
{
    if( !currentRoomP->longMsgP )
    {
        verror("Room '%s' has no LONG message\n", currentRoomP->symP->nameP);
    }

    if( !currentRoomP->shortMsgP )
    {
        currentRoomP->shortMsgP = currentRoomP->longMsgP;
    }

    currentRoomP = NULL;
}

void
setRoomLongMsg(char *nameP)
{
SymNodeP symP;

    if( !(symP = symFind(&msgSymsP, nameP)) )
    {
        verror("Room '%s': LONG message '%s' not found\n", currentRoomP->symP->nameP, nameP);
    }

    currentRoomP->longMsgP = (MessageBlockP)symP->ptr;
}

void
setRoomShortMsg(char *nameP)
{
SymNodeP symP;

    if( !(symP = symFind(&msgSymsP, nameP)) )
    {
        verror("Room '%s': SHORT message '%s' not found\n", currentRoomP->symP->nameP, nameP);
    }

    currentRoomP->shortMsgP = (MessageBlockP)symP->ptr;
}

// "flag NAME yes|no" -- NAME must be a defined flag; on yes, its value
// (mask) gets OR'd into word0 at record-build time. That's the whole
// of room-flag semantics (SPEC-PHASE1.md).
void
addRoomFlagAttr(char *flagNameP, bool value)
{
AttributeP aP;
SymNodeP symP;

    if( !(symP = symFind(&flagSymsP, flagNameP)) )
    {
        verror("Room '%s': flag '%s' is not defined\n", currentRoomP->symP->nameP, flagNameP);
    }

    aP = (AttributeP)malloc(sizeof(Attribute));
    aP->flagP = symP;
    aP->value = value ? 1 : 0;
    aP->nextP = currentRoomP->attributesP;
    currentRoomP->attributesP = aP;
}

// Common setup for a new exit row: bounds-check, resolve the direction
// code immediately (the hard-coded dirTable is always fully known), and
// stash the destination room's name + source line -- the only thing an
// exit needs resolved AFTER the whole parse, since forward references
// to rooms defined later in the file are normal.
static ExitP
newRoomExit(char *dirNameP, char *destNameP)
{
ExitP eP;
int code;

    if( currentRoomP->nexits >= MAXEXITS )
    {
        verror("Room '%s' has more than %d exits (MAXEXITS)\n", currentRoomP->symP->nameP, MAXEXITS);
    }

    if( !(code = dirCodeForName(dirNameP)) )
    {
        verror("Room '%s': EXIT direction '%s' is not recognized\n", currentRoomP->symP->nameP, dirNameP);
    }

    eP = &currentRoomP->exits[currentRoomP->nexits++];
    eP->destNameP = destNameP;
    eP->destLine = yylineno;
    eP->destNum = 0;
    eP->dirCode = code;
    eP->condId = 0;
    eP->condMsgP = NULL;
    eP->randThreshold = 0;

    return( eP );
}

void
addRoomExit(char *dirNameP, char *destNameP)
{
    newRoomExit(dirNameP, destNameP);
}

void
addRoomExitCond(char *dirNameP, char *destNameP, char *condNameP, char *msgNameP)
{
ExitP eP;
int condId;
SymNodeP symP;

    eP = newRoomExit(dirNameP, destNameP);

    if( (condId = condIdForName(condNameP)) < 0 )
    {
        verror("Room '%s': EXIT %s condition '%s' is not a known condition "
            "(add it to advdataloader.c's condTable[] and adventure.am1's "
            "condFlagAddrs, in the same order)\n",
            currentRoomP->symP->nameP, dirNameP, condNameP);
    }

    if( !(symP = symFind(&msgSymsP, msgNameP)) )
    {
        verror("Room '%s': EXIT %s block message '%s' not found\n",
            currentRoomP->symP->nameP, dirNameP, msgNameP);
    }

    eP->condId = condId;
    eP->condMsgP = (MessageBlockP)symP->ptr;
}

void
addRoomExitRand(char *dirNameP, char *destNameP, int percent)
{
ExitP eP;

    if( (percent < 0) || (percent > 100) )
    {
        verror("Room '%s': EXIT %s RAND percent %d is not 0-100\n",
            currentRoomP->symP->nameP, dirNameP, percent);
    }

    eP = newRoomExit(dirNameP, destNameP);
    eP->condId = COND_RAND_ID;
    eP->randThreshold = (percent * RAND_DOMAIN + 50) / 100;    // rounded, not truncated
}

// Pass 2: resolve every exit's destination room name, now that the
// whole file (hence every room) has been parsed. Uses verrorAt() with
// the line the EXIT was written on, since yylineno itself now just
// points at end-of-file.
static void
resolveRoomExits(void)
{
int i, j;
RoomP rP;
ExitP eP;
SymNodeP symP;

    for( i = 0; i < numRooms; ++i )
    {
        rP = &rooms[i];

        for( j = 0; j < rP->nexits; ++j )
        {
            eP = &rP->exits[j];

            if( !(symP = symFind(&roomSymsP, eP->destNameP)) )
            {
                verrorAt(eP->destLine, "Room '%s': EXIT destination '%s' is not a defined room\n",
                    rP->symP->nameP, eP->destNameP);
            }

            eP->destNum = ((RoomP)symP->ptr)->num;
        }
    }
}

// Build one room's 64-word drum record, exactly matching
// advroomloader.c's buildRecord().
static void
buildRoomRecord(RoomP rP, Word *outWords)
{
int i, base;
Word word;
AttributeP aP;
ExitP eP;

    memset(outWords, 0, RECORDSIZE * sizeof(Word));

    outWords[0] = rP->num | (rP->nexits << 8);
    outWords[5] = 0;   // explicit for clarity; already 0 via the memset above
    for( aP = rP->attributesP; aP; aP = aP->nextP )
    {
        if( aP->value )
        {
            outWords[5] |= aP->flagP->ival;
        }
    }

    outWords[1] = (rP->longMsgP->track << 12) + rP->longMsgP->offset;
    outWords[2] = (rP->longMsgP->padCount << 12) | rP->longMsgP->nWords;
    outWords[3] = (rP->shortMsgP->track << 12) + rP->shortMsgP->offset;
    outWords[4] = (rP->shortMsgP->padCount << 12) | rP->shortMsgP->nWords;

    for( i = 0; i < rP->nexits; ++i )
    {
        eP = &rP->exits[i];
        base = HEADERWORDS + (i * EXITWORDS);

        word = eP->destNum;
        word |= eP->condId << 8;
        word |= eP->dirCode << 13;
        outWords[base] = word;

        if( eP->condId == COND_RAND_ID )
        {
            // Reuse the block-message doublet's first word for the roll
            // threshold -- a RAND row never prints a block message on a
            // miss, it silently falls through, so that slot is otherwise
            // unused for this condition.
            outWords[base + 1] = eP->randThreshold;
        }
        else if( eP->condId != 0 )
        {
            outWords[base + 1] = (eP->condMsgP->track << 12) + eP->condMsgP->offset;
            outWords[base + 2] = (eP->condMsgP->padCount << 12) | eP->condMsgP->nWords;
        }
        // else: already zeroed by the memset above
    }
}

// ---------------------------------------------------------------------
// Objects (SPEC-PHASE2.md) -- one flat 'object' statement per row of the
// six parallel object tables (objLoc/objTake/objNames/objInvMsg/
// objHereMsg/objTreasure), file order = OBJ_* index order. Unlike rooms,
// nothing here is a forward reference: the whole messages section is
// always fully parsed by the time objects are, so every field is
// resolved/formatted to its final table-row text right here, once,
// rather than needing a second resolution pass.
// ---------------------------------------------------------------------

// Format one invmsg/heremsg field: NULL (the grammar's %none/NONE case)
// becomes the literal row text "0"; otherwise the row is "msg_<name>:1",
// same shape as the hand table's rows. The message name is NOT required
// to exist (SPEC-PHASE2.md) -- warn only, since the emission is symbolic
// text and the assembler is the final arbiter of whether msg_<name>
// actually resolves.
static char *
formatMsgField(const char *objNameP, const char *fieldNameP, char *msgNameP)
{
char buf[MAX_NAME + 8];
char *textP;

    if( !msgNameP )
    {
        textP = (char *)malloc(2);
        strcpy(textP, "0");
        return( textP );
    }

    if( !symFind(&msgSymsP, msgNameP) )
    {
        fprintf(stderr, "Warning: object '%s' %s '%s' is not a defined message (line %d)\n",
            objNameP, fieldNameP, msgNameP, yylineno);
    }

    sprintf(buf, "msg_%s:1", msgNameP);
    textP = (char *)malloc(strlen(buf) + 1);
    strcpy(textP, buf);
    return( textP );
}

void
addObjectDef(char *nameP, char *vocSymP, char *locTextP, bool take,
    char *invMsgNameP, char *hereMsgNameP, char *treasureTextP)
{
ObjectP objP;
SymNodeP symP;

    if( numObjects >= MAX_OBJECTS )
    {
        verror("Too many objects, the limit is %d.\n", MAX_OBJECTS);
    }

    symP = symMake(nameP);
    if( !symAdd(&objSymsP, symP) )
    {
        verror("Object '%s' has already been defined.\n", nameP);
    }

    objP = &objects[numObjects];
    objP->symP = symP;
    objP->index = numObjects;
    objP->vocSymP = vocSymP;
    objP->locTextP = locTextP;
    objP->take = take ? 1 : 0;
    objP->invMsgTextP = formatMsgField(nameP, "invmsg", invMsgNameP);
    objP->hereMsgTextP = formatMsgField(nameP, "heremsg", hereMsgNameP);
    objP->treasureTextP = treasureTextP;

    symP->ptr = objP;
    ++numObjects;
}

// ---------------------------------------------------------------------
// Verbs (TASK-VERB-EMISSION.md) -- one flat 'verb' statement per row of
// the single verbTab table (3 words/row: voc_*:2, argument, handler:0).
// Same no-forward-references situation as objects, so everything is
// resolved/formatted to its final row text right here. The handler field
// is stored verbatim, deliberately unvalidated -- see the Verb struct's
// comment in advdataloader.h for why.
// ---------------------------------------------------------------------

void
addVerbDef(char *nameP, char *vocWordP, VerbArgP argP, char *handlerP)
{
VerbP verbP;
SymNodeP symP;
char buf[MAX_NAME + 8];

    if( numVerbs >= MAX_VERBS )
    {
        verror("Too many verbs, the limit is %d.\n", MAX_VERBS);
    }

    symP = symMake(nameP);
    if( !symAdd(&verbSymsP, symP) )
    {
        verror("Verb '%s' has already been defined.\n", nameP);
    }

    verbP = &verbs[numVerbs];
    verbP->symP = symP;

    sprintf(buf, "voc_%s:2", vocWordP);
    verbP->vocTextP = (char *)malloc(strlen(buf) + 1);
    strcpy(verbP->vocTextP, buf);

    switch( argP->kind )
    {
    case VERBARG_MOVE:
        sprintf(buf, "DIR_%s", argP->strVal);
        verbP->argTextP = (char *)malloc(strlen(buf) + 1);
        strcpy(verbP->argTextP, buf);
        break;

    case VERBARG_NONE:
        verbP->argTextP = (char *)malloc(2);
        strcpy(verbP->argTextP, "0");
        break;

    case VERBARG_MSGREF:
        // Reuses the exact same formatting/warning logic objects' invmsg/
        // heremsg fields use, so a verb's message reference round-trips
        // identically to an object's (same "msg_<name>:1" shape, same
        // warn-but-don't-fail policy since this is symbolic text and the
        // assembler is the final arbiter).
        verbP->argTextP = formatMsgField(nameP, "msgref", argP->strVal);
        break;

    case VERBARG_KARG:
        sprintf(buf, "%d", argP->intVal);
        verbP->argTextP = (char *)malloc(strlen(buf) + 1);
        strcpy(verbP->argTextP, buf);
        break;
    }

    sprintf(buf, "%s:0", handlerP);
    verbP->handlerTextP = (char *)malloc(strlen(buf) + 1);
    strcpy(verbP->handlerTextP, buf);

    symP->ptr = verbP;
    ++numVerbs;
}

// ---------------------------------------------------------------------
// Text pipeline -- lifted UNCHANGED in behavior from
// AdvTextLoader/advtextloader.c (SPEC-PHASE1.md "Text pipeline").
// ---------------------------------------------------------------------

// Translate the standard '\n' escape convention, replacing each
// two-character sequence with a single newline byte
// and swallow any spaces immediately following.
// A lone trailing backslash (no following 'n') is left as-is and passed
// through literally.
static void
translateEscapes(MessageBlockP blockP)
{
char buf[MAX_TEXT];
const char *src = blockP->text;
char *dst = buf;

    while( *src )
    {
        if( (src[0] == '\\') && (src[1] == 'n') )
        {
            *dst++ = '\n';
            src += 2;

            while( *src == ' ' )
            {
                ++src;      // swallow the word-wrap join space, if any
            }
        }
        else
        {
            *dst++ = *src++;
        }
    }

    *dst = 0;
    strncpy(blockP->text, buf, MAX_TEXT - 1);
    blockP->text[MAX_TEXT - 1] = 0;
}

// Upper-case a block's text in place, warning about anything unusual.
static void
normalizeText(MessageBlockP blockP)
{
char *cP;

    for( cP = blockP->text; *cP; ++cP )
    {
        if( islower((unsigned char)*cP) )
        {
            *cP = toupper((unsigned char)*cP);
        }
        else if( (*cP != '\n') && !isprint((unsigned char)*cP) )
        {
            // '\n' is valid, everything else non-printable
            // is unexpected, give a warning.
            fprintf(stderr, "Warning: block '%s' has a non-printable byte 0%03o, left as-is\n",
                blockP->symP->nameP, (unsigned char)*cP);
        }
    }
}

// Pack text into Word[] using DEC-SIXBIT-style packing: 3 characters
// per 18-bit word, (ascii-32)&077 per character, high to low. Every
// byte must already be normalized (uppercase, or '\n') by the time this
// runs -- normalizeText() guarantees that. '_' is reserved as the
// embedded-newline sentinel and is an ERROR if it appears literally
// (see AdvTextLoader/advtextloader.c's packSixbit() header comment for
// the full rationale).
//
// Returns the number of words written into outP (caller must ensure
// room for at least (nchars/3)+1 words) and, via *padCountP, how many
// of the LAST word's 3 character slots are unused padding (0, 1, or 2).
static int
packSixbit(const char *text, Word *outP, int *padCountP)
{
int nchars;
int i, w;
int slot;
Word vals[3];
unsigned char c;

    nchars = (int)strlen(text);

    for( w = i = 0; i < nchars; )
    {
        for( slot = 0; slot < 3; ++slot )
        {
            if( i < nchars )
            {
                c = (unsigned char)text[i++];

                if( c == '\n' )
                {
                    vals[slot] = 077;       // '_' stand-in for embedded newline
                }
                else if( c == '_' )
                {
                    fprintf(stderr,
                        "Error: literal '_' found in packed text -- '_' is reserved "
                        "as the embedded-newline sentinel and cannot appear as a "
                        "real character (see packSixbit's header comment)\n");
                    exit(1);
                }
                else if( (c < 32) || (c > 95) )
                {
                    fprintf(stderr,
                        "Error: character 0%03o out of sixbit range (32-95) in "
                        "packed text -- was normalizeText skipped?\n", c);
                    exit(1);
                }
                else
                {
                    vals[slot] = c - 32;
                }
            }
            else
            {
                vals[slot] = 0;             // padding
            }
        }

        outP[w++] = (vals[0] << 12) | (vals[1] << 6) | vals[2];
    }

    *padCountP = (nchars % 3) ? (3 - (nchars % 3)) : 0;
    return( w );
}

// ---------------------------------------------------------------------
// Message placement -- pack FIRST, then track-fit check/bump, then
// record track/offset, then account (SKELETON-EVAL 2.2's fix), matching
// advtextloader.c's loop order and its never-span-a-track rule exactly.
// ---------------------------------------------------------------------
static void
computeMessagePlacement(int startTrack)
{
int i;
int track, offset;
int nwords;
Word packed[(MAX_TEXT / 3) + 2];
MessageBlockP blockP;

    // Translate '\n' escapes before any other processing, for every
    // block -- same order as advtextloader.c's main().
    for( i = 0; i < numMsgs; ++i )
    {
        translateEscapes(&msgBlocks[i]);
    }

    track = startTrack;
    offset = (startTrack == SAVE_TRACK) ? DRUM_START_WORDS : 0;

    for( i = 0; i < numMsgs; ++i )
    {
        blockP = &msgBlocks[i];
        normalizeText(blockP);
        nwords = packSixbit(blockP->text, packed, &blockP->padCount);     // pack FIRST

        if( nwords > WORDS_PER_TRACK )
        {
            verror("Block '%s', %d words, is larger than a track, stopping.\n",
                blockP->symP->nameP, nwords);
        }

        if( (offset + nwords) > WORDS_PER_TRACK )
        {
            // Doesn't fit in the remainder of this track, move to the next one.
            ++track;
            offset = 0;

            if( track >= NUM_TRACKS )
            {
                verror("Ran out of drum tracks (used %d..%d) placing block '%s'.\n",
                    startTrack, NUM_TRACKS - 1, blockP->symP->nameP);
            }
        }

        blockP->track = track;     // record AFTER the possible bump
        blockP->offset = offset;
        blockP->nWords = nwords;

        offset += nwords;          // account
    }
}

// ---------------------------------------------------------------------
// Drum image I/O
// ---------------------------------------------------------------------

static void
doWrite(int roomBaseTrack)
{
int i;
int discardPad;
int nwords;
off_t byteOffset;
MessageBlockP blockP;
Word packed[(MAX_TEXT / 3) + 2];
Word rec[RECORDSIZE];

    if( (trackImageFd = open(DEFAULT_TRACK_FILE, O_CREAT | O_WRONLY, 0666)) < 0 )
    {
        fprintf(stderr, "Can't open drum track image file '%s': ", DEFAULT_TRACK_FILE);
        perror(NULL);
        fail();
    }

    // The track image file consists of an initial integer containing the starting track number,
    // followed by the data as it would be written to the drum.
    // advdrumloader then copies this data to the approprate track base on the drum.
    if( write(trackImageFd, &startTrack, sizeof(int)) != (ssize_t)(sizeof(int)) )
    {
        fprintf(stderr, "write failed for starting track number");
        perror(NULL);
        fail();
    }

    for( i = 0; i < numMsgs; ++i )
    {
        blockP = &msgBlocks[i];

        // The remaining data is written with its real track offset - startTrack
        nwords = packSixbit(blockP->text, packed, &discardPad);
        byteOffset = ((off_t)(blockP->track - startTrack) * WORDS_PER_TRACK + blockP->offset) * (off_t)sizeof(Word);
        byteOffset += sizeof(int);      // skip over the starting track we just wrote

        if( lseek(trackImageFd, byteOffset, SEEK_SET) < 0 )
        {
            fprintf(stderr, "seek failed for block '%s': ", blockP->symP->nameP);
            perror(NULL);
            fail();
        }
        if( write(trackImageFd, packed, nwords * sizeof(Word)) != (ssize_t)(nwords * sizeof(Word)) )
        {
            fprintf(stderr, "write failed for block '%s': ", blockP->symP->nameP);
            perror(NULL);
            fail();
        }
    }

    for( i = 0; i < numRooms; ++i )
    {
        buildRoomRecord(&rooms[i], rec);

        byteOffset = (((off_t)(roomBaseTrack - startTrack) * WORDS_PER_TRACK) +
            (off_t)(rooms[i].num - 1) * RECORDSIZE) * (off_t)sizeof(Word);
        byteOffset += sizeof(int);      // again, add in the initial int we wrote

        if( lseek(trackImageFd, byteOffset, SEEK_SET) < 0 )
        {
            fprintf(stderr, "seek failed for room '%s': ", rooms[i].symP->nameP);
            perror(NULL);
            fail();
        }
        if( write(trackImageFd, rec, sizeof(rec)) != (ssize_t)sizeof(rec) )
        {
            fprintf(stderr, "write failed for room '%s': ", rooms[i].symP->nameP);
            perror(NULL);
            fail();
        }
    }

    close(trackImageFd);
    trackImageFd = -1;
}

// -c: read-only compare against what would have been written.
// Reports every differing region and writes nothing.
static int
doCompare(const char *path, int roomBaseTrack)
{
int i;
int diffs;
off_t byteOffset;
Word packed[(MAX_TEXT / 3) + 2];
Word rec[RECORDSIZE];
Word diskBuf[(MAX_TEXT / 3) + 2];
int discardPad;
int nwords;
ssize_t n;

    if( (trackImageFd = open(path, O_RDONLY)) < 0 )
    {
        fprintf(stderr, "Can't open drum image file '%s': ", path);
        perror(NULL);
        fail();
    }

    diffs = 0;
    // Check the starting track
    n = read(trackImageFd, &i, sizeof(int));
    if( n != (ssize_t)(sizeof(int)) )
    {
        fprintf(stderr, "DIFF: starting track short read or mismatch.\n");
        ++diffs;
    }

    for( i = 0; i < numMsgs; ++i )
    {
    MessageBlockP blockP = &msgBlocks[i];

        nwords = packSixbit(blockP->text, packed, &discardPad);
        byteOffset = ((off_t)(blockP->track - startTrack) * WORDS_PER_TRACK + blockP->offset) * (off_t)sizeof(Word);
        byteOffset += sizeof(int);      // skip initial track no at location 0

        if( lseek(trackImageFd, byteOffset, SEEK_SET) < 0 )
        {
            fprintf(stderr, "seek failed for block '%s': ", blockP->symP->nameP);
            perror(NULL);
            fail();
        }

        n = read(trackImageFd, diskBuf, nwords * sizeof(Word));
        if( n != (ssize_t)(nwords * sizeof(Word)) )
        {
            fprintf(stderr, "DIFF: message '%s' track %d offset %d (%d words) -- short read\n",
                blockP->symP->nameP, blockP->track, blockP->offset, nwords);
            ++diffs;
            continue;
        }

        if( memcmp(diskBuf, packed, nwords * sizeof(Word)) != 0 )
        {
            fprintf(stderr, "DIFF: message '%s' track %d offset %d (%d words)\n",
                blockP->symP->nameP, blockP->track, blockP->offset, nwords);
            ++diffs;
        }
    }

    for( i = 0; i < numRooms; ++i )
    {
        buildRoomRecord(&rooms[i], rec);

        byteOffset = (((off_t)(roomBaseTrack - startTrack) * WORDS_PER_TRACK) + (off_t)(rooms[i].num - 1) *
            RECORDSIZE) * (off_t)sizeof(Word);
        byteOffset += sizeof(int);      // skip initial track no at location 0

        if( lseek(trackImageFd, byteOffset, SEEK_SET) < 0 )
        {
            fprintf(stderr, "seek failed for room '%s': ", rooms[i].symP->nameP);
            perror(NULL);
            fail();
        }

        n = read(trackImageFd, diskBuf, sizeof(rec));
        if( n != (ssize_t)sizeof(rec) )
        {
            fprintf(stderr, "DIFF: room '%s' num %d -- short read\n", rooms[i].symP->nameP, rooms[i].num);
            ++diffs;
            continue;
        }

        if( memcmp(diskBuf, rec, sizeof(rec)) != 0 )
        {
            fprintf(stderr, "DIFF: room '%s' num %d track %d offset %d (%d words)\n",
                rooms[i].symP->nameP, rooms[i].num, roomBaseTrack, (rooms[i].num - 1) * RECORDSIZE, RECORDSIZE);
            ++diffs;
        }
    }

    close(trackImageFd);
    trackImageFd = -1;

    if( diffs == 0 )
    {
        printf("advdataloader -c: clean, %d messages + %d rooms match '%s'.\n", numMsgs, numRooms, path);
    }
    else
    {
        printf("advdataloader -c: %d differing region(s) found in '%s'.\n", diffs, path);
    }

    return( diffs );
}

// ---------------------------------------------------------------------
// .ah emission -- fprintf blocks copied verbatim from
// AdvTextLoader/advtextloader.c and AdvRoomLoader/advroomloader.c
// (SPEC-PHASE1.md items 1-2), including header comments that literally
// name those tools -- that's intentional, it's what makes the output
// byte-identical to what they'd produce.
// ---------------------------------------------------------------------

static void
emitMsgtab(FILE *outP, int startTrack)
{
int i;
MessageBlockP blockP;

    fprintf(outP, "// Auto-generated by advtextloader, do not hand-edit.\n");
    fprintf(outP, "// Regenerate with: advtextloader -i <drumimage> -o %s <srcfile>\n", MSGTAB_OUTFILE);
    fprintf(outP, "// Each label is a 2-word record: track/word-offset, then a packed\n");
    fprintf(outP, "// (padCount<<12)|wordCount value -- padCount (0-2) is how many of the\n");
    fprintf(outP, "// last packed word's 3 character slots are unused padding, needed\n");
    fprintf(outP, "// because sixbit packing has no self-terminating byte (see\n");
    fprintf(outP, "// packSixbit's header comment in advtextloader.c). advPrintMsg in\n");
    fprintf(outP, "// adventure.am1 decodes both fields; advroomloader.c only ever\n");
    fprintf(outP, "// copies this word through verbatim and does not need to know its\n");
    fprintf(outP, "// internal shape. Pass the record's address to advPrintMsg to print it.\n");
    if( startTrack == SAVE_TRACK )
    {
        fprintf(outP, "// Track %d front blocks: words 0-%d SAVE state, words %d-%d WIZCOM\n",
            startTrack, SAVE_BLOCK_WORDS - 1, WIZCOM_BASE_OFFSET, DRUM_START_WORDS - 1);
        fprintf(outP, "// (fixed-size blocks -- see adv_drumlayout.ah, the generated contract).\n");
        fprintf(outP, "// Text starts at drum word %d.\n\n", DRUM_START_WORDS);
    }
    else
    {
        fprintf(outP, "// Text starts at drum track %d, word 0 (started via -s; the SAVE/WIZCOM\n",
            startTrack);
        fprintf(outP, "// front blocks live on track %d, elsewhere).\n\n", SAVE_TRACK);
    }
    fprintf(outP, "#ifndef ADV_MSGTAB_AH\n#define ADV_MSGTAB_AH\n\n");

    for( i = 0; i < numMsgs; ++i )
    {
        blockP = &msgBlocks[i];

        fprintf(outP, "msg_%s,\t0d%d\t// track<<12 + offset\n",
            blockP->symP->nameP, (blockP->track << 12) + blockP->offset);
        fprintf(outP, "\t0d%d\t// (padCount<<12)|wordCount -- %d word%s, %d pad slot%s\n",
            (blockP->padCount << 12) + blockP->nWords,
            blockP->nWords, (blockP->nWords == 1) ? "" : "s",
            blockP->padCount, (blockP->padCount == 1) ? "" : "s");
    }

    fprintf(outP, "\n#endif\n");
}

static void
emitRoomtab(FILE *outP, int roomBaseTrack, int tracksNeeded, int maxTrack)
{
int i;

    fprintf(outP, "// Auto-generated by advroomloader from Adventure/Rooms/adventureRooms.txt --\n");
    fprintf(outP, "// do not hand-edit. Regenerate with:\n");
    fprintf(outP, "//   advroomloader -i <drumimage> -m <msgtabfile> <srcfile>\n");

    if( tracksNeeded > 1 )
    {
        fprintf(outP, "// Room records live on drum tracks %d-%d (%d tracks), %d words each,\n",
            roomBaseTrack, roomBaseTrack + tracksNeeded - 1, tracksNeeded, RECORDSIZE);
        fprintf(outP, "// one per room number (room N's record is at flat word offset\n");
        fprintf(outP, "// (N-1)*%d from the start of track %d -- see adventure.am1's loadRoom,\n",
            RECORDSIZE, roomBaseTrack);
        fprintf(outP, "// which derives the same track/offset split at runtime via shift+mask,\n");
        fprintf(outP, "// Stage 16's own STAGE16-PLAN.md section 1, and STAGE11-PLAN.md section 3\n");
        fprintf(outP, "// for the original single-track design this generalizes). Track %d was\n",
            roomBaseTrack);
    }
    else
    {
        fprintf(outP, "// Room records live on drum track %d, %d words each, one per room number\n",
            roomBaseTrack, RECORDSIZE);
        fprintf(outP, "// (room N's record is at word offset (N-1)*%d). See adventure.am1's\n", RECORDSIZE);
        fprintf(outP, "// loadRoom/ROOM_CACHE and STAGE11-PLAN.md section 3. Track %d was\n",
            roomBaseTrack);
    }

    fprintf(outP, "// computed, not passed in -- one past msgtab's own highest track (%d); see\n", maxTrack);
    fprintf(outP, "// Adventure/DRUMOVERWRITE-TASK.md.\n\n");
    fprintf(outP, "#ifndef ADV_ROOMTAB_AH\n#define ADV_ROOMTAB_AH\n\n");

    for( i = 0; i < numRooms; ++i )
    {
        fprintf(outP, "#define %s 0d%d\n", rooms[i].symP->nameP, rooms[i].num);
    }

    fprintf(outP, "\n#define NROOMS 0d%d\n", numRooms);
    fprintf(outP, "#define ROOMTAB_BASE_TRACK 0d%d\n", roomBaseTrack);
    fprintf(outP, "#define ROOMTAB_RECORDSIZE 0d%d\n", RECORDSIZE);
    fprintf(outP, "#define ROOMTAB_HEADERSIZE 0d%d\n", HEADERWORDS);
    fprintf(outP, "#define EXIT_OFFSET 0d%d\n", HEADERWORDS);
    fprintf(outP, "#define EXIT_RECORDSIZE 0d%d\n", EXITWORDS);
    fprintf(outP, "#define MAXEXITS 0d%d\n\n", MAXEXITS);

    fprintf(outP, "// Condition IDs -- must stay in this exact order in adventure.am1's own\n");
    fprintf(outP, "// condFlagAddrs dispatch table (see STAGE11-PLAN.md section 4). 0 is reserved\n");
    fprintf(outP, "// for \"unconditional\", never emitted here.\n");
    for( i = 0; condTable[i].nameP; ++i )
    {
        fprintf(outP, "#define COND_%s 0d%d\n", condTable[i].nameP, condTable[i].id);
    }

    // So the adventure program stays in sync with this.
    fprintf(outP, "#define COND_RAND 0d%d\n", COND_RAND_ID);
    fprintf(outP, "#define RAND_DOMAIN_MASK 0d%d\n", RAND_DOMAIN - 1);
    fprintf(outP, "#define DARK_FLAG_MASK 0%o\n", DARK_FLAG);
    fprintf(outP, "#define DWARF_FLAG_MASK 0%o\n", DWARF_FLAG);
    fprintf(outP, "#define SURFACE_FLAG_MASK 0%o\n", SURFACE_FLAG);
    fprintf(outP, "#define PIRATE_FORBID_MASK 0%o\n", PIRATE_FORBID_FLAG);
    fprintf(outP, "#define ROOMTAB_FLAGS 5\n");
    fprintf(outP, "#define ROOMTAB_LONG_MSG 1\n");
    fprintf(outP, "#define ROOMTAB_SHORT_MSG 3\n");
    fprintf(outP, "#define ROOMTAB_EXITS 0d%d\n", HEADERWORDS);
    fprintf(outP, "#define EXIT_FLAGS 0\n");
    fprintf(outP, "#define EXIT_MSG 1\n");

    // These expect the value to be in the AC
    fprintf(outP, "// Value in AC for these.\n");
    fprintf(outP, "// AC will be nonzero if true\n");
    fprintf(outP, "#define IS_DWARF and [DWARF_FLAG_MASK]\n");
    fprintf(outP, "#define IS_DARK and [DARK_FLAG_MASK]\n");
    fprintf(outP, "#define IS_SURFACE and [SURFACE_FLAG_MASK]\n");
    fprintf(outP, "#define IS_PIRATE_FORBID and [PIRATE_FORBID_MASK]\n\n");

    fprintf(outP, "// Value in AC for these, result in AC.\n");
    fprintf(outP, "#define GET_ROOM_NUMBER and [0xFF]\n");
    fprintf(outP, "#define GET_EXIT_COUNT sar 8s; and [0xFF]\n");
    fprintf(outP, "#define GET_DIRECTION_CODE sar 9s; sar 4s; and [0x1F]\n");
    fprintf(outP, "#define GET_CONDITION_ID sar 8s; and [0x1F]\n\n");

    fprintf(outP, "\n#endif\n");
}

// Emit adv_surfacebitmap.ah -- one bit per room (bit (room-1), 1-based
// room numbers), set if that room is SURFACE-flagged. Consumed by
// mgDestCheck (adventure.am1, bank 3's isSurfaceRoom wrapper) via
// UTIL/bitsets.ac's testBitInList, for the one place a room's own
// SURFACE flag is needed before its record has been loaded (a move
// destination during closing). See PendingRework/TASK-ROOM-FLAG-WORD.md.
// buildRoomRecord() is reused as-is (already correctly computes word 5)
// rather than re-deriving SURFACE membership from attributesP by name --
// avoids duplicating flag-lookup logic, and guarantees the bitmap can
// never disagree with the drum record it's describing. nWords is
// computed from the live numRooms, not a hardcoded constant, so the
// bitmap stays renumber/insertion-safe.
static void
emitSurfaceBitmap(FILE *outP)
{
Word rec[RECORDSIZE];
int  *bitmap;
int  nWords = (numRooms + 17) / 18;
int  i, w, bitIx;

    bitmap = (int *)calloc(nWords, sizeof(int));

    for( i = 0; i < numRooms; ++i )
    {
        buildRoomRecord(&rooms[i], rec);
        if( rec[5] & SURFACE_FLAG )        // rec[ROOMTAB_FLAGS], word index 5
        {
            bitIx = rooms[i].num - 1;       // 0-based, matches loadRoom's own
                                             // (curRoom-1) convention and
                                             // bitsets.ac's own bit-numbering
                                             // (msb=17/lsb=0, i.e. plain 1<<n)
            bitmap[bitIx / 18] |= (1 << (bitIx % 18));
        }
    }

    fprintf(outP, "// Auto-generated by advdataloader from AdvDataLoader/adventure.adv --\n");
    fprintf(outP, "// do not hand-edit. Regenerate via `make` in Adventure/.\n");
    fprintf(outP, "// One bit per room (bit (room-1), room numbers 1-based), set if that\n");
    fprintf(outP, "// room is SURFACE-flagged -- consumed by mgDestCheck (adventure.am1)\n");
    fprintf(outP, "// via UTIL/bitsets.ac's testBitInList, for the one place a room's own\n");
    fprintf(outP, "// SURFACE flag is needed before its record has been loaded (a move\n");
    fprintf(outP, "// destination during closing). See PendingRework/TASK-ROOM-FLAG-WORD.md.\n\n");
    fprintf(outP, "#ifndef ADV_SURFACEBITMAP_AH\n#define ADV_SURFACEBITMAP_AH\n\n");
    fprintf(outP, "#define SURFACE_BITMAP_WORDS 0d%d\n\n", nWords);
    fprintf(outP, "SURFACE_BITMAP,\n");
    for( w = 0; w < nWords; ++w )
    {
        fprintf(outP, "\t0%o\n", bitmap[w]);
    }
    fprintf(outP, "\n#endif\n");
    free(bitmap);
}

// ---------------------------------------------------------------------
// Object table emission (SPEC-PHASE2.md) -- adv_objdefs.ah (cpp-only:
// OBJ_* index defines, NOBJS, and the accessor macros) plus six
// one-line-per-object table-body files, one per parallel array. Called
// from main() only when the objects section is non-empty. Values are
// printed exactly as addObjectDef()/the locVal/treasureVal grammar
// actions already resolved and formatted them -- symbolic text passed
// through verbatim, the assembler is the final arbiter, same as the
// hand tables this replaces. Whitespace/comment style is free-form
// (binary identity is judged at the assembled level); only the
// accessor-macro BODIES need to be byte-for-byte identical to the hand
// macros (SPEC-PHASE2.md acceptance item 4) -- kept exact by using a
// single space between the macro head and its body below, so the body
// text itself (e.g. "objLoc+o") is trivially diffable.
// ---------------------------------------------------------------------

// {macro suffix, table array name, far-tag} for the five accessor-macro
// pairs (OBJ_<suffix>(o) / FAR_OBJ_<suffix>(o)) -- byte-for-byte the
// same macro BODIES as today's hand macros (adventure.am1's "Object
// indices and accessor macros" block). objNames is deliberately absent:
// the hand file never had an OBJ_NAMES/FAR_OBJ_NAMES pair either --
// findObj addresses it directly under eem, not through a +o accessor.
static const struct {
    const char *suffixP;
    const char *arrayNameP;
    const char *tagP;      // ":0" (bank 0) or ":2" (bank 2)
} objAccessors[] = {
    { "LOC",      "objLoc",      ":0" },
    { "TAKE",     "objTake",     ":0" },
    { "INVMSG",   "objInvMsg",   ":2" },
    { "HEREMSG",  "objHereMsg",  ":2" },
    { "TREASURE", "objTreasure", ":2" },
};
#define NUM_OBJ_ACCESSORS (int)(sizeof(objAccessors) / sizeof(objAccessors[0]))

static void
emitObjDefs(FILE *outP)
{
int i;

    fprintf(outP, "// Auto-generated by advdataloader from '%s', do not hand-edit.\n", baseNameP);
    fprintf(outP, "// Regenerate with: advdataloader <srcfile>\n");
    fprintf(outP, "// Object indices and accessor macros -- see adventure.am1's \"Object table\"\n");
    fprintf(outP, "// section for the six parallel arrays these index into (SPEC-PHASE2.md).\n\n");
    fprintf(outP, "#ifndef ADV_OBJDEFS_AH\n#define ADV_OBJDEFS_AH\n\n");

    for( i = 0; i < numObjects; ++i )
    {
        fprintf(outP, "#define OBJ_%s 0d%d\n", objects[i].symP->nameP, objects[i].index);
    }
    fprintf(outP, "\n#define NOBJS 0d%d\n\n", numObjects);

    for( i = 0; i < NUM_OBJ_ACCESSORS; ++i )
    {
        fprintf(outP, "#define OBJ_%s(o) %s+o\n",
            objAccessors[i].suffixP, objAccessors[i].arrayNameP);
    }
    fprintf(outP, "\n");
    for( i = 0; i < NUM_OBJ_ACCESSORS; ++i )
    {
        fprintf(outP, "#define FAR_OBJ_%s(o) %s%s+o\n",
            objAccessors[i].suffixP, objAccessors[i].arrayNameP, objAccessors[i].tagP);
    }

    fprintf(outP, "\n#endif\n");
}

typedef char *(*ObjFieldGetter)(ObjectP objP);

static char *
getObjLocField(ObjectP objP)
{
    return( objP->locTextP );
}

static char *
getObjTakeField(ObjectP objP)
{
static char buf[8];

    sprintf(buf, "%d", objP->take);
    return( buf );
}

static char *
getObjNamesField(ObjectP objP)
{
static char buf[MAX_NAME + 4];

    sprintf(buf, "%s:.", objP->vocSymP);
    return( buf );
}

static char *
getObjInvMsgField(ObjectP objP)
{
    return( objP->invMsgTextP );
}

static char *
getObjHereMsgField(ObjectP objP)
{
    return( objP->hereMsgTextP );
}

static char *
getObjTreasureField(ObjectP objP)
{
    return( objP->treasureTextP );
}

static void
emitObjTableFile(const char *path, const char *labelP, ObjFieldGetter getterP)
{
FILE *outP;
int i;

    if( !(outP = fopen(path, "w")) )
    {
        fprintf(stderr, "Can't create output file '%s'\n", path);
        fail();
    }

    fprintf(outP, "// Auto-generated by advdataloader from '%s', do not hand-edit.\n", baseNameP);
    fprintf(outP, "// Regenerate with: advdataloader <srcfile>\n");
    fprintf(outP, "// %s table body -- one row per object, file order = OBJ_* index order,\n", labelP);
    fprintf(outP, "// see adv_objdefs.ah.\n\n");

    fprintf(outP, "%s,\t%s\t// %s\n", labelP, getterP(&objects[0]), objects[0].symP->nameP);
    for( i = 1; i < numObjects; ++i )
    {
        fprintf(outP, "\t\t%s\t// %s\n", getterP(&objects[i]), objects[i].symP->nameP);
    }

    fclose(outP);
}

static void
emitObjTables(char *dirP, FILE *deffP)
{
char outPath[1024];

    // These are defines
    emitObjDefs(deffP);

    sprintf(outPath,"%s%s%s", dirP, (*dirP)?"/":"", "adv_objloc.ac");
    emitObjTableFile(outPath, "objLoc", getObjLocField);

    sprintf(outPath,"%s%s%s", dirP, (*dirP)?"/":"", "adv_objtake.ac");
    emitObjTableFile(outPath, "objTake", getObjTakeField);

    sprintf(outPath,"%s%s%s", dirP, (*dirP)?"/":"", "adv_objnames.ac");
    emitObjTableFile(outPath, "objNames", getObjNamesField);

    sprintf(outPath,"%s%s%s", dirP, (*dirP)?"/":"", "adv_objinvmsg.ac");
    emitObjTableFile(outPath,"objInvMsg", getObjInvMsgField);

    sprintf(outPath,"%s%s%s", dirP, (*dirP)?"/":"", "adv_objheremsg.ac");
    emitObjTableFile(outPath,"objHereMsg", getObjHereMsgField);

    sprintf(outPath,"%s%s%s", dirP, (*dirP)?"/":"", "adv_objtreasure.ac");
    emitObjTableFile(outPath, "objTreasure", getObjTreasureField);
}

static void
emitVerbTab(char *dirP)
{
int i;
FILE *outP;
char outPath[1024];

    sprintf(outPath,"%s%s%s", dirP, (*dirP)?"/":"", VERBTAB_OUTFILE);
    if( !(outP = fopen(outPath, "w")) )
    {
        fprintf(stderr, "Can't create output file '%s'\n", outPath);
        fail();
    }

    fprintf(outP, "// Auto-generated by advdataloader from '%s', do not hand-edit.\n", baseNameP);
    fprintf(outP, "// Regenerate with: advdataloader <srcfile>\n");
    fprintf(outP, "verbTab,\n");
    for( i = 0; i < numVerbs; ++i )
    {
        fprintf(outP, "    %s;\t%s;\t%s\t// %s\n",
            verbs[i].vocTextP, verbs[i].argTextP, verbs[i].handlerTextP, verbs[i].symP->nameP);
    }
    fprintf(outP, "    0                          // end of table\n");

    fclose(outP);
}

// ---------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------

// Error of some kind, clean up and leave. Since all placement/record
// building is validated in memory before the drum image is opened, the
// only thing there is to clean up mid-parse is the input file; trackImageFd
// is only ever open during doWrite()/doCompare(), both of which are
// past all validation by the time they run.
void
fail(void)
{
    if( yyin )
    {
        fclose(yyin);
    }

    if( trackImageFd >= 0 )
    {
        close(trackImageFd);
    }

    exit(1);
}

// Emit adv_drumlayout.ah -- the drum layout contract between this tool
// (which places the SAVE/WIZCOM front blocks' reservation, the message
// text, and the room records) and adventure.am1, whose drum-transfer
// records must agree. Emitted from the same #defines the placement code
// uses, so the two sides cannot drift.
static void
emitDrumLayout(FILE *outP)
{
    fprintf(outP, "// Auto-generated by advdataloader from '%s', do not hand-edit.\n", baseNameP);
    fprintf(outP, "// Regenerate with: advdataloader <srcfile>\n");
    fprintf(outP, "// Drum layout contract: SAVE and WIZCOM live in fixed-size blocks at\n");
    fprintf(outP, "// the front of SAVE_TRACK; message text starts right after them on the\n");
    fprintf(outP, "// same track (and flows onto later tracks); room records follow one\n");
    fprintf(outP, "// track past the last text track (see adv_roomtab.ah). adventure.am1's\n");
    fprintf(outP, "// own SAVE_*/WC_* record offsets are RELATIVE to these block bases.\n");
    fprintf(outP, "#ifndef ADV_DRUMLAYOUT_AH\n#define ADV_DRUMLAYOUT_AH\n\n");
    fprintf(outP, "#define SAVE_TRACK             0d%d\n", SAVE_TRACK);
    fprintf(outP, "#define SAVE_BLOCK_WORDS       0d%d\n", SAVE_BLOCK_WORDS);
    fprintf(outP, "#define WIZCOM_TRACK           0d%d   // same track -- fixed front blocks\n", SAVE_TRACK);
    fprintf(outP, "#define WIZCOM_BASE_OFFSET     0d%d\n", WIZCOM_BASE_OFFSET);
    fprintf(outP, "#define WIZCOM_BLOCK_WORDS     0d%d\n", WIZCOM_BLOCK_WORDS);
    fprintf(outP, "#define DRUM_TEXT_START_OFFSET 0d%d\n", DRUM_START_WORDS);
    fprintf(outP, "\n#endif\n");
}

static void
usage(void)
{
    fprintf(stderr,
        "Usage: advdataloader [-s starttrack] [-c] [-o dir] srcfile\n"
        "  -s starttrack, first drum track to use, 0-%d (default %d)\n"
        "  -c compare mode, report differences against imagefile, write nothing\n"
        "  -o outdir, put generated files in this directory, default is current directory\n"
        "The drum track image file %s always goes in the current directory.\n",
        NUM_TRACKS - 1, DEFAULT_START_TRACK, DEFAULT_TRACK_FILE);

    exit(1);
}
