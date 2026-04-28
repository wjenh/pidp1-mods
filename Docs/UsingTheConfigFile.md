## Using the pidp-1 configuration file

This document describes the /opt/pidp1-mods/pdp1.config and how to use it.

This is version 1.1; it will be updated as needed.

Edit date 28-Apr-2026

## What is the configuration file?

It is text file that allows runtime configuration of many aspects of the pidp-1 enulator as well
as various IOTS.

The config file has a simple format.\
Lines starting with '#' are comments and are ignored.\
Empty lines are ignored.\
Embedded spaces are ignored.\
Otherwise, a line of the form 'xxx=yyy' is expected.\
The meaning of 'yyy' depends upon the option.\
For an option that is on or off, 'y', 'yes', or 'on' means enable, anything else means disable.\
For a numeric option, it can be an integer or a floating-point number *nn.nn*.\
Anything else is a string option, the string following the *=* is its value.

Most options are built-in but new ones can be added for use in IOTs.\
If an option that is not built in is seen, it is added to a list of extra options that
can be accessed via functions in the .../src/blicolnlights/configuration.c library.
An example can be found in the source code for the IOT_45 line printers.

- If it is a boolean setting, the onOff field in the extra option is set.
- If it is a string of digits 0-9, the ivalue field is set and the fvalue field will be NAN.
- If it is a string of digits 0-9., the fvalue field is set and the ivalue field is set to the integer part.
- Otherwise, the setting is kept as a string and the strvalueP field set to it.

For any value type not seen, onOff is false, strvalueP is null, fvalue is NAN, ivalue is zero.
Use isnan() from math.h to test fvalue.

## Built-in settings

These are the settings that are processed by pidp-1 and by provided IOTs.
See the sample configuration file in /opt/pidp1-mods for details

- shared
- lailia
- core1D
- all1D
- muldiv
- sbs16
- sdb
- dpyshift
- lightpen
- guilightpen
- type30lightpen
- type30size
- aperture
- two340charsets
- audio
- alpha
- gain
- tuning
- samplerate
- alpha1
- alpha2
- alpha3
- alpha4
- lptType64
- lptLineSpacing
- lptLines
- lptNoFF
- newmemfile
- type340
