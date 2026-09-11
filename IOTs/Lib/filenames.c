// This has utility functions for creating Linux filenames from am1 ascii strings.
// Relative paths and a leading ~/ or ~username/ are supported, but wildcarding is not.
// These can be used from within IOTs.
//
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include "pdp1.h"

#define MAXLEN 128 // maximum number of words we will fetch to prevent runaway code from user error

// Given a 16 bit address in address, convert the packed ascii string as created by the am1 ascii
// directive to a C string suitable for use with fopen(), in bufferP, which is bufLen bytes long.
// There are 2 chars per word, the first in the high 9 bits and the second in the low 9 bits; a
// zero char ends the string. At most MAXLEN words are read, so a name is at most 255 chars.
// Returns bufferP, or NULL if the string isn't valid: it runs off the end of memory, has no
// terminator within MAXLEN words, has a ~ form that can't be expanded, or the result (with any
// home directory prefixed) doesn't fit in bufLen bytes including its terminator.
char *
getFileName(PDP1P pdp1P, unsigned int address, char *bufferP, size_t bufLen)
{
int count;
int len;
unsigned int word;
char achar;
char *cP;
char *dirP;
struct passwd *pwdP;
char tmpstr[(MAXLEN * 2) + 1];      // two chars per word, plus room for a terminator

    if( !bufferP || (bufLen == 0) )
    {
        return(NULL);
    }

    // First unpack the file name, stopping at the first zero char.
    // Reaching MAXLEN words without one means the address isn't a string: refuse it.
    cP = NULL;
    for( count = 0; count < MAXLEN; ++count )
    {
        if( address >= MAXMEM )
        {
            return(NULL);
        }

        word = pdp1P->core[address++];
        achar = (word & 0377000) >> 9;
        tmpstr[count * 2] = achar;
        if( !achar )
        {
            cP = &tmpstr[count * 2];
            break;
        }

        achar = word & 0377;
        tmpstr[(count * 2) + 1] = achar;
        if( !achar )
        {
            cP = &tmpstr[(count * 2) + 1];
            break;
        }
    }

    if( !cP )
    {
        return(NULL);        // no terminator within MAXLEN words
    }

    dirP = "";

    if( tmpstr[0] == '~' )      // need to do directory expansion
    {
        if( !(cP = strchr(tmpstr, '/')) )
        {
            return(NULL);        // malformed
        }

        *cP++ = 0;

        if( strlen(tmpstr) == 1 )    // ~/... form, user home
        {
            if( !(dirP = getenv("HOME")) )
            {
                pwdP = getpwuid(getuid());
                if( !pwdP )
                {
                    return(NULL);
                }

                dirP = pwdP->pw_dir;
            }
        }
        else                        // ~uname/... form, uname's home
        {
            pwdP = getpwnam(tmpstr + 1);
            if( !pwdP )
            {
                return(NULL);
            }

            dirP = pwdP->pw_dir;
        }

        len = snprintf(bufferP, bufLen, "%s/%s", dirP, cP);
    }
    else
    {
        len = snprintf(bufferP, bufLen, "%s", tmpstr);
    }

    if( (len < 0) || ((size_t)len >= bufLen) )
    {
        return(NULL);        // doesn't fit in the caller's buffer
    }

    return(bufferP);
}
