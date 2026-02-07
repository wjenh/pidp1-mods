## Pidp-1 mods

This contains files modified from https://github.com/obsolescence/pidp1 to add new functionality and fix some issues.
Note that this no longer tracks the original repo since that seems to have been abandoned by the developers.
The link was removed on 3-Feb-26 to make this a fully-independent repository.

It also adds various tools such as the am1 assembler and include files for am1, drum utilities, documentation, etc.

A sample drum image containing 9 programs is provided, use the *drumlist* tool to see what's included.

The entire build can exist in parallel with the original distribution, but if the OS commands and desktop items are
selected, they will overwrite the originals.
The origial settings can be recovered by running its install.sh script again.

## Installing and building

It contains a full build tree.
It is checked out in the /opt directory and will make its own installation directory, which will always be pidp1-mods.

Move to /opt, check this out there:

```
sudo git clone https://github.com/wjenh/pidp1-mods.git
```

Everything after this proceeds as for the original install.

## What is configured by default?

The default for the emulator is basically *everything*:

- The useful PDP-1D instructions, lia, lai, lsw, swp, cmi, sni, szi
- Lightpen support and an extended dpy for enabling and disabling it and setting the aperture size
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

Many of thes features can be diabled via #defines in pdp1.c and p7sim main.c.

One dpy option, reorigin dpy coordinates, conflicts with the lightpen.
It can be enabled in pdp1.c and lightpen support diabled.

The tools, including am1, are not automatically built, see below.

## **NOTE**

You can build some parts without building the entire set.
For example, if you want to use the new assembler, you can just do a make in its directory.
You will need to have flex and bison installed to build it.
