// This is an implementation of a line printer for the pdp-1, the Type 64 120 column printer.
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "common.h"
#include "pdp1.h"
#include "iotHandler.h"

//#define DOLOGGING
#include "Logger/iotLogger.h"
#define LOG45 0
#define LOG45ASCII 0
#define LOG45FLEX 0
#define LOG45FILE 0
#define LOG45PRINT 0

#define DEFAULTFILE "/tmp/pdp1lpt.txt"
#define BUFSIZE 120     // 120 column printer

// IOT 2045 clears the print buffer and closes the output file
// IOT 0045 adds characters to the print buffer from the IO register
// IOT 1x45 adds line spacing and prints the buffer, opening the output file if needed
// IOT 3045 gives a new file name or restores the default name
// IOT 3145 sets the character mode to flexo or ascii

#define ERROR 0777776   // -1 in 1's cmpl 12 bit
// Flex conversion
#define LCS -2
#define UCS -3
#define NONE -1

static char *spacing[] = {
    "\r",
    "\n",
    "\n\n",
    "\n\n\n",
    "\n\n\n\n\n\n",
    "\n\n\n\n\n\n\n\n\n\n\n",
    "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n",
    "\n\f"
    };

static int curShift = LCS;          // used for the flex shift char processing
static int bufLoc;                  // location to place next character in buffer
static char buffer[BUFSIZE + 1];    // the print buffer

static char *filenameP = DEFAULTFILE;
static FILE *outfP;

static bool asciiMode;
static bool inWait;                 // completion delay in effect

static int flexoToAscii(char fc, int *shiftP);

int
iotHandler(PDP1 *pdp1P, int dev, int pulse, int completion)
{
int i;
int word, addr, fchar, achar;
char *cP;
bool fail;
bool noWait;

    if( pulse )
    {
        return(1);
    }

    iotCondLog(LOG45, "In lpt iot mb %o dev %o cmpl %d\n", pdp1P->mb, dev, completion);
    inWait = noWait = fail = false;

    if( (pdp1P->mb & 03700) == 02000 )         // 2045, clear buffer
    {
        bufLoc = 0;
        memset(buffer, 0, sizeof(buffer));
        curShift = LCS;
        asciiMode = false;                      // also resets to the default flexo mode

        if( outfP )
        {
            iotCondLog(LOG45FILE, "Closing file\n");
            fclose(outfP);
            outfP = NULL;
        }

        // Handle a pending iot C, we clear it
        if( inWait )
        {
            IOCOMPLETE(pdp1P);
        }

        enablePolling(MSTOCYCLES(30));
        inWait = true;
    }
    else if( (pdp1P->mb & 03700) == 0 )        // 0045, add chars to buffer
    {
        word = pdp1P->io;

        if( asciiMode )
        {
            for( i = 0; i < 2; ++i )
            {
                if( bufLoc >= BUFSIZE )
                {
                    break;                          // no more room
                }

                achar = (word & 0377000) >> 9;
                word <<= 9;
                iotCondLog(LOG45ASCII, "Ascii mode achar 0x%x\n", achar);
                if( !achar )
                {
                    continue;
                }

                buffer[bufLoc++] = achar;
            }
        }
        else
        {
            for( i = 0; i < 3; ++i )
            {
                if( bufLoc >= BUFSIZE )
                {
                    break;                          // no more room
                }

                fchar = (word & 0770000) >> 12;
                word <<= 6;
                if( (achar = flexoToAscii(fchar, &curShift)) == NONE )
                {
                    continue;
                }

                iotCondLog(LOG45FLEX, "Flex mode achar 0x%x\n", achar);
                buffer[bufLoc++] = achar;
            }
        }

        noWait = true;                         // immediate
    }
    else if( (pdp1P->mb & 03000) == 01000 )    // 1x45, print buffer and space
    {
        if( !outfP && !(outfP = fopen(filenameP, "a")) )
        {
            fail = true;                      // sorry
        }
        iotCondLog(LOG45FILE, "Open file '%s', %d\n", filenameP, fail);

        if( !fail )
        {
            // buffer will always be null terminated, just print it if not empty
            if( *buffer )
            {
                if( fputs(buffer, outfP) < 0 )
                {
                    fail = true;
                }
                iotCondLog(LOG45PRINT, "Printed '%s', status %d\n", buffer, fail);
            }

            // what to do for spacing
            if( !fail )
            {
                i = (pdp1P->mb >> 6) & 07;
                if( fputs(spacing[i], outfP) < 0 )
                {
                    fail = true;
                }

                bufLoc = 0;
                if( i != 0 )            // not an overstrike, reset the buffer and shift state
                {
                    curShift = LCS;
                    memset(buffer, 0, sizeof(buffer));
                }

                fflush(outfP);
                enablePolling(MSTOCYCLES(200));
                inWait = true;
            }
        }
    }
    else if( (pdp1P->mb & 03700) == 03000 )    // change file name
    {
        if( outfP )
        {
            fclose(outfP);
            outfP = NULL;
        }

        if( pdp1P->io == 0 )                // just reset the file
        {
            if( filenameP != DEFAULTFILE )
            {
                free(filenameP);
            }

            filenameP = DEFAULTFILE;
            iotCondLog(LOG45FILE, "Open  file, reset to '%s'\n",filenameP);
        }
        else
        {
            // first unpack the file name, we'll use the lp buffer
            addr = pdp1P->io & (MAXMEM - 1);
            iotCondLog(LOG45FILE, "Open  file, io %06o, addr %06o\n", pdp1P->io, addr);
            for( cP = buffer;; )
            {
                word = pdp1P->core[addr++];
                achar = (word & 0377000) >> 9;
                if( !(*cP++ = achar) )
                {
                    break;
                }

                achar = word & 0377;
                if( !(*cP++ = achar) )
                {
                    break;
                }
            }

            filenameP = (char *)malloc(strlen(buffer) + 1);
            strcpy(filenameP, buffer);
            iotCondLog(LOG45FILE, "Open  file, filename '%s'\n", filenameP);

            // and reset
            bufLoc = 0;
            memset(buffer, 0, sizeof(buffer));
            curShift = LCS;
            enablePolling(0);                   // just in case
            if( inWait )
            {
                // Handle a pending iot C, we clear it
                inWait = false;
                IOCOMPLETE(pdp1P);
            }
        }

        noWait = true;                         // immediate
    }
    else if( (pdp1P->mb & 03700) == 03100 )    // change character mode
    {
        asciiMode = pdp1P->io != 0;
        iotCondLog(LOG45FILE, "File mode %d\n", asciiMode);
        noWait = true;
    }
    else
    {
        iotCondLog(LOG45, "Bad lpt instr\n");
        enablePolling(0);
        inWait = false;
        return(0);                          // fail
    }

    if( noWait && completion )
    {
        IOCOMPLETE(pdp1P);                  // this IOT never waits
    }

    if( fail )
    {
        iotCondLog(LOG45, "Fail, closing file\n");
        fclose(outfP);
        outfP = NULL;
        pdp1P->io = ERROR;
    }
    else
    {
        pdp1P->io = 0;
    }

    return(1);
}

// Our 'interrupt' handler.
// If we get here, we were delaying and now done.
void
iotPoll(PDP1 *pdp1P)
{
    inWait = false;
    enablePolling(0);
    IOCOMPLETE(pdp1P);
}

// The usual flex to ascii stuff
// SHIFT | concise to get uppercase
#define SHIFT 0100
#define Red NONE
#define Blk NONE
#define LF NONE

// missing	replacement
// 204	⊃	#
// 205	∨	!
// 206	∧	&
// 211	↑       ^
// 220	→	\
// 273	×	*
// 140	·	@
// 156	‾	`

static const char concise2ascii[] = {
        ' ', '1', '2', '3', '4', '5', '6', '7',         // 00-07
        '8', '9', LF, NONE, NONE, NONE, NONE, NONE,     // 10-17
        '0', '/', 's', 't', 'u', 'v', 'w', 'x',         // 20-27
        'y', 'z', NONE, ',', Blk, Red, '\t', NONE,      // 30-37
        '@', 'j', 'k', 'l', 'm', 'n', 'o', 'p',         // 40-47
        'q', 'r', NONE, NONE, '-', ')', '`', '(',       // 50-57
        NONE, 'a', 'b', 'c', 'd', 'e', 'f', 'g',        // 60-67
        'h', 'i', LCS, '.', UCS, '\b', NONE, '\n',      // 70-77

        ' ', '\"', '\'', '~', '#', '!', '&', '<',        // same, shifted
        '>', '^', LF, NONE, NONE, NONE, NONE, NONE,
        '\\', '?', 'S', 'T', 'U', 'V', 'W', 'X',
        'Y', 'Z', NONE, '=', Blk, Red, '\t', NONE,
        '_', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
        'Q', 'R', NONE, NONE, '+', ']', '|', '[',
        NONE, 'A', 'B', 'C', 'D', 'E', 'F', 'G',
        'H', 'I', LCS, '*', UCS, '\b', NONE, '\n'
};

// Returns NONE if the character should be ignored, else the ascii char.
// shiftP holds the current shift state.
static int
flexoToAscii(char fc, int *shiftP)
{
int ac;
    
    fc &= 0177;                 // in case it's actually fiodec, convert to concise

    if( *shiftP )
    {
        fc |= SHIFT;
    }

    ac = concise2ascii[fc];
    if( ac == NONE )
    {
        return(NONE);
    }

    if( ac == LCS )
    {
        *shiftP = 0;
        return(NONE);
    }

    if( ac == UCS )
    {
        *shiftP = 1;
        return(NONE);
    }

    return(ac);
}
