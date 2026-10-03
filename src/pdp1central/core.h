#ifndef CORE_H
#define CORE_H
// The pdp1central core: everything the control app does that is not drawing.
// Process status, the localhost command ports, running one child program without blocking,
// the name=value settings files, the schema that describes pidp1.config, and the microtape
// mount list.
// Nothing here calls SDL or Nuklear, so the front end can be replaced without touching it.
// Single-threaded: every call returns within a port timeout, and the front end calls them
// from its one event loop.

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#define CONF_MAX_LINE 255       // configuration.c reads lines with fgets() into 256 bytes
#define CONF_MAX_VALUE 63       // and the name and the value each with %63

// A name=value file kept as its lines, so a save changes only what was edited.
typedef struct _ConfFile
{
    char *pathP;
    char **linesP;              // each line without its newline
    int lineCount;
    int lineCap;
    bool finalNewline;          // the last line ended with a newline
    bool existed;               // the file was there when loaded
    bool backedUp;              // pidp1.config.bak was made by this session's first save
    struct timespec loadedMtime;
    off_t loadedSize;
    char valueText[CONF_MAX_VALUE + 1];     // confGet's result
    char errorText[256];        // why the last call failed
} ConfFile;

// Setting types, from the schema's type column.
typedef enum
{
    SCHEMA_BOOL,
    SCHEMA_INT,
    SCHEMA_FLOAT,
    SCHEMA_TEXT,
    SCHEMA_PORT
} SchemaType;

// When a change to a setting takes effect, from the schema's applies column.
typedef enum
{
    APPLIES_RELOAD,             // SIGHUP to pdp1
    APPLIES_PANEL,              // SIGHUP to the panel driver
    APPLIES_RUN,                // SIGHUP to pdp1, then its plugin reads it as the machine next runs
    APPLIES_RESTART,            // pdp1 restart
    APPLIES_PROGRAM,            // restart of the program that reads it
    APPLIES_NONE                // retired, ignored
} SchemaApplies;

typedef struct
{
    char name[CONF_MAX_VALUE + 1];
    char group[32];
    SchemaType type;
    bool hasRange;
    double rangeLow;
    double rangeHigh;
    char defaultText[CONF_MAX_VALUE + 1];   // "-" when no reader has a default
    char reader[64];
    SchemaApplies applies;
    bool sticky;                // back to the default needs a restart; a new value applies as above
    char label[CONF_MAX_VALUE + 1];         // shown instead of the name; "" for none
    char summary[256];
} SchemaEntry;

typedef struct
{
    SchemaEntry *entriesP;
    int count;
} Schema;

#define MT_DRIVES 8             // Type 550 drives 1-8
#define MT_SPEC_MAX 272         // the plugin's entry limit: a 255-character path, then ",locked"

// microtapes.txt as the Microtape plugin reads it. Arrays are indexed by drive, 1-8.
typedef struct
{
    char spec[MT_DRIVES + 1][MT_SPEC_MAX];  // the drive's entry as written, "" for no line
    char path[MT_DRIVES + 1][MT_SPEC_MAX];  // its path without ",locked", "" if it has none
    bool locked[MT_DRIVES + 1];
    int skipped;                // lines that are not blank or comments and do not parse
    int error;                  // errno for a file that exists and could not be read, else 0
    bool exists;
    ino_t ino;
    off_t size;
    struct timespec mtime;
    struct timespec ctime;
} MtList;

// procs.c
int procCount(const char *nameP);
bool portAnswers(int port);

// cmdport.c
int portCommand(int port, const char *lineP, char *replyP, size_t replyLen);
int tapeCommand(int frontPort, int emuPort, const char *lineP, char *replyP, size_t replyLen);

// child.c
bool childStart(char *const argvP[]);
int childPoll(char *outP, size_t outLen);
int childStatus(void);
bool childRunning(void);

// conffile.c
ConfFile *confLoad(const char *pathP);
void confFree(ConfFile *cfP);
ConfFile *confCopy(const ConfFile *cfP);
bool confRetarget(ConfFile *cfP, const char *pathP);
const char *confGet(ConfFile *cfP, const char *nameP);
bool confSet(ConfFile *cfP, const char *nameP, const char *valueP);
bool confUnset(ConfFile *cfP, const char *nameP);
bool confSave(ConfFile *cfP);
bool confChangedOnDisk(ConfFile *cfP);
int confNames(ConfFile *cfP, char namesP[][CONF_MAX_VALUE + 1], int maxNames);

// mtlist.c
bool mtListLoad(const char *pathP, MtList *listP);
bool mtListChanged(const char *pathP, const MtList *listP);

// schema.c
Schema *schemaLoad(const char *pathP, char *errorP, size_t errorLen);
void schemaFree(Schema *schemaP);
const SchemaEntry *schemaFind(const Schema *schemaP, const char *nameP);
bool schemaCheck(const SchemaEntry *entryP, const char *valueP, char *errorP, size_t errorLen);
bool schemaIsOn(const char *valueP);
const char *schemaAppliesText(SchemaApplies applies);
const char *schemaDisplayName(const SchemaEntry *entryP);

#endif
