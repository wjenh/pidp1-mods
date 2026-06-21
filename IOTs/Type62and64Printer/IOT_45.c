/*
 * This is an implementation of line printers for the pdp-1, the Type 62 abd Type 64 printers.
 *
 * 21-Jun-2026 wje minor revision for some timing changes
 *
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "configuration.h"
#include "iotHandler.h"
#include "flexlib.h"

//#define DOLOGGING
#include "iotLogger.h"

#define LOG45 0
#define LOG45ASCII 0
#define LOG45FLEX 0
#define LOG45FILE 0
#define LOG45PRINT 0
#define LOG45CONFIG 0
#define LOG45FF 0

#define DEFAULTFILE "/tmp/pdp1lpt.txt"
#define BUFSIZE 120     // 120 column printer

// Some versions of the Type 62 could do 600 lpm.
#define TYPE62LINEDELAY 16  // milliseconds per
#define TYPE62PRINTDELAY 84  // milliseconds per

#define TYPE64LINEDELAY 32 // milliseconds per
#define TYPE64PRINTDELAY 168 // milliseconds per

// Buffer clear/reset (lpc).
// No documented value could be confirmed, so this is derived from the printer's rated 300 lines/minute.
#define TYPE64CLEARDELAY 5 // milliseconds

#define ERROR 0777776   // -1 in 1's cmpl 12 bit

// Define the actions that can be done, bits that are or'd
#define PRINT   0x1     // print the current buffer, reset buffer counter to 0
#define SPACE   0x2     // do line spacing
#define ADD     0x4     // add chars to buffer
#define OVER    0x10    // set buffer counter to 0
#define RESET   0x40    // reset everything
#define LPM     0x400   // lpm, same for both 62 and 64
#define LPF     0x1000  // lpf, same for both 62 and 64

// These are the defaults, entries 1-6 overridden by any config file setting
static int spacing64[] = {
    0,          // overstrike
    1,
    2,
    3,
    6,
    11,
    22,
    -1          // marker for formfeed
    };

static int spacing62[] = {
    1,
    2,
    3,
    4,
    11,
    22,
    33,
    -1          // marker for formfeed
    };

static int curShift = LCS;          // used for the flex shift char processing
static int bufLoc;                  // location to place next character in buffer
static int lineNo;                  // number of lines done
static int linesPerPage = 66;       // override in config
static char buffer[BUFSIZE + 1];    // the print buffer

static char *filenameP = DEFAULTFILE;
static FILE *outfP;

static bool configDone;             // config loaded
static bool type64;                 // emulating a type 64, else a type 62
static bool asciiMode;
static bool noFF;

static bool inWait;                 // completion delay in effect

static void configure(void);

extern int flexToAscii(char fc, int *shiftP);
extern int getFileName(PDP1P pdp1P, unsigned int addr, char *bufP);

int
iotHandler(PDP1 *pdp1P, int dev, int pulse, int completion)
{
int i, j;
int word, addr;
int actions;
int spaceval;
int delaytime;
int fchar, achar;
bool fail;
bool noWait;

    if( pulse )
    {
        return(1);
    }

    iotCondLog(LOG45, "In lpt iot mb %o dev %o cmpl %d\n", MB(pdp1P), dev, completion);
    inWait = noWait = fail = false;

    if( !configDone )
    {
        configure();
        configDone = true;
    }

    // Figure out the type 62 vs 64 diffs.
    // 62:
    // 0045, print no advance
    // 1045, add chars
    // 2x45, space
    // 64:
    // 0045, add chars
    // 1x45, print and space
    // 2045, reset
    actions = 0;
    delaytime = 0;  // if 0, no delay, else cycles

    if( (MB(pdp1P) & 03700) == 0 )             // 0045
    {
        actions = (type64)?ADD:OVER;
    }
    else if( (MB(pdp1P) & 03000) == 01000 )    // 1x45
    {
        if( type64 )
        {
            spaceval = (MB(pdp1P) >> 6) & 07;
            if( spaceval )
            {
                actions = PRINT|SPACE;
            }
            else
            {
                // x=0 is overstrike.
                // Like the Type 62 prl, defer the actual write to the
                // output file until a real line advance happens rather than printing and
                // clearing the pre-overstrike buffer immediately.
                actions = OVER;
            }
        }
        else
        {
            actions = ADD;
        }
    }
    else if( (MB(pdp1P) & 03000) == 02000 )    // 2x45
    {
        if( type64 )
        {
            actions = RESET;
        }
        else
        {
            actions = PRINT|SPACE;
            spaceval = (MB(pdp1P) >> 6) & 07;
        }
    }
    else if( (MB(pdp1P) & 03700) == 03000 )    // 3045
    {
        actions = LPF;
    }
    else if( (MB(pdp1P) & 03700) == 03100 )    // 3145
    {
        actions = LPM;
    }

    // Now do the processing.
    if( actions & RESET )
    {
        lineNo = 1;
        bufLoc = 0;
        memset(buffer, 0, sizeof(buffer));
        curShift = LCS;
        asciiMode = false;                      // also resets to the default flexo mode
        delaytime = 0;

        if( outfP )
        {
            iotCondLog(LOG45FILE, "Closing file\n");
            fclose(outfP);
            outfP = NULL;
        }

        delaytime = MSTOCYCLES(TYPE64CLEARDELAY);
        inWait = true;
    }

    if( !outfP )
    {
        if( !(outfP = fopen(filenameP, "a")) )
        {
            fail = true;                      // sorry
        }

        iotCondLog(LOG45FILE, "Open file '%s', %d\n", filenameP, fail);
    }

    if( !fail && (actions & ADD) )                     // put chars in buffer
    {
        word = IO(pdp1P);

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
                if( (achar = flexToAscii(fchar, &curShift)) == NONE )
                {
                    continue;
                }

                iotCondLog(LOG45FLEX, "Flex mode achar 0x%x\n", achar);
                buffer[bufLoc++] = achar;
            }
        }

        noWait = true;                         // immediate
    }

    // This needs to be separate because the 62 and 64 handle overstrikes differently.
    // This is the Type 62 prl path (bare OVER) and the Type 64 pas-with-x=0 path (also bare
    // OVER, see above).
    // Neither actually prints anything here, but the real printer still ran
    // its print cycle (just without advancing the paper), so this needs the same completion
    // delay as an actual print.
    if( actions & OVER )
    {
        bufLoc = 0;
        delaytime += (type64)?MSTOCYCLES(TYPE64PRINTDELAY):MSTOCYCLES(TYPE62PRINTDELAY);
        inWait = true;
    }

    // do before SPACE
    if( !fail && (actions & PRINT) )
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

        bufLoc = 0;
        curShift = LCS;
        memset(buffer, 0, sizeof(buffer));
    }

    if( !fail && (actions & SPACE) )
    {
        // what to do for spacing
        i = (type64)?spacing64[(MB(pdp1P) >> 6) & 07]:spacing62[(MB(pdp1P) >> 6) & 07];
        iotCondLog(LOG45FF, "SPACE, spacing %d\n", i);

        if( i == -1 )      // form feed
        {
            // The delay time is not certain, assume same as one line advance time per remaining lines
            j = linesPerPage - lineNo;
            iotCondLog(LOG45FF, "FF, lpp %d, lineNo %d\n", linesPerPage, lineNo);

            if( noFF )
            {
                iotCondLog(LOG45FF, "FF with noFF\n");
                // We go one more to termiate the current line
                for( i = 0; i <= j; ++i )
                {
                    fputc('\n', outfP);
                }
            }
            else
            {
                iotCondLog(LOG45FF, "FF using formfeed\n");
                fputs("\n\f", outfP);
            }

            lineNo = 1;
            delaytime += (type64)?MSTOCYCLES(TYPE64LINEDELAY * j):MSTOCYCLES(TYPE62LINEDELAY * j);
            iotCondLog(LOG45FF, "FF delay time %d\n", delaytime);
        }
        else
        {
            while( i-- > 0 )
            {
                ++lineNo;
                if( lineNo >= linesPerPage )
                {
                    lineNo = 1;
                }

                fputc('\n', outfP);
                delaytime += (type64)?MSTOCYCLES(TYPE64LINEDELAY):MSTOCYCLES(TYPE62LINEDELAY);
            }
        }

        // reset the buffer and shift state
        bufLoc = 0;
        curShift = LCS;
        memset(buffer, 0, sizeof(buffer));

        fflush(outfP);
        inWait = true;
    }

    // Do even if there was a fail
    if( actions == LPF )
    {
        if( outfP )
        {
            fclose(outfP);
            outfP = NULL;
        }

        if( IO(pdp1P) == 0 )                // just reset the file
        {
            if( filenameP != DEFAULTFILE )
            {
                free(filenameP);
            }

            filenameP = DEFAULTFILE;
            iotCondLog(LOG45FILE, "File reset to '%s'\n",filenameP);
        }
        else
        {
            // first unpack the file name, we'll use the lp buffer
            addr = IO(pdp1P) & (MAXMEM - 1);
            iotCondLog(LOG45FILE, "Open  file, io %06o, addr %06o\n", IO(pdp1P), addr);
            if( !getFileName(pdp1P, addr, buffer) )
            {
                fail = true;
            }
            else
            {
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
        }

        noWait = true;                         // immediate
    }

    // Do even if there was a fail
    if( actions  == LPM )                      // change character mode, close file
    {
        asciiMode = IO(pdp1P) & 1;
        iotCondLog(LOG45FILE, "File mode %d\n", asciiMode);

        // This does almost what the Type 64 reset command does, the Type 62 doesn't have a reset,
        // and that's how we close the output file.
        // It does not change ascii mode though, the above bit does that.
        if( IO(pdp1P) & 2 )
        {
            if( outfP )
            {
                fclose(outfP);
                outfP = NULL;
                iotCondLog(LOG45FILE, "File closed\n");
            }

            lineNo = 1;
            bufLoc = 0;
            memset(buffer, 0, sizeof(buffer));
            curShift = LCS;
        }

        noWait = true;
    }

    if( !fail && delaytime && !noWait )
    {
        enablePolling(delaytime);
    }

    if( noWait && completion )
    {
        IOCOMPLETE(pdp1P);                  // this IOT never waits
    }

    if( fail )
    {
        iotCondLog(LOG45, "Fail, closing file\n");
        if( outfP )
        {
            fclose(outfP);
            outfP = NULL;
        }

        IO(pdp1P) = ERROR;
    }
    else
    {
        IO(pdp1P) = 0;
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

void
configure()
{
int i, ival;
char *cP;
ConfigurationSettingP settingP;

    if( (settingP = findConfigurationSetting(getConfiguration(), "lptType64")) )
    {
        iotCondLog(LOG45CONFIG, "In lpt, lptType64 %d\n", settingP->onOff);
        type64 = settingP->onOff;
    }

    if( (settingP = findConfigurationSetting(getConfiguration(), "lptLineSpacing")) )
    {
        iotCondLog(LOG45CONFIG, "In lpt, lptLineSpacing %s\n", settingP->strvalueP);
        // pick up no more than 8 values
        for( cP = settingP->strvalueP, i = 0; cP && *cP && (i < 8); ++i)
        {
            if( (ival = atoi(cP)) )
            {
                iotCondLog(LOG45CONFIG, "In lpt, spacing %d is %d\n", i, ival);
                spacing62[i] = spacing64[i] = ival;
            }

            if( (cP = strchr(cP, ',')) )
            {
                ++cP;
            }
        }
    }

    if( (settingP = findConfigurationSetting(getConfiguration(), "lptLines")) )
    {
        iotCondLog(LOG45CONFIG, "In lpt, lines per page %d\n", settingP->ivalue);
        linesPerPage = settingP->ivalue;
    }

    if( (settingP = findConfigurationSetting(getConfiguration(), "lptNoFF")) )
    {
        noFF = settingP->onOff;
        iotCondLog(LOG45CONFIG, "In lpt, noFF %d\n", noFF);
    }
}
