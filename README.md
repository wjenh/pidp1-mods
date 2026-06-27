## Pidp-1 mods

This contains files modified from https://github.com/obsolescence/pidp1 to add new functionality and fix some issues.
Note that this no longer tracks the original repo since this branch has diverged too much for automatic tracking.
The link was removed on 3-Feb-26 to make this a fully-independent repository.
Selective updates from the original branch are made now.

Many unused files from the original have been cleaned out and some of the original **C** files updated to
have some comments and be more readable.

Almost 5,000 lines of documentation in over 20 documents have been added in *MD* format,
not counting comments in the source code. Commented code? Legibly formatted commented code? Gasp!

In addition to new emulator features such as lightpen support, high speed channels, and dynamically-loaded IOTs,
it also adds various tools such as the am1 assembler and include files for am1,
the ad1 symbolic debuger, drum utilities, documentation, etc.

Implementations of the Type23 drum, the DCS communications system, the Type 33 symbol generator, the Type 340
advanced graphics display with all options and multiterminal support,
both the Type 62 and Type 64 line printers and the BBN timesharing clock are provided.

A number of demo programs in **am1** assembler are also provided.
Note that **am1** can generate **macro_1** code if you really want to use it, but there is no reason to do so.

A sample drum image containing multiple programs is provided, use the *drumlist* tool to see what's included.

See the philosphical ramblings at the end of this document if you want to know why this version has gone
in the direction it has.

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

Checkout must be done in the /opt directory.

It is strongly recommended to use:

```
sudo git clone --depth 1 https://github.com/wjenh/pidp1-mods.git
```
to avoid also pulling down all the historical changes, which can be large.

As of 4-March-26, pdf versions of the documentation is no longer being checked in, use a good md viewer
in your browser, such as Markdown Viewer.

## Installing and building

It contains a full build tree.
When checked out in the /opt directory it will make its own installation directory, which will always be pidp1-mods.

Everything after this proceeds as for the original install.
Go into the /opt/pidp1-mods/install directory, type './install.sh' and follow the prompts.

## Configuration file

A configuration file, /opt/pidp1-mods/pidp1.config, is read when the emulator starts.
 t allows control over many features, see the provided one and the documentation for details.
It can be changed on the fly and a *SIGINT* sent to the pdp1 process to have it reload the configuration.

There is also a directive for the pdp1control program:
```
pdp1control reload
```
that will do the same.

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
- New display subsystem

The following IOTs are also built:

- Type 33 Symbol Generator
- Type 23 Parallel Drum
- Types 62 and 64 line printers
- Type 630 Data Communications System
- BBN-style real time clock
- Type 340 Vector Graphics Display

These features can be enabled and diabled via the configuration file, as can
the lightpen, audio, mult/div, PDP-1D extensions,  dpy origin shift, and sdb.

One dpy option, reorigin dpy coordinates, conflicts with the lightpen sdb iot.
If both are enabled, the lightpen sdb iot takes priority.

## Changes in existing applications

The visible changes are in the hardware front paenl, gui, and Type 30 display.

- gui app now has lightpen support, set in the config file
- Type 30 display now has lightpen support, set in the config file
- Type 30 display now has the ability to set the screen size on startup, command line -s size or config file
- Type 30 display can start bordered or borderless, command line -n, no border, or config file
- Type 30 border/borderless can be toggled using the 'b' key while running
- Improved performance hardware panel driver, panel_pidp1, not configurable. Looks more authentic, too
- Improved performance of p7sim

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

## Why not track the original repo? Some philosophy.

There are several reasons, but the primary one is a difference in philosopy.

The original branch was centered around a view of *inside the black box is what is important*.
While this did provide a pretty accurate representation in software of the detailed operation of the original hardware,
software is not a collection of wires, flip-flops, etc. This approach, while fascinating and laudable, definitely meant
complexity and obscure operational details. It also resulted in logic that was never an intrinsic part of the actual
processor being intermingled, specifically the I/O system.

This version goes in the opposite direction. The PDP-1 is a cabinet with stuff in it. What that stuff is in detail is
not what is important, the externally-visible behavior is. This is the "it's a black box" approach.
If it acts like a PDP-1, code can't tell the difference, then it's a PDP-1.
This does mean that timing should be respected, but no one needs to know about the 10+ subcycles that go on inside.

One major change has been to move much of the I/O out into independent code.
This is in general more like what the real implementation was.
The PDP-1 provided external cnnections to allow arbitrary devices to be connected via available slots in its cabinets
with specific control and data lines provided. The devices were controlled by IOT instructions and the specific
devices dealt with their specific details without modifiying the main processor logic.

The new version provides a much cleaner interface and allows easy addition of new devices without having to decipher
the internal logic. A good example is the complete removal of all of the display related logic from the main
emulation and into separate independent code.

New code is commented and formatted, and documentation is provided for all of the new features.

On the other hand, there were some hardware features that could be added that were modifications to the main logic.
One example is additional instructions added in various PDP-1 versions, such as the -1D instructions.
Another is standard extensions like high speed channels.

An effort has been made to add these features in a reasonably authentic way. For example, the -1D extensions operate
in the currect subcycles. But, the high speed channels worry more about the external behavior and operate internally
without replicating all of the flip-flops, etc. High speed channels still replicate the timing and the cycle-stealing
the original hardware did, but not necessarily at the same internal time points.
Again, it's a black box.

Then, there are features that never existed. These are either for convenience, such as the runtime configuration
file, or to interface with the modern world, such as the Data Communications System IOT that now understands
sockets and ports.
In all cases, the original core behavior remains as it was, new features are extensions, not replacements.
For devices for which original diagnostic tapes have been found, IOTs have been confirmed to pass those tests.

Finally, there is new software like the **am1** assembler.
This falls into the convenience class also.
If you want to fully recreate the original experience, you can of course use the original tools.
However, the macro1 assembler is crude and error-prone.
The new assembler and related tools is much easier and safter to use, and increases productivity.

Philosophically, this is not wrong.
We don't use paper tape today.
We generally don't write in assembler today.
None of the original developers would have refused to use a better assembler because it wasn't the same one
that originally existed.
In fact, there were several other assemblers at the time, such as **concise** and **midas**.

Note also that PDP-1s were freely hacked, differences exsited across many of them, the most extreme example
being the MIT PDP-1X, although the BBN PDP-1D is pretty close.

There is one more practical reason for this divergence. The original version is really not being actively maintained
and it's not likely it will be. This version is supported and provides not only a full set of additional original
device implementations but also provides an easily-extensible framework.

Regardless of your own philosophy, the PDP-1 is an amazingly fun computer to play with, enjoy!

Bill
