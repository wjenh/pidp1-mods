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
 *
 * 30-Aug-2026 wje owner-directed fix: every run now also explicitly
 * reinitializes the SAVE/WIZCOM reserved block at the front of
 * SAVE_TRACK on the LIVE drum (see resetSaveWizBlock() below). Found
 * while chasing a "restart after QUIT fails, but only if a game was
 * SAVEd" report: advdataloader's datafile never carries real content
 * for that block (its own message/room placement always starts right
 * after it), so the block's previous contents on the live drum -- a
 * real player's save, a stale record from an older layout, or (since
 * this is a general-purpose drum image, not an Adventure-only file)
 * leftover data from a completely different application -- used to
 * survive untouched across a deploy. That is not safe to assume valid,
 * so rather than trying to detect/preserve whatever was there, every
 * deploy now unconditionally stamps it back to a known-good "nothing
 * saved yet" state, which adventure.am1's own existing logic already
 * treats correctly (doRestore's SAVE_MAGIC check reports NO SAVED GAME;
 * wizComLoad's WC_VALID check falls back to wcApplyPoof's real POOF
 * defaults on the very next connection) -- see resetSaveWizBlock().
 *
*/

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#define DEFAULT_DRUM "/opt/pidp1-mods/pdp23drum"

#define TRACK_SIZE  4096    // words in a full drum track
#define MAX_TRACK  31       // highest valid track number

// SAVE_TRACK / DRUM_FRONT_WORDS mirror AdvDataLoader/advdataloader.h's
// SAVE_TRACK and (SAVE_BLOCK_WORDS + WIZCOM_BLOCK_WORDS) exactly -- kept
// as separate #defines here (rather than #including advdataloader.h,
// which drags in the parser's symtab/struct machinery this small,
// standalone tool has no other use for) with the same "must stay in
// sync by hand" convention this project already uses for its other
// cross-file drum-layout constants. If either value changes on the
// advdataloader side, update these two lines to match.
#define SAVE_TRACK        16      // == advdataloader.h SAVE_TRACK
#define DRUM_FRONT_WORDS  1024    // == advdataloader.h SAVE_BLOCK_WORDS(512) + WIZCOM_BLOCK_WORDS(512)

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

int buffer[TRACK_SIZE];     // one full drum track
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

    lseek(outFd, track * TRACK_SIZE * sizeof(int), SEEK_SET);
    while( (count = read(inFd, buffer, TRACK_SIZE * sizeof(int))) > 0 )
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

// resetSaveWizBlock -- unconditionally overwrites the SAVE_TRACK front
// block (SAVE_TRACK, word offset 0, for DRUM_FRONT_WORDS words) on the
// drum image identified by outFd with all-zero words.
//
// Why all-zero is "a valid value" and not just "blank": adventure.am1
// treats this exact block as two fixed-size records, each guarded by its
// own magic/validity word at its own base offset -- SAVE_MAGIC_OFFSET
// (relative offset 0, i.e. this block's first word) must equal
// SAVE_MAGIC (0d31344) for doRestore to trust the rest of the SAVE
// record, and WC_MAGIC_OFFSET (relative offset SAVE_BLOCK_WORDS, i.e.
// this block's word 512) must equal WC_VALID (0d234567) for wizComLoad
// to trust the rest of the WIZCOM record. Zero can never accidentally
// equal either magic number, so a zeroed block reads back, on the very
// next connection, as "no saved game" (doRestore's own NO SAVED GAME
// message) and "no WIZCOM record yet" (wizComLoad's existing wcNoRec
// branch, which calls wcApplyPoof to (re)establish real POOF defaults)
// -- both already-correct, already-tested code paths in adventure.am1.
// This function only needs to land on a value neither check will ever
// mistake for real data; it deliberately does not try to duplicate
// wcApplyPoof's actual POOF field values here, so the two can never
// drift out of sync with each other.
//
// Called unconditionally on every run (not just when the datafile's own
// track range happens to include SAVE_TRACK): this tool always deploys
// the one Adventure drum image, so "just ran advdrumloader" should
// always mean "the save/WIZCOM area is in a known, valid state" --
// never a stale record left over from an earlier layout, and never
// leftover bytes from some other application that happened to have used
// this same drum image.
//
// No return value. Aborts the whole program (via exit()) if either the
// seek or the write fails -- a half-initialized reserved block is worse
// than not deploying at all, since it would look valid enough to read
// but not actually be internally consistent.
void
resetSaveWizBlock(int outFd)
{
int zero[DRUM_FRONT_WORDS];

    memset(zero, 0, sizeof(zero));

    if( lseek(outFd, (off_t)SAVE_TRACK * TRACK_SIZE * sizeof(int), SEEK_SET) < 0 )
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
