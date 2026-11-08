// Check the /opt/pidp1-mods/pidp1.config file for a setting
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

bool getConfig(char *nameP, char *resultP);

static FILE *confFP;

// Check for a boolean setting in the config file.
bool
checkConfig(char *settingP)
{
int i;
bool onOff;
char answer[64];

    if( !getConfig(settingP, answer) )
    {
        return(false);
    }

    onOff = !strcmp(answer,"y") || !strcmp(answer,"yes") || !strcmp(answer,"on");
    return( onOff );
}

// Find a config setting by name, if found, set it in resultP if not null and return true,
// else false if not found.
bool
getConfig(char *nameP, char *resultP)
{
int i;
char line[256];
char option[64];
char answer[64];

    if( !confFP )
    {
        // Not open yet, do so
        if( !(confFP = fopen("/opt/pidp1-mods/pidp1.config", "r")) )
        {
            return(0);
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

        if( (i = sscanf(line, "%[a-zA-Z0-9] = %[a-zA-Z0-9.]", option, answer)) != 2 )
        {
            continue;
        }

        if( strcmp(option, nameP) )
        {
            continue;           // not us
        }

        if( resultP )
        {
            strcpy(resultP, answer);
        }
        return(true);
    }

    return( false );
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
