// This is a utility to convert flex characters to ascii.
// The companion is asciitoflex.c
// See below for use.
#define IN_FLEXLIB_C
#include "flexlib.h"

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

// Returns NONE if the character should be ignored.
// Shift must be kept, it tracks the current shift state.
// It can be cleared if starting a new string of conversions.
int
flexToAscii(char fc, int *shiftP)
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

