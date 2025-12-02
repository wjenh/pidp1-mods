
/*
 * This is a copy of the ascii<->flexo conversion routines used in IOT_22, DCS2.
 */

// Used in the flexo (actually concise) conversions, these are the flex lower/upper shift characters
#define CUNSHIFT   072
#define CSHIFT     074

// No equivalent flexo for ascii char
#define FLEX_NCHAR 013

#define NONE -1
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

// Bit 0100 means unshifted
// Bit 0200 means shifted
// Neither means no case
static int ascii2concise[] = {
	NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, // 00
	0075, 0036, NONE, NONE, NONE, 0077, NONE, NONE,
	NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE,
	NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE,

	0000, 0205, 0201, 0204, NONE, NONE, 0206, 0202, // 40
	0157, 0155, 0273, 0254, 0133, 0154, 0173, 0121,
	0120, 0101, 0102, 0103, 0104, 0105, 0106, 0107,
	0110, 0111, NONE, NONE, 0207, 0233, 0210, 0221,

	0140, 0261, 0262, 0263, 0264, 0265, 0266, 0267, // 100
	0270, 0271, 0241, 0242, 0243, 0244, 0245, 0246,
	0247, 0250, 0251, 0222, 0223, 0224, 0225, 0226,
	0227, 0230, 0231, 0257, 0220, 0255, 0211, 0240,

	0156, 0161, 0162, 0163, 0164, 0165, 0166, 0167, // 140
	0170, 0171, 0141, 0142, 0143, 0144, 0145, 0146,
	0147, 0150, 0151, 0122, 0123, 0124, 0125, 0126,
	0127, 0130, 0131, NONE, 0256, NONE, 0203, NONE
};


// Returns -1, NONE, if the character should be ignored, else the ascii char.
// The contents of shiftP indicate if the flexo character is shifted, 1 for yes, 0 for no.
// If a shift character is passed, NONE will be returned and the contents of shiftP updated.
// Typically, the contents of shiftP are set to 0 for the initial call.
// Subsquent calls will update it so strings of flexo characters can be processed.
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

// Returns NONE (-1) if the character should be ignored, there is no flexo equivalent.
// If a shift is seen and we aren't in the correct shift state, become so and return the shift char.
// The caller should then repeat the call with the original character to get the properly shifted one.
// The contents of shiftP should be initialized to 0 before the first call.
// It can be left unchanged across calls if multiple characters are being converted.
char
asciiToFlexo(char ac, int *shiftP)
{
int fc;
    
    fc = ascii2concise[ac];
    if( fc == NONE )
    {
        return(NONE);
    }

    if( (fc & 0200) && !*shiftP )      // we have a shifted char, but we're not shifted
    {
        *shiftP = 1;
        return( CSHIFT );
    }

    if( (fc & 0100) && *shiftP )      // we have an unshifted char, but we're shifted
    {
        *shiftP = 0;
        return( CUNSHIFT );
    }

    return( fc & 077 );
}
