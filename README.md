## Pidp-1 mods

This contains files modified from https://github.com/obsolescence/pidp1 to add new functionality and fix some issues.
Note that this no longer tracks the original repo since this branch has diverged too much for automatic tracking.
The link was removed on 3-Feb-26 to make this a fully-independent repository.
Selective updates from the original branch are made now.

In addition to new emulator features such as lightpen support, high speed channels, and dynamically-loaded IOTs,
it also adds various tools such as the am1 assembler and include files for am1,
the ad1 symbolic debuger, drum utilities, documentation, etc.

Implementations of the Type23 drum, the DCS communications system, the Type 33 symbol generator and the BBN
timesharing clock are provided.

A sample drum image containing multiple programs is provided, use the *drumlist* tool to see what's included.

The entire build can exist in parallel with the original distribution, but if the OS commands and desktop items are
selected, they will overwrite the originals.
The origial settings can be recovered by running its install.sh script again.

**If you run in parallel** be *sure* you know which commands are being executed.

Commands are not interchangeable between the two versions!
Using commands from one against a pidp instance from the other is guaranteed to cause strange behavior.
The safe way is to run the commands directly from the bin directory in the version repository you want, and also be
sure the pdp1 runtime instance matches.

Moral - running parallel versions is tricky. Best way is to do a full install from whichever one you want to use,
you can always reinstall from the other one if you want to switch back.

## Checking it out

It is strongly recommended to use:

git clone --depth 1 https://github.com/wjenh/pidp1-mods.git

to avoid also pulling down all the historical changes, which can be large.

As of 4-March-26, pdf versions of the documentation is no longer being checked in, use a good md viewer
in your browser, such as Markdown Viewer.

## Installing and building

It contains a full build tree.
It is checked out in the /opt directory and will make its own installation directory, which will always be pidp1-mods.

Move to /opt, check this out there:

```
sudo git clone --depth 1 https://github.com/wjenh/pidp1-mods.git
```

Everything after this proceeds as for the original install.
Go into the install directory, type './install.sh' and follow the prompts.

## Configuration file

A configuration file, /opt/pidp1-mods/pidp1.config, is read when the emulator starts.
It allows control over many features, see the provided one for details.

## What is configured by default?

The default for the emulator is basically *everything*:
See the config file.

- The useful PDP-1D instructions, lia, lai, lsw, swp, cmi, sni, szi
- Lightpen support and an extended dpy for setting the aperture size on the fly
- Lightpen support in p7sim, p7simES, and the integrated gui
- Dynamic IOTs
- Improved audio and a control app for it
- High Speed Channels
- Type 33 Symbol Generator added iot, sdb

The following IOTs are also built:

- Type 33 Symbol Generator
- Type 23 Parallel Drum
- BBN-style real time clock
- Type 630 Data Communications System
- IOT 60, a nonstandard iot to enable and disable the SBS16 interrupt system

These features can be enabled and diabled via the configuration file, as can
the lightpen, audio. mult/div, PDP-1D extensions,  dpy origin shift, and sdb.

One dpy option, reorigin dpy coordinates, conflicts with the lightpen sdb iot.
If both are enabled, the lightpen sdb iot takes priority.

## Changes in expsting applications

The visible changes are in the hardware front paenl, gui, and Type 30 display.

- gui app now has lightpen support, set in the config file
- Type 30 display now has lightpen support, set in the config file
- Type 30 display now has the ability to set the screen size on startup, command line -s size or config file
- Type 30 display can start bordered or borderless, command line -n, no border, or config file
- Type 30 border/borderless can be toggled using the 'b' key while running
- Improved performance hardware panel driver, panel_pidp1, not configurable. Looks more authentic, too

## Lightpen support

The lightpen is simulated using the mouse.

Holding the left mouse button down means 'pen on screen'.
Moving the mouse while pen-down simulates moving the lightpen around the screen.
Mouse button up means the lightpen has been moved away from the screen.

As far as code is concered, the operation looks just the same as the original lightpen.

The simulation is fairly accurate, but because there is no real detection of light on the screen
some magic hadwaving is done in the emulator to correlate mouse coordinates with the dpy instructions.
It works surprisingly well.

## **NOTE**

You can build some parts without building the entire set.
For example, if you want to use the new assembler, you can just do a make in its directory.
You will need to have flex and bison installed to build it.

## The drum image

An initial drum image for the Type 23 Parallel Drum can be installed.
It contains several program images that can be run by several of the PDP-1 programs
in the FunStuff directory and managed by the drum tools in Tools/Drumupdater.
