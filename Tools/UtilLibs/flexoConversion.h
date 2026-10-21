// ASCII to and from Flexo (concise), shared by am1 and ad1; see flexoConversion.c.

#ifndef FLEXOCONVERSION_H
#define FLEXOCONVERSION_H

// What the conversions return besides a character or a code.  am1.h's NONE, CUNSHIFT and
// CSHIFT, and ad1.h's NOCHAR, LCS and UCS, are these same values.
#define FLEXO_NONE      -1
#define FLEXO_LCS       -2
#define FLEXO_UCS       -3
#define FLEXO_CUNSHIFT  072
#define FLEXO_CSHIFT    074

int flexoToAscii(int fc, int *shiftP);
int flexoToAsciiShift(int fc, int *shiftP);
int asciiToFlexo(int ac, int *shiftP);

#endif
