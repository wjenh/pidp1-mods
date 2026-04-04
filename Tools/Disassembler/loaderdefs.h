#ifndef LOADERDEFS_H
#define LOADERDEFS_H
#include "loader.h"
// Update this file to add new loaders.

// Our list of supported loaders
extern int kloader(FILE *fP, int directive, int *addrP, int *wordP, char *args[]);
extern int am1loader(FILE *fP, int directive, int *addrP, int *wordP, char *args[]);
extern int binloader(FILE *fP, int directive, int *addrP, int *wordP, char *args[]);
extern int memloader(FILE *fP, int directive, int *addrP, int *wordP, char *args[]);

LoaderMap loaders[] = {
    {"bin", binloader, "This loads tapes in the usual ddt BIN format"},
    {"am1", am1loader, "This loads tapes produced by am1 with its extended loader"},
    {"kalah", kloader, "This loads the kalah program with its unknown loader"},
    {"mem", memloader, "This loads from a memory file produced by pidp-1, -Lmem,begin,end,start"},
    0
};

#endif
