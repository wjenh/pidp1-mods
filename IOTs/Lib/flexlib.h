#ifndef FLEXLIB_H
#define FLEXLIB_H

#define NONE -1
#define LCS -2
#define UCS -3
// Used in the flexo (actually concise) conversions, these are the flex lower/upper shift characters
#define CUNSHIFT   072
#define CSHIFT     074

#ifdef IN_FLEXLIB_C
#define SHIFT 0100
#define Red NONE
#define Blk NONE
#define LF NONE
#endif

#endif
