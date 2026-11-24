## Include files for the Type 62 and Type 64 line printers

This directory contains two forms of include file.

All ending in *.ah* only contain definitions, no code

All ending in *.ac* include code, place them in your code as desired.

## What's here?

- type62defs.ah, defines the iots for the Type 62 line printer
- type64defs.ah, defines the iots for the Type 64 line printer
- printFlex.ac, send a line of flexo text to the Type 62
- printAscii.ac, send a line of ascii text to the Type 62
- printFlex64.ac, send a line of flexo text to the Type 64
- printAscii64.ac, send a line of ascii text to the Type 64

The Type 62 and Type 64 files define the same names, so a program includes one printer's files only.

See the Docs directory for each printer's usage.
