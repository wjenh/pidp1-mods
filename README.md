## Pidp-1 mods

This contains files modified from https://github.com/obsolescence/pidp1 to add new functionality and fix some issues.
Note that this no longer tracks the original repo since that seems to have been abandoned by the developers.
The link was removed on 3-Feb-26 to make this a fully-independent repository.

It also adds various tools such as the am1 assembler and include files for am1, the ad1 symbolic debuger,
drum utilities, documentation, etc.

A sample drum image containing 9 programs is provided, use the *drumlist* tool to see what's included.

The entire build can exist in parallel with the original distribution, but if the OS commands and desktop items are
selected, they will overwrite the originals.
The origial settings can be recovered by running its install.sh script again.

**If you run in parallel** be *sure* you know which commands are being executed.

Commands are not interchangeable between the two versions!
Using commands from one against a pidp instance from the other is guaranteed to cause strange behavior.
The safe way is to run the commands directly from the bin directory in the version repository you want, and also be
sure the pdp1 instance matches.

Moral - running parallel versions is tricky. Best way is to do a full install from whichever one you want to use,
you can always reinstall from the other one if you want to switch

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

## Configuration file

A configuration file, /opt/pidp1-mods/pidp1.config, is read when the emulator starts.
It allows control over many features, see the provided one for details.

## What is configured by default?

The default for the emulator is basically *everything*:

- The useful PDP-1D instructions, lia, lai, lsw, swp, cmi, sni, szi
- Lightpen support and an extended dpy for setting the aperture size on the fly
- Lightpen support in p7sim and p7simES
- Dynamic IOTs
- Improved audio and a control app for it
- High Speed Channels
- Type 33 Symbol Generator added iot, sdb
- Improved performance hardware panel driver

The following IOTs are also built:

- Type 33 Symbol Generator
- Type 23 Parallel Drum
- BBN-style real time clock
- Type 630 Data Communications System
- IOT 60, a nonstandard iot to enable and disable the SBS16 interrupt system

Many of thes features can be diabled via #defines in pdp1.c and p7sim main.c.\
The lightpen, audio. mult/div, dpy origin shift, and sdb can be enabled or disabled and other values set
in the config file.

One dpy option, reorigin dpy coordinates, conflicts with the lightpen sdb iot.
If both are enabled, the lightpen sdb iot takes priority.

The tools, including am1, are not automatically built, see below.

## **NOTE**

You can build some parts without building the entire set.
For example, if you want to use the new assembler, you can just do a make in its directory.
You will need to have flex and bison installed to build it.

## The drum image

An initial drum image for the Type 23 Parallel Drum can be installed.
It contains several program images that can be run by several of the PDP-1 programs
in the FunStuff directory and managed by the drum tools in Tools/Drumupdater.
