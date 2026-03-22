// This is an implementation of a line printer for the pdp-1, the Type 64 120 column printer.
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "common.h"
#include "pdp1.h"
#include "configuration.h"
#include "iotHandler.h"

//#define DOLOGGING
#include "Logger/iotLogger.h"
#define LOG45 0
#define LOG45ASCII 0
#define LOG45FLEX 0
#define LOG45FILE 0
#define LOG45PRINT 0
#define LOG45CONFIG 0
#define LOG45FF 0

#define DEFAULTFILE "/tmp/pdp1lpt.txt"
#define BUFSIZE 120     // 120 column printer

#define TYPE62LINEDELAY 16  // milliseconds per
#define TYPE62PRINTDELAY 84  // milliseconds per

#define TYPE64LINEDELAY 32 // milliseconds per
#define TYPE64PRINTDELAY 168 // milliseconds per

#define ERROR 0777776   // -1 in 1's cmpl 12 bit
// Flex conversion
#define LCS -2
#define UCS -3
#define NONE -1

// Define the actions that can be done, bits that are or'd
#define PRINT   0x1     // print the current buffer, reset buffer counter to 0
#define SPACE   0x2     // do line spacing
#define ADD     0x4     // add chars to buffer
#define OVER    0x10    // set buffer counter to 0
#define CLOSE   0x20    // close the output file
#define RESET   0x40    // reset everything
#define RETURN  0x100   // print an immediate cr
#define NL      0x200   // print an immediate nl
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

static int flexoToAscii(char fc, int *shiftP);
static void configure(void);

int
iotHandler(PDP1 *pdp1P, int dev, int pulse, int completion)
{
int i, j;
int word, addr;
int actions;
int spaceval;
int delaytime;
int fchar, achar;
char *cP;
bool fail;
bool noWait;

    if( pulse )
    {
        return(1);
    }

    iotCondLog(LOG45, "In lpt iot mb %o dev %o cmpl %d\n", pdp1P->mb, dev, completion);
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

    if( (pdp1P->mb & 03700) == 0 )             // 0045
    {
        actions = (type64)?ADD:OVER;
    }
    else if( (pdp1P->mb & 03000) == 01000 )    // 1x45
    {
        if( type64 )
        {
            spaceval = (pdp1P->mb >> 6) & 07;
            if( spaceval )
            {
                actions = PRINT|SPACE;
            }
            else
            {
                actions = PRINT|OVER|RETURN;
            }
        }
        else
        {
            actions = ADD;
        }
    }
    else if( (pdp1P->mb & 03000) == 02000 )    // 2x45
    {
        if( type64 )
        {
            actions = RESET;
        }
        else
        {
            actions = PRINT|SPACE;
            spaceval = (pdp1P->mb >> 6) & 07;
        }
    }
    else if( (pdp1P->mb & 03700) == 03000 )    // 3045
    {
        actions = LPF;
    }
    else if( (pdp1P->mb & 03700) == 03100 )    // 3145
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

        delaytime = MSTOCYCLES(30);
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

    if( actions & ADD )                     // put chars in buffer
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

    // This needs to be separate because the 62 and 64 handle overstrikes differently
    if( actions & OVER )
    {
        bufLoc = 0;
    }

    // do thse before PRINT and SPACE
    if( actions & RETURN )
    {
        fputc('\r', outfP);
    }

    if( actions & NL )
    {
        fputc('\n', outfP);
        ++lineNo;
        delaytime += (type64)?TYPE64PRINTDELAY:TYPE62PRINTDELAY;
    }

    // do before SPACE
    if( actions & PRINT )
    {
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
        }

        bufLoc = 0;
        curShift = LCS;
        memset(buffer, 0, sizeof(buffer));
    }

    if( actions & SPACE )
    {
        // what to do for spacing
        if( !fail )
        {
            i = (type64)?spacing64[(pdp1P->mb >> 6) & 07]:spacing62[(pdp1P->mb >> 6) & 07];
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
            enablePolling(MSTOCYCLES(200));
            inWait = true;
        }
    }

    if( actions == LPF )
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

    if( actions  == LPM )                      // change character mode, close file
    {
        asciiMode = pdp1P->io & 1;
        iotCondLog(LOG45FILE, "File mode %d\n", asciiMode);

        // This does almost what the Type 64 reset command does, the Type 62 doesn't have a reset,
        // and that's how we close the output file.
        // It does not change ascii mode though, the above bit does that.
        if( pdp1P->io & 2 )
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

    if( delaytime && !noWait )
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

void
configure()
{
int i, ival;
char *cP;
ConfigurationP confP;
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
