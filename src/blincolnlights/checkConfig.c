// Check the /opt/pidp1-mods/pidp1.config file for a setting
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

static FILE *confFP;

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

    if( !confFP )
    {
        // Not open yet, do so
        if( !(fP = fopen("/opt/pidp1-mods/pidp1.config", "r")) )
        {
            return(false);
        }
    }
    else
    {
        // back to the beginning
        fseek(confFP, 0L, SEEK_SET);
    }

    while( fgets(line, sizeof(line), confFP) )
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

    return( onOff );
}

// Close the config file.
// If needed, it will be reopened.
void
closeConfigFile()
{
    if( confFP )
    {
        fclose(confFP);
        confFP = false;
    }
}
