// This contains a function to convert a flex/concise character to an ascii character.

#include <stdbool.h>

#define NOCHAR -1
#define LCS -2
#define UCS -3
#define NONE 0xFF
#define Red NONE
#define Blk NONE
#define LF NONE
// SHIFT | concise to get uppercase
#define SHIFT 0100

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

// Returns NOCHAR if the character should be ignored, UCS if the char was an upper shift, LCS if lower,
// else the ascii char.
// shiftP is updated with the current shift state, true is UCS, false is LCS.
int
flexToAscii(char fc, bool *shiftP)
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
        return(NOCHAR);
    }

    if( ac == LCS )
    {
        *shiftP = false;
        return(LCS);
    }

    if( ac == UCS )
    {
        *shiftP = true;
        return(UCS);
    }

    return(ac);
}
