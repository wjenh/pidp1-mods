#ifndef LOADER_H
#define LOADER_H
// Define various things used for implementing a loader for the disassembler.

// Directives passed to loaders
// INIT is passed before the loader gets any other directive
#define LOADER_CMD_START    1
#define LOADER_CMD_NEXT     2
#define LOADER_CMD_INIT     3

// Loaders return special status values for the INIT call, the values are flags.
// LOADER_ERROR can still be returned and will always be recognized.
#define LOADER_INIT_NONE     0x0        // just a marker for no special flags
#define LOADER_INIT_NORIM    0x1        // don't attempt to look for any RIM data

// Statuses returned from loaders
#define LOADER_OK       0   // generic ok, the directive worked
#define LOADER_DONE     1   // the terminating condition was seen, start address returned in addrP or -1 if none
#define LOADER_MORE     2   // more words to come
#define LOADER_AGAIN    3   // more words to come, but ignore this result, call again
#define LOADER_STOP     4   // am1 loader saw a stop, not a regular start

#define LOADER_ERROR    -1   // bad format, eof, etc

// initArgs is only passed for INIT, else ignore it, could be null
typedef int (*LoaderP)(FILE *fP, int directive, int *addressP, int *wordP, char *initArgs[]);

// This is used to hold loader name to function mapping
typedef struct
{
    char *nameP;
    LoaderP loaderP;
    char *descriptionP;
} LoaderMap, *LoaderMapP;

#endif
