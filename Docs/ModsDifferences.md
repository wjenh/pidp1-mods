## Differences between pidp1-mods and the original pidp1 branches

As of 11-Mar-2026

Differences fall into these broad categories:
- Changes that are not visible to code or execution
- Chages that only affect instructions
- Chages that are extensions to original IOTs
- Changes in peripherals that do not affect interoperability
- Changes that require the pidp1-mods branch
- Changes that can be used for either branch or any other PDP-1, real or emulated

## Invisible changes

These changes are additions to the core pidp-1 emulator.
They are not controlled by code, nor do they affect operations, unless of course you use them.
Even then, they do not affect program execution in general.

These changes are:

- improved audio implementing digital filters to mimic the analog filtering on the actual PDP-1.
- the ability to add new IOTs via shared objects that do not require any changes to emulator code.
- control of features via a configuration file read at startup.
- support for the ad1 debuggger in the emulator.

## Instruction changes

These changes only make a difference if actually used.
Note, however, using some will mean code will not execute propery on the non-mod emulator.

These changes are:

- addition of the PDP-1D instructions sni, szi, cmi, lsw, lai, lia.
Note that lai and lia are supported by the non-mod emulator but only if compiled in.
- addition of the sdb variation of dpy for the Type 33 Symbol Generator.
- addition of the origin-shifting extension to dpy to allow shifing the 0,0 origin from center to corners.
- addition of the aperture-setting extension to dpy to emulate the different aperture masks the lightpen had.

All of these can be enabled or disabled via the configuration file without any recompilation of the emulator.
If you are using the ad1 assembler, it can warn about the use of the PDP-1D instructions.

## Extensions to original IOTs

These are additonal functionality added to IOTs, not to primary instructions, and will not affect code
that doesn't use those IOTs.
These are done to allow setting parameters that originally were implemented in hardware or to
provide functionality needed for operation in the modern world.

These changes are:

- new subcommands for the Type 630 Data Communications System IOT.
These are extensions to allow specifying parameters such as port numbers, client/server, etc.
- new subcommands for the BBN timesharing clock to add a countdown timer, the ability to change interrupt channel,
and the ability to enable the interrupt system.

## Changes in peripherals that do not affect interoperability

These changes are to various of the peripheral emulations such as the Type 30 display, p7sim, etc.
These changes should be transparent so that a peripheral compiled for either branch will work with the other, but
possibly without some new functionality.

These changes are:

- p7sim additional command-line and config file settings for screen size, lightpen support.
Note that lightpen support has been added to the main branch now, but without aperture mask settings.
- lightpen support in the integrated gui, same note as above.
- changes to the hardware panel driver, vpanel, to greatly reduce cpu load and more closely mimic the appearance of the actual PDP-1.
This should be interoperable with wither branch, although it has not been tested against the original at this time.

## Changes that require the pidp1-mods branch

These are changes that are not avaialble in the original branch.
Some of these have been mentioned above.

- emulator startup configuration file to set, enable, and disable many things.
This is also used by p7sim if it is accessible on the machine running p7sim.
- improved audio output.
- extended PDP-1D instructions.
- extended lightpen functions.
- any dynamically-loaded IOT, which is all of the ones added in the mods branch.
- support for the ad1 symbolic source-level debugger.
- various standalone utilities generally for the Type 23 Parallel Drum.
- all of the demo programs that are in assembler unless the IOTs they use are (compatibly )
implemented in the original branch.
These also use am1.

## Additions that are independent of any branch

These are standalone programs that can be used for any branch or, in theory, any other PDP-1 emulation such as simh,
or even on the real PDP-1.

These are:

- the am1 advanced macro assembler
- the tape disassembler
- the macro-to-am1 converter
