/*
 * This is a simple program that copies the adventure save slot and wizcom configuration block
 * to a file so if the drum track is overwritten, your game setup won't be lost.
 * It can then be rewritten to the drum after reloading adventure.
 *
 * Usage advsave [-i path-to-live-drum-image] [-r] savefile
 *
 * IF no -i path is given, the default is '/opt/pidp1-mods/pdp23drum'.
 *
 * The first (int size) word in the datafile is the initial track,
 * the rest is just a 1024 word snapshot of the beginning of that track.
 * Both the save slot and the configuration data are there.
 *
 * Using -r restores from a saved copy.
 *
 * The save file iw written with a one int word magic number for validation.
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
#define SAVE_MAGIC ((int)(('X' << 24) | ('Y' << 16) | ('Z' << 8) | 'Z'))
#define DRUM_SAVE_MAGIC 31344   // must match adventure.am1's value

bool loadDrum(int dataFd, int drumFd, int track, char *filenameP, char *drumFilenameP);
bool saveDrum(int dataFd, int drumFd, int track, char *filenameP, char *drumFilenameP);
void usage(void);

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
            ;

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
        return(false);
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

bool
loadDrum(int dataFd, int drumFd,int track, char *filenameP, char *drumFilenameP)
{
int i;
int buffer[DRUM_START_WORDS];

    if( (read(dataFd, &i, sizeof(int)) != sizeof(int)) || (i != SAVE_MAGIC) )
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

    i = SAVE_MAGIC;
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

void
usage(void)
{
    fprintf(stderr, "Usage: advsave [-r] [-t track] [-i drumfile] savefile\n");
    fprintf(stderr, "  -r, reload a saved game else save one\n");
    fprintf(stderr, "  -t track, the track where adventure is loaded, default %d\n", SAVE_TRACK);
    fprintf(stderr, "  -i drumfile, the Type 23 drum image file, default %s\n", DEFAULT_DRUM);
    exit(1);
}
