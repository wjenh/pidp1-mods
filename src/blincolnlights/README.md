# Blincolnlights

This repository contains code to interface with the physical PiDP-1 panel.

The panel drivers and the emulator communicate by mmap'ing
a file that represents the physical panel.
That way a physical panel is easily swappable for a virtual one
or another user interface.

For the PiDP-1 panel, start `panel_pidp1/panel_pidp1` before
starting any emulator.
The virtual version of this is `vpanel_pdp1/panel_pdp1`.
