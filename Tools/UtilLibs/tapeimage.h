/*
 * The binary tape reader shared by fastload and ovlload; see tapeimage.c.
 */

#ifndef TAPEIMAGE_H
#define TAPEIMAGE_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "ad1link.h"

#define LOADFAILED  -1
#define LOADSTOP    -2

int loadTape(char *filenameP);
bool sendImage(Ad1Link *linkP, bool *wasRunningP);
bool tapeImageWord(uint32_t address, uint32_t *wordP);
bool tapeMemFileWritable(const char *nameP);
bool tapeWriteMemFile(FILE *memFileFp);

#endif
