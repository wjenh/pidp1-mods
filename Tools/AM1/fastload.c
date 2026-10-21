/*
 * Load a binary tape as if it had been done via read-in.
 * A tape in bin or am1 format can be loaded.
 * There are two modes.
 * If the emulator is running, the tape can be loaded directly into the
 * pdp-1 live memory over the emulator's debugger link (TCP, the ad1port
 * setting in pidp1.config; -h names a host on another machine).
 * If not, the core-image save file, coremem, can be overwritten
 * with the tape so that on the next pdp-1 boot the program will
 * be memory resident.
 *
 * If you're tired of waiting for those simulated tape reads,
 * here's your answer.
 *
 * 26-Aug-2026 wje initial version
 * 28-Aug-2026 wje add load-to-memory-file
 * 12-Sep-2026 wje add more detail to usage
 * 20-Sep-2026 claude replace the shared-memory link with the network link; a bad tape changes nothing
 * 22-Sep-2026 wje add ppporch's len and lineP initialization
 * 25-Sep-2026 claude initialize lineP and len before the second getline too
 * 2-Oct-2026 wje add -y option to autostart
 * 3-Oct-2026 wje clarify messages re the emulator being halted and a program being started
 * 12-Oct-2026 claude move the tape reader and the link sender to tapeimage.c, shared with ovlload
 * 21-Oct-2026 claude a tape with a .ovl beside it has its drum overlays written first, as ovlload
 *   does, by the shared ovlfile.c; -i names the drum file
 * 21-Oct-2026 claude a failed -m load leaves the memory file as it was, it is written last
*/

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "ad1link.h"
#include "tapeimage.h"
#include "ovlfile.h"

#define DEFAULT_MEMFILE "/opt/pidp1-mods/coremem"
#define DEFAULT_DRUM "/opt/pidp1-mods/pdp23drum"

// A program with drum overlays: its .ovl, and the drum as it was before they were written.
static OvlFile ovl;
static int32_t drum[OVL_DRUMWORDS];
static long drumFileWords;

static bool checkOverlays(char *tapeP, const char *ovlNameP, int *addressP);
static bool writeOverlays(const char *drumNameP);
static bool writeMemFile(const char *nameP, bool drumWritten);
static void usage(void);

int
main(int argc, char **argv)
{
int address;
bool useMemFile;
size_t len;
char *cP;
char *filenameP;
char *lineP;
char *memFileNameP;
char *hostP;
char *drumNameP;
char *ovlNameP;
Ad1Link link;
bool wasRunning;
bool stillRunning;
bool autoStart;
bool parsed;
bool putOnDrum;
int status;
uint32_t run;
uint32_t pc;
uint32_t state[AD1P_STATE_WORDS];

    memFileNameP = DEFAULT_MEMFILE;
    drumNameP = DEFAULT_DRUM;
    hostP = NULL;
    useMemFile = false;
    autoStart = false;

    // do the command line processing
    ++argv;
    --argc;

    while(argc && (**argv == '-'))                        /* look for directives */
    {
        for(cP = *argv + 1; *cP;)
        {
            switch(*cP++)
            {
            case 'm':
                useMemFile = true;
                break;

            case 'y':
                autoStart = true;
                break;

            case 'f':               // memfile to use if not the default
                --argc;
                ++argv;
                memFileNameP = *argv;
                break;

            case 'i':               // the drum file a program's overlays go to, if not the default
                --argc;
                ++argv;
                if( argc < 1 )
                {
                    usage();
                }

                drumNameP = *argv;
                break;

            case 'h':               // host[:port] of the emulator, if not this machine
                --argc;
                ++argv;
                if( argc < 1 )
                {
                    usage();
                }

                hostP = *argv;
                break;

            default:
                usage();
                break;
            }
        }

        --argc;
        ++argv;
    }

    if(argc != 1)
    {
        usage();
    }

    filenameP = *argv;

    // A tape with an .ovl is a program with drum overlays.
    // The two are checked against each other and the drum file read
    // before anything is written.
    // The overlays then go on the drum before the program is stored, as ovlload does.
    ovlSetProgram("fastload", stderr);
    if( !(ovlNameP = ovlNameForTape(filenameP)) )
    {
        fprintf(stderr, "fastload: out of memory.\n");
        exit(1);
    }

    parsed = false;
    putOnDrum = false;
    if( access(ovlNameP, F_OK) == 0 )
    {
        if( !checkOverlays(filenameP, ovlNameP, &address) )
        {
            exit(1);
        }

        parsed = true;
        if( !useMemFile && ad1LinkIsRemote(hostP) )
        {
            printf("The pdp-1 is on another machine: the overlays in '%s' are not written to its drum;"
                " run ovlload there.\n", ovlNameP);
        }
        else if( !ovlReadDrum(drumNameP, drum, &drumFileWords) )
        {
            printf("Nothing was written.\n");
            exit(1);
        }
        else
        {
            putOnDrum = true;
        }
    }

    if( useMemFile )
    {
        printf("Loading into memory file, be sure the pdp-1 isn't running!\n");
        printf("If it is, this load will be overwritten when it exits.\n");
        printf("Continue? (y/n) ");
        lineP = 0;
        len = 0;
        if( (getline(&lineP, &len, stdin)) != -1 )
        {
            if( *lineP != 'y' )
            {
                exit(0);
            }
        }

        // Asked before the drum is touched, without emptying the file.
        // It is zeroed and written only after everything else has succeeded.
        if( !tapeMemFileWritable(memFileNameP) )
        {
            fprintf(stderr, "Can't open memory file '%s', be sure the directory and file have write permission.\n",
                memFileNameP);
            exit(1);
        }

        // checkOverlays() has parsed it already, a tape with no .ovl is parsed here.
        if( !parsed && ((address = loadTape(filenameP)) == LOADFAILED) )
        {
            fprintf(stderr, "Can't load the tape, is it a valid macro or am1 rim tape?\n");
            exit(1);
        }

        if( putOnDrum && !writeOverlays(drumNameP) )
        {
            printf("The memory file was not written.\n");
            exit(1);
        }

        if( !writeMemFile(memFileNameP, putOnDrum) )
        {
            exit(1);
        }

        if( address == LOADSTOP )
        {
            printf("Tape loaded, an am1 program with a stop directive.\n");
            exit(0);
        }

        printf("Tape loaded, start address 0%0o.\n", address);
    }
    else
    {
        // Parse the whole tape before touching the emulator.
        // CheckOverlays() has parsed it already when there is a .ovl.
        if( !parsed && ((address = loadTape(filenameP)) == LOADFAILED) )
        {
            fprintf(stderr, "Can't load the tape, is it a valid macro or am1 rim tape?\n");
            exit(1);
        }

        if( ad1LinkOpen(&link, hostP, 0, "fastload") != 0 )
        {
            fprintf(stderr, "%s\n", ad1LinkError());
            exit(1);
        }

        // Stopped before the overlays are written so no running program uses
        // the drum while it changes.
        // STOP's reply gives the state after the stop, so whether its running is checked first.
        wasRunning = false;
        if( putOnDrum )
        {
            if( (status = ad1GetState(&link, state)) != AD1P_ST_OK )
            {
                fprintf(stderr, "Can't read the pdp-1's state: %s\n", ad1StatusText(status));
                ad1LinkClose(&link);
                exit(1);
            }

            if( state[AD1P_STATE_RUN] && ((status = ad1Stop(&link, &run, &pc)) != AD1P_ST_OK) )
            {
                fprintf(stderr, "Can't stop the pdp-1: %s\n", ad1StatusText(status));
                ad1LinkClose(&link);
                exit(1);
            }

            wasRunning = (state[AD1P_STATE_RUN] != 0);
            if( !writeOverlays(drumNameP) )
            {
                ad1LinkClose(&link);
                printf("The program was not loaded; the pdp-1 is stopped.\n");
                exit(1);
            }
        }

        // Stops the pdp-1 if it is running then stores every block.
        stillRunning = false;
        if( !sendImage(&link, &stillRunning) )
        {
            ad1LinkClose(&link);
            exit(1);
        }

        if( wasRunning || stillRunning )
        {
            printf("The pdp-1 was running and has been halted.\n");
        }

        if( address == LOADSTOP )
        {
            printf("Tape loaded, an am1 program with a stop directive.\n");
            ad1LinkClose(&link);
            exit(0);
        }

        if( !autoStart )
        {
            printf("Tape loaded, start address 0%0o. Start it (y or n/newline)?\n", address);
        }

        // The memory-file path above has its own pair.
        lineP = 0;
        len = 0;
        if( autoStart || ((getline(&lineP, &len, stdin)) != -1) )
        {
            if( autoStart || (*lineP == 'y') )
            {
                if( (status = ad1Start(&link, (uint32_t)address, NULL, NULL)) != AD1P_ST_OK )
                {
                    fprintf(stderr, "Can't start the pdp-1: %s\n", ad1StatusText(status));
                    ad1LinkClose(&link);
                    exit(1);
                }

                printf("The pdp-1 program has been started.\n");
            }
        }

        ad1LinkClose(&link);
    }

    exit(0);
}

// Read the tape's .ovl and check it against the tape.
// The start address is returned in *addressP.
// Returns false after reporting an error if there is a mismatch.
static bool
checkOverlays(char *tapeP, const char *ovlNameP, int *addressP)
{
char err[OVL_ERRSIZE];
uint32_t stampWord;
bool haveStamp;

    if( !ovlRead(ovlNameP, &ovl, err) )
    {
        fprintf(stderr, "fastload: %s. Nothing was written.\n", err);
        return(false);
    }

    if( (*addressP = loadTape(tapeP)) == LOADFAILED )
    {
        fprintf(stderr, "Can't load the tape, is it a valid macro or am1 rim tape?\n");
        return(false);
    }

    stampWord = 0;
    haveStamp = tapeImageWord((uint32_t)ovl.tableAddr, &stampWord);
    return( ovlStampMatches(&ovl, tapeP, haveStamp, stampWord) );
}

// Write the overlays to the drum file and warn of any other tool's data they replaced.
// Returns fals, having said why and what was written if a write fails.
static bool
writeOverlays(const char *drumNameP)
{
    if( !ovlWriteDrum(drumNameP, &ovl, drumFileWords, false) )
    {
        return(false);
    }

    ovlWarnOverwritten(&ovl, drum);
    return(true);
}

// Empty the memory file and write the parsed tape to it, the last step of a -m load.
// Returns false, having said why and what was left, if it could not be written.
static bool
writeMemFile(const char *nameP, bool drumWritten)
{
FILE *fP;
bool written;

    if( !(fP = fopen(nameP, "w")) )
    {
        fprintf(stderr, "Can't open memory file '%s', be sure the directory and file have write permission.\n",
            nameP);
        printf("%s\n", drumWritten ? "The drum was written; the memory file was not." :
            "The memory file was not written.");
        return(false);
    }

    written = tapeWriteMemFile(fP);
    if( (fclose(fP) != 0) || !written )
    {
        fprintf(stderr, "Can't write memory file '%s'.\n", nameP);
        printf("%s\n", drumWritten ? "The drum was written; the memory file is incomplete." :
            "The memory file is incomplete.");
        return(false);
    }

    return(true);
}

void
usage(void)
{
    fprintf(stderr, "Usage: fastload [-h host[:port]] [-y] [-m] [-f memfilename] [-i drumfile] rimfile\n");
    fprintf(stderr, "    By default, this will load directly into active memory, stopping the pdp-1\n");
    fprintf(stderr, "    if it is running. The pidp-1 must be running with its debugger port enabled,\n");
    fprintf(stderr, "    ad1port in pidp1.config, on by default.\n");
    fprintf(stderr, "    -h names a pidp-1 on another machine, default localhost:1044.\n");
    fprintf(stderr, "        A host other than localhost with no port gets 1045.\n");
    fprintf(stderr, "    -y is auto-start.\n");
    fprintf(stderr, "        If active memory has been loaded, the program is automatically started.\n");
    fprintf(stderr, "    -m updates the coremem file, the pidp1 and  program must be manually started.\n");
    fprintf(stderr, "    -f specifies the coremem file, the default is /opt/pidp1-mods/coremem.\n");
    fprintf(stderr, "        Don't use -m if the pidp-1 is running, the memory file will be overwritten by it.\n");
    fprintf(stderr, "    A tape with an .ovl, rimfile's name with .ovl, has drum overlays,\n");
    fprintf(stderr, "        If they match,they are  written to the drum file first.\n");
    fprintf(stderr, "    -i specifies the drum file, the default is /opt/pidp1-mods/pdp23drum.\n");
    fprintf(stderr, "        With -h naming another machine the drum cannot be written, run ovlload there.\n");
    exit(1);
}
