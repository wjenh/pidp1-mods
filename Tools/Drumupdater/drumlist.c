/*
 * Examine a drum image, list tracks loaded via drumupdater.
 *
 * Usage: drumlister [-i imagefile]
 * where:
 * i - use the given name for the drum image instead of 'drumImage'
 *
 * Original author: Bill Ezell (wje), pdp1@quackers.net
 *
 * Revision history:
 *
 * 02/01/2026 wje - Initial version
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>

#define DEFAULT_IMAGE "drumImage"

typedef int Word;        // we put the 18 bit PDP-1 words into a 32 bit integer
Word buffer[128]; 

char *formatStr(Word *bufP, char *strP);
void usage(void);

int
main(int argc, char **argv)
{
int opt;
int fd;
int trackNo = 0;
char *cP, *cP2;
char imageName[512];

    strcpy(imageName, DEFAULT_IMAGE);

    // parse our comd line args
    while( (opt = getopt(argc, argv, "i:")) != -1 )
    {
        switch( opt )
        {
        case 'i':
           strcpy(imageName, optarg);
           break;

        default: /* '?' */
	   usage();
        }
    }

    if( optind > argc )
    {
        usage();                // no other args
    }

    if( (fd = open(imageName, O_RDONLY)) < 0 )
    {
        fprintf(stderr,"Can't open drum image file '%s'\n", imageName);
        exit(1);
    }

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

    for( trackNo = 0; trackNo < 32; ++trackNo )
    {
        lseek(fd, (07751 + (trackNo * 4096)) * sizeof(Word), SEEK_SET);
        if( !read(fd, buffer, 027 * sizeof(Word)) )   // read 7751-7777
        {
            printf("End of initialized tracks.\n");
            break;
        }

        if( (buffer[024] == 0707070) && (buffer[025] == 0707070) && (buffer[026] == 0707070) )
        {
            printf("Track %d: %s starts at %04o\n", trackNo, buffer[023]?formatStr(buffer, imageName):"no label",
                buffer[022]);
        }
        else
        {
            printf("Track %d is not initialized.\n", trackNo);
        }
    }

    close(fd);
    exit(0);
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
    fprintf(stderr,"Usage: drumlist [-i imagefile]\n");
    fprintf(stderr,"where:\n");
    fprintf(stderr,"i - use the given name for the drum image instead of 'drumImage'\n");
    exit(1);
}
