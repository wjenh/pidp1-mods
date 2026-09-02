/*
 * This is a simple program that loads the drum-image data produced by advdataloader into the drum
 * It uses the initial track information from that data, then writes full 4K tracks for as many
 * as are needed.
 *
 * Usage advdataloader [-i path-to-live-drum-image] datafile
 *
 * IF no -i path is given, the default is '/opt/pidp1-mods/pdp23drum'.
 *
 * The first (int size) word in the datafile is the initial track, the rest is track images.
 * If the last is not a full track, a full track of data is still written.
 *
 * 29-Aug-2026 wje initial version
 * 30-Aug-2026 wje every run now also explicitly reinitializes the SAVE/WIZCOM reserved blocks
 * 2-Sep-2026 wje but don't reinitialize if the correct magic number is there
 *
*/

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#define ADV_DEFINES_ONLY
#include "advdataloader.h"

// This must stay in sync with the value in adventure.am1!
#define SAVE_MAGIC 31344
#define MAX_TRACK (NUM_TRACKS -1)       // highest valid track number

void usage(void);

int
main(int argc, char **argv)
{
int opt;
int inFd, outFd;
int track, count;
char *filenameP;
char *imageNameP;

int buffer[WORDS_PER_TRACK];     // one full drum track
int saveArea[DRUM_START_WORDS];

    imageNameP = DEFAULT_DRUM;

    while( (opt = getopt(argc, argv, "i:")) != -1 )
    {
        switch( opt )
        {
        case 'i':
            imageNameP = optarg;
            break;

        default:
            usage();
        }
    }

    if( optind >= argc )
    {
        usage();
    }

    outFd = -1;
    filenameP = argv[optind];

    if( (inFd = open(filenameP, O_RDONLY)) < 0 )
    {
        fprintf(stderr,"Can't open input data file '%s'.\n", filenameP);
        exit(1);
    }

    if( (outFd = open(imageNameP, O_RDWR|O_CREAT, 0666)) < 0 )
    {
        fprintf(stderr,"Can't open drum file '%s'.\n", imageNameP);
        close(inFd);
        exit(1);
    }

    if( lseek(outFd, SAVE_TRACK * WORDS_PER_TRACK * sizeof(int), SEEK_SET) < 0 )
    {
        fprintf(stderr, "Can't seek to SAVE/WIZCOM block (track %d) in drum file %s: ", SAVE_TRACK, imageNameP);
        perror(NULL);
        exit(1);
    }

    // If we get 0 bytes back, the drum file was never initialzed, not an error, just clear the save area.
    if( ((count = read(outFd, saveArea, sizeof(saveArea))) != sizeof(saveArea)) || (saveArea[0] != SAVE_MAGIC) )
    {
        if( count < 0 )
        {
            fprintf(stderr, "Can't read drum file '%s'\n", imageNameP);
            perror(NULL);
            exit(1);
        }

        memset(saveArea, 0, sizeof(saveArea));       // nothing there, clear the save area
        printf("No valid save was found, initializing the save and wizcom area.\n");
    }
    else
    {
        printf("A valid save was found, preserving it.\n");
    }

    if( (read(inFd, &track, sizeof(int)) != sizeof(int)) || (track < 0) || (track > MAX_TRACK) )
    {
        fprintf(stderr,"Data file '%s' is not valid.\n", filenameP);
        exit(1);
    }

    lseek(outFd, track * WORDS_PER_TRACK * sizeof(int), SEEK_SET);
    while( (count = read(inFd, buffer, WORDS_PER_TRACK * sizeof(int))) > 0 )
    {
        write(outFd, buffer, count);
    }

    // Now rewrite the save area
    if( lseek(outFd, SAVE_TRACK * WORDS_PER_TRACK * sizeof(int), SEEK_SET) < 0 )
    {
        fprintf(stderr, "Can't seek to SAVE/WIZCOM block (track %d): ", SAVE_TRACK);
        perror(NULL);
        exit(1);
    }

    if( write(outFd, saveArea, sizeof(saveArea)) != sizeof(saveArea) )
    {
        fprintf(stderr, "Can't initialize SAVE/WIZCOM block (track %d): ", SAVE_TRACK);
        perror(NULL);
        exit(1);
    }

    close(inFd);
    close(outFd);

    printf("Adventure data has been loaded to the drum.\n");
    exit(0);
}

void
usage(void)
{
    fprintf(stderr, "Usage: advdrumloader [-i drum-file] datafile\n");
    fprintf(stderr, "    If no drum file is given, it defaults to '%s'\n", DEFAULT_DRUM);
}
