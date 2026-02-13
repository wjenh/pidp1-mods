## Include files for Type 33 Symbhol Generator

This directory contains two forms of include file.

All ending in *.ah* only contain definitions, no code

All ending in *.ac* incldue code, place them in your code as desired.

## What's here?

- type33iots.ah, defines the iots for the symbol generator
- ascchars.ac, loads the bitmaps for the ascii characters
- chars.ac, loads the bitmaps for the flex/concise characters
- digits.ac, dloads the bitmaps for the digits 0-9
- drawascchar.ac, draw an ascii character
- drawasctext.ac, draw a line of ascii text produced by the am1 ascii directive
- drawchar.ac, draw a flex/concise character
- drawdigits.ac, draw a digit 0-9
- drawtext.ac, draw a line of flex/concise text produced by the am1 text directive
- render.ac, draw a buffer of upacked bitmap 2-word sets
- unpack.ac, unpack a flexo/concise string into 2-word bitmaps
- unpackascii.ac, unpack an ascii string into 2-word bitmaps

See the Docs directory for detailed Symbol Generator usage.
