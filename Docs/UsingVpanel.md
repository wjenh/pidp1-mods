## Using vpanel_pdp1

This document describes vpanel_pdp1, the on-screen PDP-1 front panel, and how to use it.

This is version 1.1\
Edit date 15-Nov-2026\
the front panel in the browser added

## What is it?

Vpanel_pdp1 is a window showing the PDP-1 console: the lamps, the toggle switches, and the keys.
It can be used in place of the PiDP-1 hardware panel, it does the same job as panel_pidp1 (newpanel) does
for the hardware, and like that it is started before the emulator.
It can also run alongside the hardware panel as a second view of it, see below.

Above the console it also shows the rack top (I/O) indicator panel, laid out like the PiDP-1's top panel:

- TAPE READER, the reader buffer, bits 0-17
- TAPE READER status: BINARY, NAC, RC 1, RC 2, CLUTCH
- MEMORY: READ, STROBE, INHIBIT, WRITE
- TYPEWRITER: STATUS, BLACK, TYO, NAC, and the typewriter buffer, bits 12-17
- TAPE PUNCH, the punch buffer, channels 8-4 and 3-1, ON, and NAC
- DEFER 2 and OV 2

## With the hardware panel

Vpanel_pdp1 can run at the same time as the hardware panel, on a monitor or remotely for example.

It looks for the hardware panel's driver, panel_pidp1 or newpanel, when it starts, about once a second
after that, and before each mouse button press.
The driver runs when the front panel choice is *pidp*. It also runs on a Pi with no PiDP-1 board connected,
thus vpanel_pdp1 cannot see whether the board is there.
While the driver runs, the hardware panel is always the authority and vpanel_pdp1 mirrors it:

- the lamps show the same as the hardware's
- the switches and keys show where the physical ones are, clicking them does nothing
- the window title says *mirroring hardware panel*

When the driver isn't running, the on-screen switches and keys work the machine as usual.
If the driver stops, vpanel_pdp1 takes over the switches where the physical ones were left, so the
machine sees no change, only the keys go back to neutral.
If the driver starts, vpanel_pdp1 stops working the switches as soon as the driver changes one,
or within about a second if the physical switches match the screen.
A click in that second is not written: vpanel_pdp1 looks for the driver first.

Vpanel_pdp1 never overwrites a switch setting another program has changed, it takes the new setting instead.
So when the hardware panel starts with its switches set differently from the screen, the emulator only ever
sees the hardware's settings.
That matters for POWER: the emulator power clears, resetting the machine, if it sees POWER off even briefly.

## The front panel in the browser

The web interface, the *web* choice in pdp1control.sh or pdp1central, also shows the front panel:
the *Front Panel* section at the top of its page, at *http://<pi>:8080*.
It is drawn from the same art and layout as vpanel_pdp1, by the browser, so it responds at once
even when the Pi is a Pi Zero across the network, unlike vpanel_pdp1 over ssh X forwarding.

- the lamps show on or off, as in vpanel_pdp1
- the mouse works the switches and keys as in vpanel_pdp1, below
- *Front Panel* in the *Controls* sidebar shows or hides the whole front panel, off at first, remembered by the browser
- *Rack Panel* shows or hides the rack top (I/O) panel; *800px*, *1200px* and *Full Width* set the size;
  the browser remembers both
- while the hardware panel's driver runs, it mirrors the hardware panel, as vpanel_pdp1 does,
  and says *mirroring hardware panel*; the server looks for the driver before every switch change,
  so a click never reaches the emulator once the driver has started
- with the emulator not started, it says *emulator not running*

Like vpanel_pdp1, it never overwrites a switch setting another program has changed:
the page sends the settings it was showing along with the ones it wants, and only the switches
that differ are changed.

The web server, web_pdp1/panel.go, reads the panel the same way vpanel_pdp1 does, so it can run alongside
vpanel_pdp1 and the hardware panel. Only one browser can be connected at a time, as for the rest of the web interface.

The art and layout in web_pdp1/srv/panel are made by the same script as vpanel_pdp1's, see *The art*.

## Using the panel

Mouse buttons work the switches and keys:

| Button | Toggle switch | Key |
|--------|---------------|-----|
| left | flip it | press it down while held |
| middle | up (on) | nothing |
| right | down (off) | lift it up while held |

DEPOSIT works only by lifting it, the right button, as on the real machine.
START lifted is start in sequence break mode.

A switch that is up (on) has its dark lever above the pivot, down (off) below it.
The POWER, SINGLE STEP and SINGLE INST switches are on when the lever points left.
A key shows a dark lever while it is pressed down or lifted up, and only the end of it at rest.

Each lamp is shown on or off, a lamp lit for at least half the time since the last frame is on.

## Running from the command line

**vpanel_pdp1** [-s scale] [-i] [-n]

- s set the window size multiplier, 0.5 to 4.0, 1.5 for example
- i show the rack top (I/O) panel
- n don't show the rack top (I/O) panel

Command line switches override configuration file settings.

At scale 1 the window is 800 by 668, or 800 by 448 without the I/O panel.
At scale 2 that is 1600 by 1336, taller than a 1080 line screen, use 1.5 there or hide the I/O panel.

The window can also be resized with the mouse, the panel keeps its shape and fills as much of it as it can.

## Configuration parameters

These settings are read from */opt/pidp1-mods/pidp1.config* when vpanel_pdp1 starts:

| parameter | values | default | effect |
|-----------|--------|---------|--------|
| vpanelscale | 0.5 - 4.0 | 1.0 | the window size multiplier |
| vpaneliopanel | true, false | true | show or don't show the rack top (I/O) panel |

## The art

The panel art is built into the program, it needs no files at run time.
It is all made at twice the size it is drawn at scale 1, so it stays sharp up to scale 2.
It is made by *src/vpanel_pdp1/art/mkvpanelart.py*, from the original console art,
*pdp1_panel.png*, see that file if the art is to be changed.
The script also writes the browser front panel's copy of the art and its *layout.json*, every lamp,
switch and key's place taken from vpanel_pdp1's own element table, so the two always match.
Run it again after changing the art or the layout, and commit what it writes in both places.
