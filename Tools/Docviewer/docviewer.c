/* docviewer - convert a PDP-1 tape image that is Flexo text to ascii
 *
 * Usage: docviewer tapefile
 *
*/

#include <stdio.h>
#include <stdlib.h>

#define NONE -1     // used for flex->ascii conversion

int flexoToAscii(char fc, int *shiftP);

int
main(int argc, char **argv)
{
FILE *fP;
int inch, outch;
int shift = 0;
int sawNl = 0;

    if( argc != 2 )
    {
        fprintf(stderr, "Usage: docviewer tapefile\n");
        exit(1);
    }

    if( !(fP = fopen(argv[1], "r")) )
    {
        fprintf(stderr, "Can't open file '%s'\n", argv[1]);
        exit(1);
    }

    while( fread(&inch, 1, 1, fP) )
    {
        if( inch == 0 )
        {
            continue;       // tape feed, ignore
        }

        if( inch & 0100 )
        {
            continue;       // not sure what this was supposed to mean, seems like it should be ignored
        }

        outch = flexoToAscii(inch, &shift);
        if( outch == NONE )
        {
            continue;
        }

        if( outch == '\n' )
        {
            if( sawNl > 1 )
            {
                continue;       // drop blank lines
            }

            sawNl++;
        }
        else
        {
            sawNl = 0;
        }

        fputc(outch, stdout);
    }

    fclose(fP);
    exit(0);
}

// Concise/flex to ascii conversion stuff
#define LCS -2
#define UCS -3

#define Red NONE
#define Blk NONE
#define LF NONE

// SHIFT | concise to get uppercase
#define SHIFT 0100

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
// A shift character will update the state of shiftP and return NONE.
int
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
