# Description

This is the peripheral interface for the PDP-1 emulator.
It visually simulates the Type 30 display,
paper tape reader and punch, and the typewriter.

Mounting and saving tapes can be done via the keyboard.
For file selection to work, you need
`tkaskopenfile` and `tkaskopenfilewrite` in your PATH.

# Usage

## Arguments

`-h host` selects which host to connect to. Default is `localhost`.

`-p port` sets the port of the display. `3400` is default,
`3401` is the second display as used by certain versions of spacewar.

`-r resolution` sets the internal resolution of the display.
`1024` is default, but `512` may give noticeably better performance
on some hardware.
There is more work necessary to make the renderer work better
at even lower resolution.

After these options the next argument, if given,
is taken as the filename of a papertape image
that will be mounted in the reader at startup.

## Key bindings

| Key     | Action                     |
| ------- | -------------------------- |
| F7      | mount file in reader       |
| F8      | remount last file          |
| F9      | saved punched tape to file |
| F10     | clear punch                |
| Ctrl-+  | increase font size         |
| Ctrl--  | decrease font size         |
| Ctrl-F1 | toggle audio               |
| Ctrl-F2 | toggle mul/div option      |
| ------- | -------------------------- |
| F1      | cycle layouts              |
| F2      | toggle layout mode         |
| F3      | duplicate current layout   |
| F5      | re-read pdp1_layout.txt    |
| F6      | save pdp1_layout.txt       |

In layout mode you can rearrange the four regions:
left mouse button moves, right button resizes.
Cycling with tab and using the arrow keys also works.
Use ctrl to resize and shift for more precise movement.
Space bar hides and unhides windows.

The background can currently only be set by editing the configuration,
`pdp1_layout.txt` in the current directory.

# TODO

* Mouse controls for tape handling
* Light pen
* Second display
* Key binding cheat sheet
* Visual indication of audio/muldiv modes
* ???
