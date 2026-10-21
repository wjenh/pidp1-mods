// preprocess.h - am1's preprocessor, see preprocess.c

#ifndef PREPROCESS_H
#define PREPROCESS_H

#include <stdbool.h>

// One level of a statement's origin: a macro expansion that made it.
typedef struct PPOriginLevel
{
    const char *macroP;         // the macro's name
    const char *fileP;          // the file it is defined in, as the markers name it
    int line;                   // the line of its body the statement came from
} PPOriginLevel;

// Where a statement made by macros came from: each expansion, from the macro
// the program used inward.
typedef struct PPOrigin
{
    int count;
    PPOriginLevel *levelsP;
} PPOrigin;

// Record a -D (type 'D', "name" or "name=value") or -I (type 'I', a directory),
// in command-line order.
void ppAddOption(char type, const char *valueP);

// Preprocess srcNameP into outNameP, including <...> files from incRootP after
// the -I directories.
// Returns true on success, false after reporting the errors on stderr.
bool preprocess(const char *srcNameP, const char *outNameP, const char *incRootP);

// The origin of the statement am1 reads as the ordinal-th (from 0) on line of
// fileP, as the markers number and name it.
// NULL for a statement no macro made and for one not found.
// Asked in the order am1 reads the statements.
const PPOrigin *ppStatementOrigin(const char *fileP, int line, int ordinal);

#endif
