/*
 * This is a simple program that copies the adventure save slot and wizcom
 * configuration block to a file so if the drum track is overwritten, your
 * game setup won't be lost. It can then be rewritten to the drum after
 * reloading adventure.
 *
 * Usage advsave [-i path-to-live-drum-image] [-r] savefile
 *
 * If no -i path is given, the default is '/opt/pidp1-mods/pdp23drum'.
 *
 * The first (int size) word in the save file is a magic number for
 * validation, the rest is a 1024 word snapshot of the beginning of the
 * selected track. Both the save slot and the configuration data are there.
 *
 * Using -r restores from a saved copy.
 *
 * 31-Aug-2026 wje initial version
 *
*/

#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#define ADV_DEFINES_ONLY
#include "advdataloader.h"
// The DRUM's save marker comes from the file adventure.am1 includes too
// (as SAVE_MAGIC), so this tool cannot drift from the assembler.
#include "advmagic.h"
// ADVSAVE_FILE_MAGIC is this tool's OWN marker for its .sav container
// file, unrelated to the drum's.
#define ADVSAVE_FILE_MAGIC ((int)(('X' << 24) | ('Y' << 16) | ('Z' << 8) | 'Z'))
#define DRUM_SAVE_MAGIC SAVE_MAGIC

bool loadDrum(int dataFd, int drumFd, int track, char *filenameP, char *drumFilenameP);
bool saveDrum(int dataFd, int drumFd, int track, char *filenameP, char *drumFilenameP);
void usage(void);

// Parses the options, opens the save file and the drum image, and runs
// either saveDrum or loadDrum.
// Exits 0 if the transfer succeeded, 1 on any error.
int
main(int argc, char **argv)
{
int opt;
int dataFd, drumFd;
int track;
bool reload;
bool stat;
char *filenameP;
char *imagenameP;

    imagenameP = DEFAULT_DRUM;
    track = SAVE_TRACK;
    reload = false;

    while( (opt = getopt(argc, argv, "i:t:r")) != -1 )
    {
        switch( opt )
        {
        case 'r':
            reload = true;
            break;

        case 't':
            track = atoi(optarg);
            break;

        case 'i':
            imagenameP = optarg;
            break;

        default:
            usage();
        }
    }

    if( optind >= argc )
    {
        usage();
    }

    filenameP = argv[optind];

    if( (track >= NUM_TRACKS) || (track < 0) )
    {
        fprintf(stderr, "The track number must be 0-31.\n");
        exit(1);
    }

    if( reload )
    {
         dataFd = open(filenameP, O_RDONLY, 0);
    }
    else
    {
        dataFd = open(filenameP, O_WRONLY + O_CREAT + O_TRUNC, 0666);
    }

    if( dataFd < 0 )
    {
        fprintf(stderr,"Can't %s save file '%s'.\n", (reload)?"open":"create", filenameP);
        exit(1);
    }

    if( (drumFd = open(imagenameP, O_RDWR, 0)) < 0 )
    {
        fprintf(stderr,"Can't open drum file '%s'.\n", imagenameP);
        close(dataFd);
        exit(1);
    }

    if( reload )
    {
        stat = loadDrum(dataFd, drumFd, track, filenameP, imagenameP);
    }
    else
    {
        stat = saveDrum(dataFd, drumFd, track, filenameP, imagenameP);
    }

    if( stat )
    {
        printf("Adventure game and wizcom data %s.\n", (reload)?"restored":"saved");
    }

    close(drumFd);
    close(dataFd);
    exit(stat?0:1);
}

// Restores a previously saved snapshot: validates the save file's own
// magic word, reads DRUM_START_WORDS from it, and writes them over the
// start of the given drum track.
// Returns true on success, false if the file is not an advsave file, is
// short, or the drum write failed. Diagnostics go to stderr.
bool
loadDrum(int dataFd, int drumFd,int track, char *filenameP, char *drumFilenameP)
{
int i;
int buffer[DRUM_START_WORDS];

    if( (read(dataFd, &i, sizeof(int)) != sizeof(int)) || (i != ADVSAVE_FILE_MAGIC) )
    {
        fprintf(stderr,"Data file '%s' is not an adventure save file.\n", filenameP);
        return(false);
    }

    if( (read(dataFd, buffer, sizeof(int) * DRUM_START_WORDS) != (sizeof(int) * DRUM_START_WORDS)) )
    {
        fprintf(stderr,"Data file '%s' is not an adventure save file.\n", filenameP);
        return(false);
    }

    lseek(drumFd, track * WORDS_PER_TRACK * sizeof(int), SEEK_SET);
    if( write(drumFd, buffer, sizeof(int) * DRUM_START_WORDS) != (sizeof(int) * DRUM_START_WORDS) )
    {
        fprintf(stderr,"Error writing drum file '%s'.\n", drumFilenameP);
        return(false);
    }

    return(true);
}

// Captures a snapshot: reads DRUM_START_WORDS from the start of the given
// drum track, checks word 0 against the drum's SAVE_MAGIC so an
// uninitialised image is not saved, then writes this tool's own magic word
// followed by the snapshot to the save file.
// Returns true on success, false if the drum read failed, the track holds
// no loaded game, or a write failed. Diagnostics go to stderr.
bool
saveDrum(int dataFd, int drumFd, int track, char *filenameP, char *drumFilenameP)
{
int i;
int buffer[DRUM_START_WORDS];

    lseek(drumFd, track * WORDS_PER_TRACK * sizeof(int), SEEK_SET);
    if( read(drumFd, buffer, sizeof(int) * DRUM_START_WORDS) != (sizeof(int) * DRUM_START_WORDS) )
    {
        fprintf(stderr,"Error reading drum file '%s'.\n", drumFilenameP);
        return(false);
    }

    if( buffer[0] != DRUM_SAVE_MAGIC )
    {
        fprintf(stderr,"Drumfile '%s' doesn't have a valid adventure game loaded.\n", drumFilenameP);
        return(false);
    }

    i = ADVSAVE_FILE_MAGIC;
    if( write(dataFd, &i, sizeof(int)) != sizeof(int) )
    {
        fprintf(stderr,"Error writing data  file '%s'.\n", filenameP);
        return(false);
    }

    if( write(dataFd, buffer, sizeof(int) * DRUM_START_WORDS) != (sizeof(int) * DRUM_START_WORDS) )
    {
        fprintf(stderr,"Error writing data  file '%s'.\n", filenameP);
        return(false);
    }

    return(true);
}

// Prints the usage summary to stderr and exits 1. Does not return.
void
usage(void)
{
    fprintf(stderr, "Usage: advsave [-r] [-t track] [-i drumfile] savefile\n");
    fprintf(stderr, "  -r, reload a saved game else save one\n");
    fprintf(stderr, "  -t track, the track where adventure is loaded, default %d\n", SAVE_TRACK);
    fprintf(stderr, "  -i drumfile, the Type 23 drum image file, default %s\n", DEFAULT_DRUM);
    exit(1);
}
