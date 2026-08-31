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
 * 30-Aug-2026 wje every run now also explicitlyreinitializes the SAVE/WIZCOM reserved blocks
 *
*/

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#define ADV_DEFINES_ONLY
#include "advdataloader.h"

#define MAX_TRACK  (NUM_TRACKS -1)       // highest valid track number

void usage(void);
void resetSaveWizBlock(int outFd);

int
main(int argc, char **argv)
{
int opt;
int inFd, outFd;
int track, count;
char *filenameP;
char *imageNameP;

int buffer[WORDS_PER_TRACK];     // one full drum track
char imageName[1024];

    imageNameP = DEFAULT_DRUM;

    while( (opt = getopt(argc, argv, "i:")) != -1 )
    {
        switch( opt )
        {
        case 'i':
            strcpy(imageName, optarg);
            imageNameP = imageName;
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

    if( (outFd = open(imageNameP, O_WRONLY|O_CREAT, 0666)) < 0 )
    {
        fprintf(stderr,"Can't open drum file '%s'.\n", imageNameP);
        close(inFd);
        exit(1);
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

    // Always reinitialize the SAVE/WIZCOM reserved block, regardless of
    // which track(s) the datafile itself targeted -- see this file's own
    // header comment ("30-Aug-2026 wje owner-directed fix") for why this
    // must not depend on whatever the datafile happened to contain there.
    resetSaveWizBlock(outFd);

    close(inFd);
    close(outFd);

    printf("Adventure data has been loaded to the drum.\n");
    exit(0);
}

// Unconditionally overwrites the SAVE_TRACK save and wizcom blocks.
//
// No return value.
// Aborts the whole program on failure,  a half-initialized reserved block is worse
// than not deploying at all, since it would look valid enough to read
// but not actually be internally consistent.
void
resetSaveWizBlock(int outFd)
{
int zero[DRUM_START_WORDS];

    memset(zero, 0, sizeof(zero));

    if( lseek(outFd, (off_t)SAVE_TRACK * WORDS_PER_TRACK * sizeof(int), SEEK_SET) < 0 )
    {
        fprintf(stderr, "Can't seek to SAVE/WIZCOM block (track %d): ", SAVE_TRACK);
        perror(NULL);
        exit(1);
    }

    if( write(outFd, zero, sizeof(zero)) != (ssize_t)sizeof(zero) )
    {
        fprintf(stderr, "Can't initialize SAVE/WIZCOM block (track %d): ", SAVE_TRACK);
        perror(NULL);
        exit(1);
    }
}

void
usage(void)
{
    fprintf(stderr, "Usage: advdrumloader [-i drum-file] datafile\n");
    fprintf(stderr, "    If no drum file is given, it defaults to '%s'\n", DEFAULT_DRUM);
}
