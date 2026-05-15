// This has utility functions for creating Linux filenames from am1 ascii strings.
// Relative paths and a leading ~/ or ~username/ are supported, but wildcarding is not.
// These can be used from within IOTs.
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include "pdp1.h"

#define MAXLEN 128 // maximum number of words we will fetch to prevent runaway code from user error

// Given a 16 bit address in address, convert the packed ascii string as created by the am1 text directive
// to a C string suitable for use with fopen().
// If the string isn't valid, null is returned, else bufferP.
char *
getFileName(PDP1P pdp1P, unsigned int address, char *bufferP)
{
int count;
unsigned int word;
char achar;
char *cP;
char *dirP;
struct passwd *pwdP;
char tmpstr[256];

    // First unpack the file name.
    // There are 2 chars per word, 1st in high 9 bits, second in low 9 bits.
    // Either being null terminates the string.
    for( cP = tmpstr, count = 0; count++ <= MAXLEN; )
    {
        if( address >= MAXMEM )
        {
            return(NULL);
        }

        word = pdp1P->core[address++];
        achar = (word & 0377000) >> 9;
        if( !(*cP++ = achar) )
        {
            break;
        }

        achar = word & 0377;
        if( !(*cP++ = achar) )
        {
            break;
        }
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

        sprintf(bufferP, "%s/%s", dirP, cP);
    }
    else
    {
        strcpy(bufferP, tmpstr);
    }

    return(bufferP);
}
