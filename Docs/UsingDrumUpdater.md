# Using the drumupdater

This document describes the drumupdater and how to use it

## What is drumupdater?

Drumupdater is a program that will load a PDP-1 rim/bin program tape into a given Type 23 parallel drum track.
The program can then be loaded via several PDP-1 programs such ad drumloader, rotate, and switcher.

It also writes a program label to the track which can be used as a simple directory entry.
Note that the label is stored as packed ascii as produced by m1pp's ascii directive and will work
with the textasc utility provided in MacroIncludes/DCS.

An intiialized drum track is so marked, see below.

It only updates a single track and leaves the rest of the drum image as-is, so multiple programs can be
loaded or changed.

## Building drumupdater

Just type make.

## Usage

drumupdater [-i drumimagefile] [-t trackno] [-l label] tapefile

If no drum image file is given, the default is 'drumImage' in the local directory.

If no track number is given, the default is 0.

If no label is given, the default is the tapefile without any leading path or trailing extension.
Labels are limited to 34 characters, any longer will be silently truncated to that length.

## What's on the drum?

The contents of the input 'tape' are read just as would be loaded into core by read-in into an internal
4096 word PDP-1 buffer becoming effectively a full memory bank core image.

Locations 07751 - 07777 are then overwritten with the following contents:
```
07751-07772 the label
07773       the program start address from the tape
07774-07777 the bit pattern 0707070, which indicates the track has been loaded with a program
```

These locations are where the usual loader is placed, and so are not needed in the image.
Note however that any program that uses locations 07751 and up for its own purposes cannot be
saved to drum because obviously program data would be lost.
