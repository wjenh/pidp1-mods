A CalComp incremental drum plotter, the 560, 563 or 565 that DEC sold with a PDP-1 controller.
The paper is written as a PNG file at one pixel per step, 100 dots an inch.
See Docs/UsingThePlotter.md for the IOT, the timing, the settings and an example.

IOT_73.c is the plugin, pngWrite.c the PNG encoder it uses, uncompressed, no library needed.

A make builds IOT_73.so, make install copies it to the IOTs directory.
IOTs/install.sh does both.

Am1Includes/PLOTTER/plotterdefs.ah has the instruction names and the cks flag bit.

The IOTs of DEC's PDP-1 plotter control have not been found.
The commands are those of DEC's Type 350 plotter control for the PDP-7, hopefully the same.

Tests has the host tests which need no emulator and plottest.am1, a program that draws a
figure, times it and counts its breaks, see Tests/README.md.
