# Description

This is the peripheral interface for the PDP-1 emulator.
It visually simulates the Type 30 display,
paper tape reader and punch, and the typewriter.

Mounting and saving tapes can be done via the keyboard.
For file selection to work, you need
`tkaskopenfile` and `tkaskopenfilewrite` in your PATH.

# Usage

| Key    | Action                     |
| ------ | -------------------------- |
| F7     | mount file in reader       |
| F8     | remount last file          |
| F9     | saved punched tape to file |
| F10    | clear punch                |
| ------ | -------------------------- |
| F1     | cycle layouts              |
| F2     | toggle layout mode         |
| F3     | duplicate current layout   |
| F5     | re-read pdp1_layout.txt    |
| F6     | save pdp1_layout.txt       |

In layout mode you can rearrange
the four regions:
left mouse button moves, right button resizes.
Cycling with tab and using the arrow keys also works.
Use ctrl to resize and shift for more precise movement.
Space bar hides and unhides windows.

The background can currently only be set by editing the configuration,
`pdp1_layout.txt` in the current directory.

# TODO

* Mouse controls for tape handling
* Resize typewriter font
* Light pen
* ???
