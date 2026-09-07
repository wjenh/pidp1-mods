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
 *
 * The SPEC-PHASE1.md / SPEC-PHASE2.md cited in comments below moved to
 * ../CompletedTasks/AdvDataLoader-Phase2/ on 04-Sep-26. They are a record
 * of how this was built, not a description of what it does now; README.md
 * in this directory is the current reference.
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
#define OBJALIAS_OUTFILE "adv_objalias.ac"
#define SURFACEBITMAP_OUTFILE "adv_surfacebitmap.ac"
#define DWARFBITMAP_OUTFILE "adv_dwarfbitmap.ac"

#define MAX_OBJECTS 512     // number of objects we can store
#define MAX_VERBS   512     // number of verb rows we can store (~178 today)

// Largest direction code an EXIT row can carry -- buildRoomRecord() packs
// it into the exit word's direction field, so this IS that field's mask.
// Directions themselves are not capped by it (curDir is a whole word),
// only their use in a room's exit list; newRoomExit() enforces it.
// 127 covers adven.dat's motion numbering (75 distinct codes, highest 77)
// with room to spare.
#define MAX_EXIT_DIRCODE EXIT_DIR_MASK
#define MAX_ALIASES 256     // number of object-alias rows (~26 today)

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

static Alias aliases[MAX_ALIASES];
static int numAliases;
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
    { "ROAD",      2 },  /* HILL/ROAD */
    { "ENTER",     3 },  /* ENTER -- TASK-FR2 step 4: motion 3's only word, de-aliased from IN */
    { "UPSTR",     4 },  /* UPSTR */
    { "DOWNS",     5 },  /* DOWNS */
    { "FORES",     6 },  /* FORES */
    { "FORWARD",   7 },  /* CONTI/FORWA/ONWAR */
    { "VALLE",     9 },  /* VALLE */
    { "STAIR",     10 },  /* STAIR */
    { "OUT",       11 },  /* EXIT/LEAVE/OUT/OUTSI */
    { "BUILD",     12 },  /* BUILD/HOUSE */
    { "GULLY",     13 },  /* GULLY */
    { "STREA",     14 },  /* STREA -- voc_stream already exists (adven.f4 892's ENTER STREAM) */
    { "ROCK",      15 },  /* ROCK */
    { "BED",       16 },  /* BED */
    { "CRAWL",     17 },  /* CRAWL */
    { "COBBL",     18 },  /* COBBL */
    { "IN",        19 },  /* IN/INSID/INWAR */
    { "SURFA",     20 },  /* SURFA */
    { "NULL",      21 },  /* NOWHE/NULL -- RESERVED -- adven.f4 1005 makes NULL/NOWHE a label-8 special */
    { "DARK",      22 },  /* DARK */
    { "PASSAGE",   23 },  /* PASSA/TUNNE -- TUNNEL shares this code -- one adven.dat word, two spellings */
    { "TUNNEL",    23 },  /* PASSA/TUNNE -- shares DIR_PASSAGE, adven.dat's own PASSA/TUNNE pair */
    { "LOW",       24 },  /* LOW */
    { "CANYO",     25 },  /* CANYO */
    { "GIANT",     27 },  /* GIANT */
    { "VIEW",      28 },  /* VIEW */
    { "UP",        29 },  /* ABOVE/ASCEN/U/UP/UPWAR */
    { "DOWN",      30 },  /* D/DESCE/DOWN/DOWNW */
    { "PIT",       31 },  /* PIT */
    { "OUTDO",     32 },  /* OUTDO */
    { "CRACK",     33 },  /* CRACK */
    { "STEPS",     34 },  /* STEPS -- voc_steps already exists (object 1007, same word) */
    { "DOME",      35 },  /* DOME */
    { "LEFT",      36 },  /* LEFT -- TASK-FR2 step 4: de-aliased from NE */
    { "RIGHT",     37 },  /* RIGHT -- TASK-FR2 step 4: de-aliased from SE */
    { "HALL",      38 },  /* HALL */
    { "JUMP",      39 },  /* JUMP */
    { "BARRE",     40 },  /* BARRE */
    { "OVER",      41 },  /* OVER */
    { "ACROSS",    42 },  /* ACROS */
    { "EAST",      43 },  /* E/EAST */
    { "WEST",      44 },  /* W/WEST */
    { "NORTH",     45 },  /* N/NORTH */
    { "SOUTH",     46 },  /* S/SOUTH */
    { "NE",        47 },  /* NE */
    { "SE",        48 },  /* SE */
    { "SW",        49 },  /* SW */
    { "NW",        50 },  /* NW */
    { "DEBRI",     51 },  /* DEBRI */
    { "HOLE",      52 },  /* HOLE */
    { "WALL",      53 },  /* WALL */
    { "BROKE",     54 },  /* BROKE */
    { "Y2",        55 },  /* Y2 */
    { "CLIMB",     56 },  /* CLIMB */
    { "FLOOR",     58 },  /* FLOOR */
    { "ROOM",      59 },  /* ROOM */
    { "SLIT",      60 },  /* SLIT */
    { "SLAB",      61 },  /* SLAB/SLABR */
    { "XYZZY",     62 },  /* XYZZY -- RESERVED -- doXyzzy owns the word */
    { "DEPRE",     63 },  /* DEPRE */
    { "ENTRA",     64 },  /* ENTRA */
    { "PLUGH",     65 },  /* PLUGH -- RESERVED -- doPlugh owns the word */
    { "SECRE",     66 },  /* SECRE */
    { "CROSS",     69 },  /* CROSS */
    { "BEDQU",     70 },  /* BEDQU */
    { "PLOVE",     71 },  /* PLOVE -- RESERVED -- doPlove owns the word */
    { "ORIEN",     72 },  /* ORIEN */
    { "CAVER",     73 },  /* CAVER */
    { "SHELL",     74 },  /* SHELL */
    { "RESER",     75 },  /* RESER */
    { "FORK",      77 },  /* FORK */
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
static int exitCondId(const char *dirNameP, char *condNameP);
static MessageBlockP exitMsgBlock(const char *dirNameP, char *msgNameP);

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
void addRoomExitCondSilent(char *dirNameP, char *destNameP, char *condNameP);
void addRoomExitCondMsg(char *dirNameP, char *condNameP, char *msgNameP);
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
static void emitRoomFlagBitmap(FILE *outP, Word flagMask, const char *guardP,
                               const char *symbolP, const char *purposeP);
static void emitSurfaceBitmap(FILE *outP);
static void emitDwarfBitmap(FILE *outP);
static void emitObjDefs(FILE *outP);
static void emitObjTables(char *dirnameP, FILE *deffP);
static void emitVerbTab(char *dirnameP);
static void emitObjAlias(char *dirnameP, FILE *deffP);
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

    sprintf(outPath,"%s%s%s", dirP, (*dirP)?"/":"", DWARFBITMAP_OUTFILE);
    if( !(outP = fopen(outPath, "w")) )
    {
        fprintf(stderr, "Can't create output file '%s'\n", DWARFBITMAP_OUTFILE);
        fail();
    }
    emitDwarfBitmap(outP);
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

    // Object aliases (TASK-FR7 7b group 1) -- same optional-section
    // policy as objects and verbs. NOBJALIAS is emitted alongside so
    // findObj's scan has a bound even when the table is empty.
    if( numObjects > 0 )
    {
        emitObjAlias(dirP, defOutP);
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

    if( numObjects > 0 )
    {
        printf("Wrote %d object aliases to '%s'\n", numAliases, OBJALIAS_OUTFILE);
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

    // buildRoomRecord() packs this code into bits 0:6 of the exit word
    // (dirCode << EXIT_DIR_SHIFT, read back by adventure.am1's
    // GET_DIRECTION_CODE "sar 9s; sar 2s; and [0x7F]"). Refused rather
    // than truncated silently: a truncated code is a plausible-looking
    // exit in the wrong direction, which is exactly the kind of defect
    // the room table cannot show.
    if( code > MAX_EXIT_DIRCODE )
    {
        verror("Room '%s': EXIT direction '%s' has code %d, above the %d the exit "
               "word's 7-bit direction field can hold\n",
            currentRoomP->symP->nameP, dirNameP, code, MAX_EXIT_DIRCODE);
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

// Resolve a condition name to its condTable ID, or die. Shared by all
// three `cond` exit forms. The range check is not paranoia: doMove tells
// a boolean gate from the three internal IDs (COND_RAND, COND_MSG,
// COND_RANDMSG) by value alone, so a condTable entry that ever collided
// with one of them would be silently mis-dispatched at run time rather
// than rejected here.
static int
exitCondId(const char *dirNameP, char *condNameP)
{
int condId;

    if( (condId = condIdForName(condNameP)) < 0 )
    {
        verror("Room '%s': EXIT %s condition '%s' is not a known condition "
            "(add it to advdataloader.c's condTable[] and adventure.am1's "
            "condFlagAddrs, in the same order)\n",
            currentRoomP->symP->nameP, dirNameP, condNameP);
    }

    if( (condId == COND_RAND_ID) || (condId == COND_MSG_ID) || (condId == COND_RANDMSG_ID) )
    {
        verror("Room '%s': EXIT %s condition '%s' has ID %d, which is one of doMove's "
               "internal IDs (COND_RAND %d, COND_MSG %d, COND_RANDMSG %d) -- "
               "renumber it in condTable[] and in condFlagAddrs together\n",
            currentRoomP->symP->nameP, dirNameP, condNameP, condId,
            COND_RAND_ID, COND_MSG_ID, COND_RANDMSG_ID);
    }

    if( condId > EXIT_COND_MASK )
    {
        verror("Room '%s': EXIT %s condition '%s' has ID %d, above the %d the exit "
               "word's 5-bit condition field can hold\n",
            currentRoomP->symP->nameP, dirNameP, condNameP, condId, EXIT_COND_MASK);
    }

    return( condId );
}

void
addRoomExitCond(char *dirNameP, char *destNameP, char *condNameP, char *msgNameP)
{
ExitP eP;
int condId;
SymNodeP symP;

    eP = newRoomExit(dirNameP, destNameP);

    condId = exitCondId(dirNameP, condNameP);

    if( !(symP = symFind(&msgSymsP, msgNameP)) )
    {
        verror("Room '%s': EXIT %s block message '%s' not found\n",
            currentRoomP->symP->nameP, dirNameP, msgNameP);
    }

    eP->condId = condId;
    eP->condMsgP = (MessageBlockP)symP->ptr;
}

// TASK-FR2 step 3 / TASK-FR19 C12: 'EXIT <dir> <dest> COND <cond>' --
// the gate with no refusal message. The condition holds, the player
// moves; it fails, and doMove's dmGateFail walks on to the next entry
// for this direction without printing anything, which is adven.f4 label
// 12's own behaviour. adven.dat's conditional rows (M >= 100) whose
// cascade continues on a further row rather than on a message.
//
// The row is marked ONLY by having no message: condId stays an ordinary
// condTable ID, so nothing about the entry's format changes.
// advdataloader.h has the four-shape table.
void
addRoomExitCondSilent(char *dirNameP, char *destNameP, char *condNameP)
{
ExitP eP;

    eP = newRoomExit(dirNameP, destNameP);

    eP->condId = exitCondId(dirNameP, condNameP);
    eP->condMsgP = NULL;         // no message => silent fall-through on failure
}

// TASK-FR2 step 3: 'EXIT <dir> NONE COND <cond> MSG <msg>' -- the gated
// message-only row. The condition holds, so the row's own action runs
// (print <msg>, stay put); it fails, so the scan moves on silently. The
// sense of `msg` is the one the unconditional NONE rows already set: on
// a row that names no destination the message IS the action, not a
// refusal.
//
// adven.dat's conditional N>500 rows -- LOC 103's clam/oyster refusals,
// LOC 117/122's troll-and-chasm pair, the fissure's msg 97.
void
addRoomExitCondMsg(char *dirNameP, char *condNameP, char *msgNameP)
{
ExitP eP;

    eP = newRoomExit(dirNameP, NULL);

    eP->condId = exitCondId(dirNameP, condNameP);
    eP->condMsgP = exitMsgBlock(dirNameP, msgNameP);
}

// Resolve a message name to its block, or die. Shared by every exit form
// that carries a message: the two COND forms and the two message-only
// ones.
static MessageBlockP
exitMsgBlock(const char *dirNameP, char *msgNameP)
{
SymNodeP symP;

    if( !(symP = symFind(&msgSymsP, msgNameP)) )
    {
        verror("Room '%s': EXIT %s block message '%s' not found\n",
            currentRoomP->symP->nameP, dirNameP, msgNameP);
    }

    return( (MessageBlockP)symP->ptr );
}

// TASK-FR3/FR4: 'EXIT <dir> NONE MSG <msg>' -- print <msg> and stay put,
// unconditionally. adven.dat's N>500 travel rows. destNameP is NULL, so
// resolveRoomExits() leaves destNum at 0, which is what doMove reads as
// "no destination".
void
addRoomExitMsg(char *dirNameP, char *msgNameP)
{
ExitP eP;

    eP = newRoomExit(dirNameP, NULL);
    eP->condId = COND_MSG_ID;
    eP->condMsgP = exitMsgBlock(dirNameP, msgNameP);
}

// TASK-FR4: 'EXIT <dir> NONE RAND <pct> MSG <msg>' -- on a hit, print
// <msg> and stay put; on a miss, fall through to the next row for this
// direction. adven.dat's M<100 N>500 rows (the msg-56/126 bounces).
//
// The threshold rides in the exit word's own threshold field, the same
// one a plain RAND row uses. It no longer has to squat in destNum: the
// message is a 10-bit index into msgtab, not an inline doublet, so the
// two stopped competing for space (advdataloader.h has the layout).
void
addRoomExitRandMsg(char *dirNameP, int percent, char *msgNameP)
{
ExitP eP;

    if( (percent < 0) || (percent > 100) )
    {
        verror("Room '%s': EXIT %s RAND percent %d is not 0-100\n",
            currentRoomP->symP->nameP, dirNameP, percent);
    }

    eP = newRoomExit(dirNameP, NULL);
    eP->condId = COND_RANDMSG_ID;
    eP->condMsgP = exitMsgBlock(dirNameP, msgNameP);
    eP->randThreshold = (percent * RAND_DOMAIN + 50) / 100;    // rounded, as addRoomExitRand
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

            // A row that names no destination at all -- the two
            // message-only types (COND_MSG_ID, COND_RANDMSG_ID) and the
            // gated message row addRoomExitCondMsg() builds; leave
            // destNum 0.
            if( !eP->destNameP )
            {
                continue;
            }

            if( !(symP = symFind(&roomSymsP, eP->destNameP)) )
            {
                verrorAt(eP->destLine, "Room '%s': EXIT destination '%s' is not a defined room\n",
                    rP->symP->nameP, eP->destNameP);
            }

            eP->destNum = ((RoomP)symP->ptr)->num;
        }
    }
}

// The 1-based index of a message block in the emitted msgtab, or 0 for
// "no message". emitMsgtab() walks msgBlocks[] in this order and emits
// two words per block behind the label msgTab, so a message's run-time
// address is msgTab + MSGTAB_RECORDSIZE * (index - 1). The index is
// 1-based precisely so that 0 can mean "this row prints nothing" --
// msgBlocks[0] is a real message.
static int
msgIndexOf(MessageBlockP blockP)
{
int idx;

    if( !blockP )
    {
        return( 0 );
    }

    idx = (int)(blockP - msgBlocks) + 1;

    if( idx > EXIT_MSGIDX_MASK )
    {
        verror("message '%s' is number %d, past the %d the exit entry's message "
               "index field can hold\n", blockP->symP->nameP, idx, EXIT_MSGIDX_MASK);
    }

    return( idx );
}

// Build one room's 64-word drum record, exactly matching
// advroomloader.c's buildRecord().
static void
buildRoomRecord(RoomP rP, Word *outWords)
{
int i, base;
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

    // Two words, one shape for every row type -- advdataloader.h has the
    // field layout. A field a given row does not use is simply 0: destNum
    // for the two message-only types, randThreshold for all but the two
    // weighted ones, the message index for a row that prints nothing.
    // That is why no per-condition branching survives here: while the
    // message was an inline doublet it needed both of the entry's other
    // words, and the row types had to fight over them.
    for( i = 0; i < rP->nexits; ++i )
    {
        eP = &rP->exits[i];
        base = HEADERWORDS + (i * EXITWORDS);

        outWords[base] = (eP->dirCode << EXIT_DIR_SHIFT)
                       | (eP->condId << EXIT_COND_SHIFT)
                       | (eP->randThreshold & EXIT_THRESH_MASK);

        outWords[base + 1] = (msgIndexOf(eP->condMsgP) << EXIT_MSGIDX_SHIFT)
                           | (eP->destNum & EXIT_DEST_MASK);
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
// Object aliases (TASK-FR7 7b group 1, S14) -- one flat 'alias'
// statement per row of the two-column objAlias table. adven.dat section
// 4 gives many objects more than one vocabulary word (LAMP is also
// HEADL and LANTE; CHEST is also BOX and TREAS); the port's objNames
// table cannot hold them, because findObj advances objNames, objLoc and
// objTake together on a single index and a second name row would slide
// the other two out of correspondence. The alias table is scanned only
// after that walk has missed, so the common path is unchanged.
//
// The target object must already have been defined -- 'aliases' follows
// 'objects' in the 'definitions' rule -- which is what lets the index be
// resolved here, at parse time, rather than needing a second pass.
// ---------------------------------------------------------------------

void
addAliasDef(char *vocWordP, char *objNameP)
{
AliasP aliasP;
SymNodeP symP;
ObjectP objP;
char buf[MAX_NAME + 8];

    if( numAliases >= MAX_ALIASES )
    {
        verror("Too many object aliases, the limit is %d.\n", MAX_ALIASES);
    }

    if( !(symP = symFind(&objSymsP, objNameP)) )
    {
        verror("Alias target object '%s' is not defined.\n", objNameP);
    }

    objP = (ObjectP)symP->ptr;

    aliasP = &aliases[numAliases];
    sprintf(buf, "voc_%s", vocWordP);
    aliasP->vocSymP = (char *)malloc(strlen(buf) + 1);
    strcpy(aliasP->vocSymP, buf);
    aliasP->objIndex = objP->index;
    aliasP->objNameP = objNameP;

    ++numAliases;
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
addVerbDef(char *nameP, char *vocWordP, int vocBank, VerbArgP argP, char *handlerP)
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

    // The bank tag comes from the corpus's optional "bank <n>" clause;
    // parser.y passes a literal 2 for a row that omits it. It is not
    // cosmetic -- am1 resolves voc_<word>:<n> against bank n, so a
    // string that has moved needs its tag moved with it. TASK-FR2 step
    // 1's motion words live in bank 3 because bank 2 is full, the same
    // reason TASK-FR7 7b's object synonyms went there (which is why
    // emitObjAlias has a hardcoded ":3").
    if( (vocBank < 0) || (vocBank > 3) )
    {
        verror("Verb '%s': bank %d is not a memory bank (0-3).\n", nameP, vocBank);
    }
    sprintf(buf, "voc_%s:%d", vocWordP, vocBank);
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

    // The table's base. A room's exit entry names its message by a
    // 1-based index into this table rather than carrying an inline
    // doublet. The records below are contiguous and in this order;
    // nothing may be inserted between msgTab and the first of them.
    fprintf(outP, "// msgTab -- the table's base. A room exit entry names a message\n");
    fprintf(outP, "// by its 1-based index here: address = msgTab + 2 * (index - 1).\n");
    fprintf(outP, "msgTab,\n");

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
    // TASK-FR3/FR4 message-only rows. Both are special-cased in doMove
    // and SKIPPED by dwMoveOne and doBack -- neither names a room, so the
    // destination field of both reads 0. Any new exit-table walker must
    // skip every row whose DESTINATION FIELD is 0, which is the test the
    // two scanners use: since TASK-FR2 step 3 a gated message row carries
    // an ordinary condTable ID, so "condId >= COND_MSG" no longer finds
    // every roomless row.
    fprintf(outP, "#define COND_MSG 0d%d\n", COND_MSG_ID);
    fprintf(outP, "#define COND_RANDMSG 0d%d\n", COND_RANDMSG_ID);
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
    fprintf(outP, "#define EXIT_DEST 1\n");
    fprintf(outP, "#define MSGTAB_RECORDSIZE 0d2\n");

    // These expect the value to be in the AC
    fprintf(outP, "// Value in AC for these.\n");
    fprintf(outP, "// AC will be nonzero if true\n");
    fprintf(outP, "#define IS_DWARF and [DWARF_FLAG_MASK]\n");
    fprintf(outP, "#define IS_DARK and [DARK_FLAG_MASK]\n");
    fprintf(outP, "#define IS_SURFACE and [SURFACE_FLAG_MASK]\n");
    fprintf(outP, "#define IS_PIRATE_FORBID and [PIRATE_FORBID_MASK]\n\n");

    fprintf(outP, "// Value in AC for these, result in AC.\n");
    // Exit-entry unpacking. These mirror buildRoomRecord()'s shifts (the
    // field layout is in advdataloader.h) and must change with them.
    // GET_EXIT_COUNT reads the room header's word 0; GET_DIRECTION_CODE,
    // GET_CONDITION_ID and GET_RAND_THRESHOLD read exit word 0;
    // GET_ROOM_NUMBER and GET_MSG_INDEX read exit word 1 (EXIT_DEST).
    // A shift of more than nine places needs two sar's.
    fprintf(outP, "#define GET_ROOM_NUMBER and [0xFF]\n");
    fprintf(outP, "#define GET_EXIT_COUNT sar 8s; and [0xFF]\n");
    fprintf(outP, "#define GET_DIRECTION_CODE sar 9s; sar 2s; and [0x7F]\n");
    fprintf(outP, "#define GET_CONDITION_ID sar 6s; and [0x1F]\n");
    fprintf(outP, "#define GET_RAND_THRESHOLD and [0x3F]\n");
    fprintf(outP, "#define GET_MSG_INDEX sar 8s; and [0x3FF]\n\n");

    fprintf(outP, "\n#endif\n");
}

// Emit one room-flag bitmap include -- one bit per room (bit (room-1),
// 1-based room numbers), set if that room carries flagMask in its record
// word 5. Two callers today: emitSurfaceBitmap() (SURFACE, consumed by
// mgDestCheck's isSurfaceRoom) and emitDwarfBitmap() (DWARF, consumed by
// dwMoveOne's confinement filter, TASK-FR13). Both answer the same
// question -- "does room N carry this flag?" for a room whose 64-word
// drum record has NOT been loaded -- so they share one emitter rather
// than keeping two copies that can drift apart.
// buildRoomRecord() is reused as-is (already correctly computes word 5)
// rather than re-deriving flag membership from attributesP by name --
// avoids duplicating flag-lookup logic, and guarantees the bitmap can
// never disagree with the drum record it's describing. nWords is
// computed from the live numRooms, not a hardcoded constant, so the
// bitmap stays renumber/insertion-safe.
// guardP is the include guard AND the "<guard>_WORDS" define stem;
// symbolP is the am1 label the table is emitted under; purposeP is the
// one-sentence "consumed by" note for the generated file's header.
static void
emitRoomFlagBitmap(FILE *outP, Word flagMask, const char *guardP,
                   const char *symbolP, const char *purposeP)
{
Word rec[RECORDSIZE];
int  *bitmap;
int  nWords = (numRooms + 17) / 18;
int  i, w, bitIx;

    bitmap = (int *)calloc(nWords, sizeof(int));

    for( i = 0; i < numRooms; ++i )
    {
        buildRoomRecord(&rooms[i], rec);
        if( rec[5] & flagMask )            // rec[ROOMTAB_FLAGS], word index 5
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
    fprintf(outP, "// room carries the flag named below.\n");
    fprintf(outP, "// %s\n\n", purposeP);
    fprintf(outP, "#ifndef %s\n#define %s\n\n", guardP, guardP);
    fprintf(outP, "#define %s_WORDS 0d%d\n\n", symbolP, nWords);
    fprintf(outP, "%s,\n", symbolP);
    for( w = 0; w < nWords; ++w )
    {
        fprintf(outP, "\t0%o\n", bitmap[w]);
    }
    fprintf(outP, "\n#endif\n");
    free(bitmap);
}

// Emit adv_surfacebitmap.ac -- the SURFACE flag, consumed by mgDestCheck
// (adventure.am1, bank 3's isSurfaceRoom wrapper) via UTIL/bitsets.ac's
// testBitInList, for the one place a room's own SURFACE flag is needed
// before its record has been loaded (a move destination during closing).
// See PendingRework/TASK-ROOM-FLAG-WORD.md.
static void
emitSurfaceBitmap(FILE *outP)
{
    emitRoomFlagBitmap(outP, SURFACE_FLAG, "ADV_SURFACEBITMAP_AH",
                       "SURFACE_BITMAP",
                       "SURFACE: consumed by mgDestCheck via bank 3's isSurfaceRoom.");
}

// Emit adv_dwarfbitmap.ac -- the DWARF flag, consumed by dwMoveOne's
// candidate filter (adventure.am1, bank 3's isDwarfRoom wrapper). This is
// adven.f4 line 696's NEWLOC.LT.15 clause: a dwarf never walks out of the
// cave. The port cannot use the source's room-NUMBER compare because the
// port's room numbering is not monotonic with adven.dat's LOC numbering
// (R_Y2 is port room 15 but LOC 33), so the flag itself is the test, and
// it has to be answerable for a candidate room whose record is not
// loaded -- exactly what SURFACE_BITMAP does for move destinations.
// See CompletedTasks/TASK-FR13-DWARF-CONFINEMENT.md.
static void
emitDwarfBitmap(FILE *outP)
{
    emitRoomFlagBitmap(outP, DWARF_FLAG, "ADV_DWARFBITMAP_AH",
                       "DWARF_BITMAP",
                       "DWARF: consumed by dwMoveOne via bank 3's isDwarfRoom (TASK-FR13).");
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
// Object alias table (TASK-FR7 7b group 1, S14) -- two words per row,
// {vocabulary-word address, OBJ_* index}, scanned by findObj only after
// its walk over objNames has missed. Rows are emitted in corpus order;
// order is match precedence, and no word appears twice, so the order is
// not load-bearing beyond that. NOBJALIAS goes into the defines file so
// the scan is bounded by a count rather than by a sentinel -- the same
// shape as NOBJS, and one word cheaper per row than a terminator.
// ---------------------------------------------------------------------
static void
emitObjAlias(char *dirP, FILE *deffP)
{
int i;
FILE *outP;
char outPath[1024];

    fprintf(deffP, "#define NOBJALIAS 0d%d\n\n", numAliases);

    sprintf(outPath,"%s%s%s", dirP, (*dirP)?"/":"", OBJALIAS_OUTFILE);
    if( !(outP = fopen(outPath, "w")) )
    {
        fprintf(stderr, "Can't create output file '%s'\n", outPath);
        fail();
    }

    fprintf(outP, "// Auto-generated by advdataloader from '%s', do not hand-edit.\n", baseNameP);
    fprintf(outP, "// Regenerate with: advdataloader <srcfile>\n");
    fprintf(outP, "// objAlias table body -- two words per row, {word address, OBJ_* index},\n");
    fprintf(outP, "// %d rows (NOBJALIAS in adv_defines.ah). See findObj's foAliasScan.\n\n", numAliases);

    fprintf(outP, "objAlias,\n");
    for( i = 0; i < numAliases; ++i )
    {
        // ":3", not objNames' ":.". Bank 2 -- which holds every other
        // voc_* string, objNames and three more object tables -- has no
        // room for these 26 rows or for the words they name, so the alias
        // vocabulary and this table both live in bank 3, and the tag is
        // explicit rather than "whatever bank included me".
        fprintf(outP, "    %s:3;\t0d%d\t// %s -> %s\n",
            aliases[i].vocSymP, aliases[i].objIndex, aliases[i].vocSymP, aliases[i].objNameP);
    }
    if( numAliases == 0 )
    {
        fprintf(outP, "    0                          // empty table\n");
    }

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
