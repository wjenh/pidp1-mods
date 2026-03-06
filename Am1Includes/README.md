## Include files for am1

This directory contains two forms of include file.

All ending in *.ah* only contain definitions, no code

All ending in *.ac* incldue code, place them in your code as desired.

All define markers so they are only included once, code includes might include other code as well as definitions.

Defines of iot commands already have iot specified, no need to do so in code although it won't hurt if you do.

## What's here?

- farmemcpy.ac, copy memory from one bank to another
- getnum.ac, give a string of ascii digits, convert to an 18 bit word
- memcpy.ac, copy memory from one location to another in the same bank
- memory.ah, a collection of inter-bank macros, farjmp, farjda, farjsp, fardac, etc.
- memset.ac, set an area of memory to a given value

## What's in the other directories?

See their README.md files.
