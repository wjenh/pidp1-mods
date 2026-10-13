## Pidp-1 Dacmusic 

This contains an addon for the pidp1-mods installation.
That must be installed first.

It provides a program written in am1 assembler that reads Peter Samson Harmony-compiled music tapes,
lets you annotate them with 'real' instruments, room acoustics, and much more, then play your creation.
The music can also be punched out, still in Harmony binary format, but with an appended metadata block
so you can preserve your masterpiece.

You can add new instruments or modify the existing ones using included standalone programs.
They generate the wave tables for the program and get stored on the Type 23 drum.

It also implements 2 new IOTs that provide a sofware emulation of a 4 channel 12 bit multiplying A/D converter
with additional support for an exponential decay generator and some other features.

The guiding principle is historical accuracy, so how can this possibly qualify?

In the pidp1-mods world, it means that if somethong could have been done in the era, even if it wasn't,
then that something is historically accurate.
The PDP-1 was a hacker's dream and if someone had thought up some hack and it was useful, it probably happened.

But isn't this a bit overboard?

Much of the D/A logic  was actually implemented on the Stanford PDP-1D, #48, by John Chowning for researching this
very subject.

These techniques were also being used in the very first standalone music synthesizers at the end of the '60s,
but the cost meant that full digital synthesis did take a while longer.

But, everything being emulated here *could* have been built with DEC list-item D/As
and other 'System Building Blocks', and some analog circuitry,
for example a transistor junction for the exponential voltage generator, etc.
All it would have taken was money. Ok, a lot of it, but it still could have been done.

## Checking it out

**DON'T** clone it!

On the Code button, select *Download zip*.

Unzip it in your /opt/pidp1-mods directory, it will copy everything into the correct place.

Git doesn't handle nested repositories well at all.
If you clone into your pidp1-mods, it won't work.

If you do want to track this repository, then create a directory outside of pipd1-mods and you can then clone into it,
but you'll have to copy any updates there yourself.

## Installing and building

It contains a full build tree for its components.

First, cd to IOTs/DAC, type 'make install'.
Then in the Funstuff/Dacmusic, type 'make'.

That's it.
**READ THE DOCUMENTATION!**, or you won't have any idea how to use it!

It will use the Type 23 parallel drum, the Type 340 display system, and of course the new DAC IOTs.

Please don't use the awful little pidp-1 speakers!
In highly technical terms, they reproduce sound about as well as a tin can and string.

Plug some decent PC speakers into the USB dongle, or even better, do what the real PDP-1 at the
Computer History Museum does, plug it into a decent amp with real speakers, then be amazed.

## The license

There isn't one, use this as you wish, no restrictions.
Just credit me if you do something else with it.

Bill Ezell\
October, 2026
