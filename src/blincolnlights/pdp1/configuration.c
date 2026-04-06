// This supports a configuration file for setting various parameters for pdp1, IOTs, and other
// components.
// The config file has a simple format.
// Lines starting with '#' are comments and are ignored.
// Empty lines are ignored.
// Otherwise, a line of the form 'xxx=yyy' is expected.
// Embedded spaces are ignored.
// The meaning of 'yyy' depends upon the option.
// For an option that is on or off, 'y', 'yes', or 'on' means enable, anything else means disable.
//
// If an option that is not built in is seen, it is added to the list of extra options.
// If it is a boolean setting, the onOff field in the extra option is set.
// If it is a string of digits 0-9, the ivalue field is set and the fvalue field will be NAN.
// If it is a string of digits 0-9., the fvalue field is set and the ivalue field is set to the integer part.
// Otherwise, the setting is kept as a string and the strvalueP field set to it.
// For any value type not seen, onOff is false, strvalueP is null, fvalue is NAN, ivalue is zero.
// Use isnan() from math.h to test fvalue.

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <string.h>

#define IN_CONFIGURATION_C
#include "configuration.h"

//#define DOLOGGING
#include "logger.h"
// Set desired log type to 1 to enable output assuming logging is defined.
#define LOG_CONFIG 0

// Once loaded, this is globally available.
// Some values are preinitialized.
static Configuration configSettings = {
    .penAperture = 22,      // default aperture in pixels, is 0.200 for the original Type 30
    // all the audio values have defaults
    .sampleRate = 11428,    // samples/second for SDL
    .alpha1 = 0.687,
    .alpha2 = 0.625,
    .alpha3 = 0.524,
    .alpha4 = 0.524,
    .gain = 1.5,
    .tuning = 1.0,
    .muldivEnabled = true,
    .core1DEnabled = true
};

static bool isLoaded;

ConfigurationP
loadConfigFile(char *filenameP)
{
int i;
bool sawOnOff, onOff;
ConfigurationSettingP settingP;
FILE *fP;
char line[256];
char option[64];
char answer[64];

    if( isLoaded )
    {
        return(&configSettings);        // already done
    }

    isLoaded = true;

    if( !(fP = fopen(filenameP, "r")) )
    {
        return(&configSettings);        // use the defaults
    }

    while( fgets(line, sizeof(line), fP) )
    {
        if( (line[0] == '#') || (line[0] == '\n') )
        {
            continue;
        }

        logger(LOG_CONFIG, "%s", line);
        if( (i = sscanf(line, "%[a-zA-Z0-9] = %s", option, answer)) != 2 )
        {
            logger(LOG_CONFIG, "invalid\n");
            fprintf(stderr, "Invalid config file line %d, %s", i, line);
            continue;
        }

        sawOnOff = onOff = false;
        if( !strcmp(answer,"y") || !strcmp(answer,"yes") || !strcmp(answer,"on") || !strcmp(answer,"true") )
        {
            sawOnOff = true;
            onOff = true;
        }
        else if( !strcmp(answer,"n") || !strcmp(answer,"no") || !strcmp(answer,"off") || !strcmp(answer,"false") )
        {
            sawOnOff = true;
        }

        if( !strcmp(option,"audio") )
        {
            configSettings.audioEnabled = onOff;
        }
        else if( !strcmp(option,"samplerate") )
        {
            configSettings.sampleRate = atoi(answer);
        }
        else if( !strcmp(option,"alpha") )
        {
            // Set all alphas, can be overridden if specific ones come later
            configSettings.alpha = atof(answer);
            configSettings.alpha1 = configSettings.alpha;
            configSettings.alpha2 = configSettings.alpha;
            configSettings.alpha3 = configSettings.alpha;
            configSettings.alpha4 = configSettings.alpha;
        }
        else if( !strcmp(option,"alpha1") )
        {
            configSettings.alpha1 = atof(answer);
        }
        else if( !strcmp(option,"alpha2") )
        {
            configSettings.alpha2 = atof(answer);
        }
        else if( !strcmp(option,"alpha3") )
        {
            configSettings.alpha3 = atof(answer);
        }
        else if( !strcmp(option,"alpha4") )
        {
            configSettings.alpha4 = atof(answer);
        }
        else if( !strcmp(option, "gain") )
        {
            configSettings.gain = atof(answer);
        }
        else if( !strcmp(option, "tuning") )
        {
            configSettings.tuning = atof(answer);
        }
        else if( !strcmp(option,"lightpen") )
        {
            configSettings.lightpenEnabled = onOff;
        }
        else if( !strcmp(option,"aperture") )
        {
            configSettings.penAperture = atoi(answer);
        }
        else if( !strcmp(option,"dpyshift") )
        {
            configSettings.dpyShiftEnabled = onOff;
        }
        else if( !strcmp(option,"sdb") )
        {
            configSettings.sdbEnabled = onOff;
        }
        else if( !strcmp(option,"sbs16") )
        {
            configSettings.sbs16Enabled = onOff;
        }
        else if( !strcmp(option,"lailia") )
        {
            configSettings.lailiaEnabled = onOff;
        }
        else if( !strcmp(option,"core1D") )
        {
            configSettings.core1DEnabled = onOff;
        }
        else if( !strcmp(option,"all1D") )
        {
            configSettings.all1DEnabled = onOff;
        }
        else if( !strcmp(option,"muldiv") )
        {
            configSettings.muldivEnabled = onOff;
        }
        else if( !strcmp(option,"newmemfile") )
        {
            configSettings.newMemFile = onOff;
        }
        else if(!strcmp(option, "shared"))
        {
            // Put the PDP1 struct in shared memory for use with other tools
            configSettings.useShm = true;
        }
        else
        {
            // unknown, put into other settings list after
            // trying to deduce the type.
            settingP = (ConfigurationSettingP)calloc(1, sizeof(ConfigurationSetting));
            settingP->nameP = (char *)malloc(strlen(option) + 1);
            strcpy(settingP->nameP, option);
            settingP->fvalue = NAN;

            // The user can decide which to use
            settingP->onOff = onOff;
            if( strspn(answer, "-0123456789") == strlen(answer) )
            {
                settingP->ivalue = atoi(answer);
            }
            else if( strspn(answer, "-0123456789.") == strlen(answer) )
            {
                settingP->fvalue = atof(answer);
                settingP->ivalue = atoi(answer);    // we set this anyway to the int part
            }
            else if( !sawOnOff ) 
            {
                settingP->strvalueP = (char *)malloc(strlen(answer) + 1);
                strcpy(settingP->strvalueP, answer);
            }

            settingP->nextP = configSettings.settingsP;
            configSettings.settingsP = settingP;
        }
    }

    // Special cases
    if( configSettings.all1DEnabled )
    {
        configSettings.core1DEnabled = true;
    }

    if( configSettings.core1DEnabled )
    {
        configSettings.lailiaEnabled = true;
    }

    logger(LOG_CONFIG, "lightpen %d\n", configSettings.lightpenEnabled);
    logger(LOG_CONFIG, "sdb %d\n", configSettings.sdbEnabled);
    logger(LOG_CONFIG, "dpy shift %d\n", configSettings.dpyShiftEnabled);
    logger(LOG_CONFIG, "audio %d\n", configSettings.audioEnabled);
    logger(LOG_CONFIG, "lailia %d\n", configSettings.lailiaEnabled);
    logger(LOG_CONFIG, "core 1D %d\n", configSettings.core1DEnabled);
    logger(LOG_CONFIG, "all 1D %d\n", configSettings.all1DEnabled);
    logger(LOG_CONFIG, "shm %d\n", configSettings.useShm);
    logger(LOG_CONFIG, "new mem file %d\n", configSettings.newMemFile);

    fclose(fP);

    return(&configSettings);
}

// Look up an extra config setting by name.
// If found, return the setting, else null.
ConfigurationSettingP
findConfigurationSetting(ConfigurationP configP, char *nameP)
{
ConfigurationSettingP settingP;

    if( !configP || !(settingP = configP->settingsP) )
    {
        return(NULL);
    }

    while( settingP )
    {
        if( !strcmp(settingP->nameP, nameP) )
        {
            return(settingP);
        }

        settingP = settingP->nextP;
    }

    return(settingP);
}
