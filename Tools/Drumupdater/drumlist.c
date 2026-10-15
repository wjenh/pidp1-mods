/*
 * Examine a drum image, list tracks loaded via drumupdater, Adventure's and a
 * Z-machine story's tracks, and the overlays of any am1 overlay files given.
 *
 * Usage: drumlist [-i imagefile] [file.ovl ...]
 * where:
 * i - use the given name for the drum image instead of '/opt/pidp1-mods/pdp23drum'
 * file.ovl - name each overlay of the file on its track, and say whether the drum
 *            holds it, as ovlload -c would
 *
 * The .ovl reader and the recognizers of other tools' data are ovlload's,
 * ../Ovlload/ovlfile.c, compiled in.
 *
 * Original author: Bill Ezell (wje), pdp1@quackers.net
 *
 * Revision history:
 *
 * 02/01/2026 wje - Initial version
 * 14/01/2026 wje - fix usage check
 * 22/09/2026 wje - add ppporch's adventure check
 * 13/10/2026 Claude - Z-machine stories, and the overlays of .ovl files
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>

#include "ovlfile.h"

#define DEFAULT_IMAGE "/opt/pidp1-mods/pdp23drum"
#define WORDS_PER_TRACK 4096
#define ADVENTURE_LAST_TRACK 22

typedef int Word;        // we put the 18 bit PDP-1 words into a 32 bit integer
Word buffer[128];

static int32_t drum[OVL_DRUMWORDS];     // the whole drum, zero past the file's end

char *formatStr(Word *bufP, char *strP);
static void listOverlays(int trackNo, OvlFile *filesP, char **namesP, int fileCount, bool *listedP);
void usage(void);

int
main(int argc, char **argv)
{
int opt;
int fd;
int i;
int j;
int fileCount;
int zTracks;
int lastOverlayTrack;
bool adventurePresent = false;
bool claimed;
bool listed;
int trackNo = 0;
ssize_t got = 0;
size_t have;
off_t drumBytes;
struct stat st;
char *imageName;
char label[2 * 128 + 1];
char serial[ZH_SERIAL_BYTES + 1];
char err[OVL_ERRSIZE];
OvlFile *filesP;

    imageName = DEFAULT_IMAGE;

    // parse our comd line args
    while( (opt = getopt(argc, argv, "i:")) != -1 )
    {
        if( opt != 'i' )
        {
            usage();
        }

        imageName = optarg;
    }

    if( (fd = open(imageName, O_RDONLY)) < 0 )
    {
        fprintf(stderr,"Can't open drum image file '%s'\n", imageName);
        exit(1);
    }

    // The whole drum, so a story's length and an overlay's words can be read anywhere.
    have = 0;
    while( (have < sizeof(drum)) && ((got = read(fd, (char *)drum + have, sizeof(drum) - have)) > 0) )
    {
        have += (size_t)got;
    }

    if( (fstat(fd, &st) < 0) || (got < 0) )
    {
        fprintf(stderr,"Can't read drum image file '%s'\n", imageName);
        exit(1);
    }

    drumBytes = st.st_size;
    close(fd);

    // Each .ovl is read whole; one that fails its checks is reported and left out.
    fileCount = argc - optind;
    if( !(filesP = calloc((fileCount > 0) ? fileCount : 1, sizeof(OvlFile))) )
    {
        fprintf(stderr,"drumlist: out of memory\n");
        exit(1);
    }

    lastOverlayTrack = -1;
    for( i = 0; i < fileCount; ++i )
    {
        if( !ovlRead(argv[optind + i], &filesP[i], err) )
        {
            fprintf(stderr,"drumlist: %s; its overlays are not listed.\n", err);
            continue;
        }

        for( j = 0; j < filesP[i].count; ++j )
        {
            if( filesP[i].overlaysP[j].track > lastOverlayTrack )
            {
                lastOverlayTrack = filesP[i].overlaysP[j].track;
            }
        }
    }

    // Adventure's SAVE block begins at word 0 of track 16.  Use the
    // same validity magic as AdvDataLoader/advdrumloader.c.
    adventurePresent = adventureSaved(drum);

    // A Z-machine story zloader installed from track 0, recognized by its header.
    zTracks = storyTracks(drum);

    // The start address is in location 07773.
    // Location 07774 will be 1 to indicate the presence of a label.
    // An 'initialized' marker of 0707070 is at locations O7775, 07776, and 07777
    // buffer[022] = start_addr;
    // buffer[023] = 1 or 0
    // buffer[024] = 0707070;
    // buffer[025] = 0707070;
    // buffer[026] = 0707070;

    // Locations 07751-07771 will have the label, up to 34 characters.
    // Characters are packed 2 per word, first character in the high 9 bits, second in the low 9 bits, etc.
    // A character of 0, a null byte, marks the end of the label.

    for( trackNo = 0; trackNo < OVL_TRACKS; ++trackNo )
    {
        claimed = false;

        if( trackNo < zTracks )
        {
            if( trackNo == 0 )
            {
                for( i = 0; i < ZH_SERIAL_BYTES; ++i )
                {
                    serial[i] = (char)storyByte(drum, ZH_SERIAL + i);
                }

                serial[ZH_SERIAL_BYTES] = 0;
                printf("Track %d: Z-machine story, version %d, serial %s\n", trackNo, storyByte(drum, ZH_VERSION),
                    serial);
            }
            else
            {
                printf("Track %d: Z-machine story\n", trackNo);
            }

            claimed = true;
        }
        // Check Adventure before looking at 07751.  Its final room-data
        // track may be partial and need not contain that far-offset word.
        else if( adventurePresent &&
            (trackNo >= ADVENTURE_SAVE_TRACK) &&
            (trackNo <= ADVENTURE_LAST_TRACK) )
        {
            if( trackNo == ADVENTURE_SAVE_TRACK )
                printf("Track %d: Adventure text/data (SAVE/WIZCOM)\n", trackNo);
            else if( trackNo <= 19 )
                printf("Track %d: Adventure text/data\n", trackNo);
            else
                printf("Track %d: Adventure room data\n", trackNo);

            claimed = true;
        }
        else if( drumBytes <= (off_t)((07751 + (trackNo * WORDS_PER_TRACK)) * sizeof(Word)) )
        {
            // Past the file's end; an overlay named on this track or a later one is
            // still listed, since ovlload extends the file only as far as it writes.
            if( trackNo > lastOverlayTrack )
            {
                printf("End of initialized tracks.\n");
                break;
            }
        }
        else
        {
            memset(buffer, 0, sizeof(buffer));
            memcpy(buffer, &drum[07751 + (trackNo * WORDS_PER_TRACK)], 027 * sizeof(Word));   // 7751-7777

            if( drumupdaterMarked(drum, trackNo) )
            {
                printf("Track %d: %s starts at %04o\n", trackNo, buffer[023]?formatStr(buffer, label):"no label",
                    buffer[022]);
                claimed = true;
            }
        }

        listOverlays(trackNo, filesP, &argv[optind], fileCount, &listed);
        if( !claimed && !listed )
        {
            printf("Track %d is not initialized.\n", trackNo);
        }
    }

    exit(0);
}

// Print a line for each overlay of the .ovl files on a track: where it is, and what
// ovlload -c would find for it.  *listedP says whether there was one.
static void
listOverlays(int trackNo, OvlFile *filesP, char **namesP, int fileCount, bool *listedP)
{
int f;
int i;
int addr;
int32_t got;
int32_t want;
const char *stateP;
Overlay *ovP;

    *listedP = false;
    for( f = 0; f < fileCount; ++f )
    {
        for( i = 0; i < filesP[f].count; ++i )
        {
            ovP = &filesP[f].overlaysP[i];
            if( ovP->track != trackNo )
            {
                continue;
            }

            switch( ovlCompare(ovP, drum, &addr, &got, &want) )
            {
            case OVL_CURRENT:
                stateP = "current";
                break;

            case OVL_DIFFERS:
                stateP = "differs";
                break;

            default:
                stateP = "stamp differs";
                break;
            }

            printf("Track %d: overlay %s of %s, drum %04o-%04o, window %s in bank %o, %o words, %s, %s\n",
                trackNo, ovP->name, namesP[f], ovP->first, ovP->last, ovP->window, ovP->bank, ovP->length,
                (ovP->persistent) ? "persistent" : "read-only", stateP);
            *listedP = true;
        }
    }
}

char *
formatStr(Word *bufP, char *strP)
{
char ch;
char *origP;
Word word;

    origP = strP;

    for(;;)
    {
        word = *bufP++;
        ch = (word >> 9);
        if( ch == 0 )
        {
            break;
        }

        *strP++ = ch;

        ch = (word & 0xFF);
        if( ch == 0 )
        {
            break;
        }

        *strP++ = ch;
    }

    *strP = 0;
    return( origP );
}

void
usage()
{
    fprintf(stderr,"Usage: drumlist [-i imagefile] [file.ovl ...]\n");
    fprintf(stderr,"where:\n");
    fprintf(stderr,"i - use the given name for the drum image instead of '%s'\n", DEFAULT_IMAGE);
    fprintf(stderr,"file.ovl - name each overlay of an am1 overlay file on its track\n");
    exit(1);
}
