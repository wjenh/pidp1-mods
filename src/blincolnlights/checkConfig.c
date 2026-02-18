// Check the /opt/pidp1-mods/pidp1.config file for a setting
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

// Check for a boolean setting in the config file.
bool
checkConfig(char *settingP)
{
int i;
bool onOff;
FILE *fP;
char line[256];
char option[64];
char answer[64];

    if( !(fP = fopen("/opt/pidp1-mods/pidp1.config", "r")) )
    {
        return(false);
    }

    while( fgets(line, sizeof(line), fP) )
    {
        if( (line[0] == '#') || (line[0] == '\n') )
        {
            continue;
        }

        if( (i = sscanf(line, "%[a-z0-9] = %[a-z0-9.]", option, answer)) != 2 )
        {
            continue;
        }

        if( strcmp(option,settingP) )
        {
            continue;           // not us
        }

        onOff = !strcmp(answer,"y") || !strcmp(answer,"yes") || !strcmp(answer,"on");
        break;
    }

    fclose(fP);
    return( onOff );
}
