#ifndef CONFIGURATION_H
#define CONFIGURATION_H
// This defines configuration settings for the config file.
// Some are known and are set in the Configuration datatype.
// Any unkown setting is saved as a name/value pair and can be searched for.
// If the value part is a boolean setting, the onOff field will be set.
// If it is the digits 0-9, the ivalue field will be set.
// If it is the digits 0-9 and ., the fvalue field will be set and the ivalue field set to the integer part.
// Otherwise, the strvalueP field will be set and will contain the value string.
// For any setting type not seen, a default is set in its field.
// The default for onOff is false.
// The default for ivalue is 0.
// The default for fvalue is NAN, use isnan() from math.h to test.
// The default for svalueP is null.

#include <stdbool.h>

// How unknown settings are kept.
// Each new one is linked into the list of settings.
typedef struct _ConfigurationSetting
{
    char *nameP;
    bool onOff;
    int ivalue;
    float fvalue;
    char *strvalueP;
    struct _ConfigurationSetting *nextP;
} ConfigurationSetting, *ConfigurationSettingP;

// How settings are returned.
// Primary ones are defined here, extas are linked.
typedef struct
{
    int penAperture;
    bool lightpenEnabled;
    bool sdbEnabled;
    bool dpyShiftEnabled;
    bool audioEnabled;
    bool lailiaEnabled;
    bool core1DEnabled;
    bool all1DEnabled;
    bool useShm;
    bool newMemFile;
    bool sbs16Enabled;
    bool muldivEnabled;
    int sampleRate;
    float alpha;
    float alpha1;
    float alpha2;
    float alpha3;
    float alpha4;
    float gain;
    float tuning;
    ConfigurationSettingP settingsP;
} Configuration, *ConfigurationP;

#ifndef IN_CONFIGURATION_C
extern ConfigurationSettingP findConfigurationSetting(ConfigurationP configP, char *nameP);
extern ConfigurationP getConfiguration(void);
extern ConfigurationP loadConfigFile(char *filenameP);
extern ConfigurationP reloadConfigFile(char *filenameP);
#endif

#endif
