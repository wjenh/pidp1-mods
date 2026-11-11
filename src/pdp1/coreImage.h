// pdp1.c must not include this file, its static readmem() is a different function.
#ifndef COREIMAGE_H
#define COREIMAGE_H

#include "pdp1.h"

void readmem(const char *filenameP, Word *mem, Word size);
void dumpmem(const char *fileNameP, Word *mem, Word size);

#endif
