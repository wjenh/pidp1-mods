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
*/

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../AD1/ad1link.h"
#include "tapeimage.h"

#define DEFAULT_MEMFILE "/opt/pidp1-mods/coremem"

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
FILE *memFilefP;
Ad1Link link;
bool wasRunning;
bool autoStart;
int status;

    memFileNameP = DEFAULT_MEMFILE;
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

        if( !(memFilefP = fopen(memFileNameP, "w")) )
        {
            fprintf(stderr, "Can't open memory file '%s', be sure the directory and file have write permission.\n",
                memFileNameP);
            exit(1);
        }

        if( (address = loadTape(filenameP, memFilefP, true)) == LOADFAILED )
        {
            fclose(memFilefP);
            fprintf(stderr, "Can't load the tape, is it a valid macro or am1 rim tape?\n");
            exit(1);
        }
        else if( address == LOADSTOP )
        {
            fclose(memFilefP);
            printf("Tape loaded, an am1 program with a stop directive.\n");
            exit(0);
        }

        printf("Tape loaded, start address 0%0o.\n", address);
        fclose(memFilefP);
    }
    else
    {
        // Parse the whole tape before touching the emulator, so a bad tape changes nothing.
        if( (address = loadTape(filenameP, 0, false)) == LOADFAILED )
        {
            fprintf(stderr, "Can't load the tape, is it a valid macro or am1 rim tape?\n");
            exit(1);
        }

        if( ad1LinkOpen(&link, hostP, 0, "fastload") != 0 )
        {
            fprintf(stderr, "%s\n", ad1LinkError());
            exit(1);
        }

        // Stops the pdp-1 if it is running, then stores every block in one step.
        wasRunning = false;
        if( !sendImage(&link, &wasRunning) )
        {
            ad1LinkClose(&link);
            exit(1);
        }

        if( wasRunning )
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

        // The memory-file path above has its own pair; this path never ran it.
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

void
usage(void)
{
    fprintf(stderr, "Usage: fastload [-h host[:port]] [-y] [-m] [-f memfilename] rimfile\n");
    fprintf(stderr, "    By default, this will load directly into active memory, stopping the pdp-1\n");
    fprintf(stderr, "    if it is running. The pidp-1 must be running with its debugger port enabled,\n");
    fprintf(stderr, "    ad1port in pidp1.config, on by default.\n");
    fprintf(stderr, "    -h names a pidp-1 on another machine, default localhost:1044.\n");
    fprintf(stderr, "        A host other than localhost with no port gets 1045.\n");
    fprintf(stderr, "    -y is auto-start.\n");
    fprintf(stderr, "        If active memory has been loaded, the program is automatically started.\n");
    fprintf(stderr, "    -m updates the coremem file, the pidp1 and  program must be manually started.\n");
    fprintf(stderr, "    -f specifies the coremem file, the default is /opt/pidp1-mods/coremem.\n");
    fprintf(stderr, "    Don't use -m if the pidp-1 is running, the memory file will be overwritten by it.\n");
    exit(1);
}
