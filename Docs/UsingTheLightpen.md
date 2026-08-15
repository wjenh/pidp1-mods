# Using the lightpen

This document describes the lightpen simulation implemented in the emulator.

Updated 11-Aug-2026

## Implementation

Pseudo-lightpen support is implemented in the pidp1 emulator in display.c.

While it can't exactly duplicate the function of the original hardware lightpen because there isn't one,
it can very closely simulate it.

It works by reading commands from the same file descriptor used by the display instructions
used to send display commands to a display client.

The reads are non-blocking. Each command is an unsigned 32 bit value, uint32_t, although any 32 bit value
will work. The command has the format:
```
0xFFcppppp

The high byte is always FF to signl a lightpen command.
The 4 bit c value is either 1 to indicate the lightpen was raised or 0 to indicate it is on the screen.
The 5 bytes of p are the packed x and y coordinates.
```

The coordinates consist of two 10 bit 1's complement values with 0 meaning the center of the screen, the same
as the coordinates used with the *dpy* instruction.

They are packed:
```
(x << 10) | y
```

When a *dpy* instruction completes 35 microseconds after it is issued, any queued commands are read and only
the last one processed. This aligns the lightpen coordinates with the instruction.

If the coordinates of the point displayed match the last lightpen coordinates within the aperture setting and
the lightpen is on the screen, then a hit has occurred.

When a hit occurs, program flag 3 is set and bit 0 in the status returned by *cks* is set to 1.

The *cks* status is reset by the next executed *dpy* command. The program flag is not automatically reset.

**IMPORTANT**

The lightpen status is only updated when completion is requested.
If there is no completion event, no update will occur.
This matches the behavior of the original hardware.

This means that *dpy-i*, no completion requested,  will not see any lightpen events.
However, the form that requests completion, *dpy-i 4000* or in am1, *dpy C* followed by an *ioh* will work.

## The aperture

The original lightpen had a set of 6 aperture attachments to control the field of view of the lightpen
ranging from an opening of 0.05" to 0.30" in 0.05" increments.\
These corresponded to the pixels that would count as a hit in its view.

The simulated lightpen pixel equivalents are computed as a fraction of 1024 pixels, so the actual
size of course depends upon the display size. The Type 30 display plotted in a 9.25" square, corresponding 
a pixel size of 0.009".

|Aperture|Diameter in pixels|
|--------|----------------|
|0.05    |6|
|0.10    |11|
|0.15    |17|
|0.20    |22|
|0.25    |28|
|0.30    |33|

Am1 has an include file that defines these, #include <LIGHTPEN/lightpen.ah>.

## Setting the aperture

An extended *dpy* command has been provided, *dpy-i 03400*, 723407.

It expects the IO register to contain the aperture size in pixels to use in bits 12-17.

For example:
```
lio [6, or lio (6 for macro1
dpy-i 3700
```
Sets an aperture diameter of 6 pixels.

This command does **not** support completion, don't do a wait!

## Motion prediction

One problem with simulating the orginal lightpen is that it was a *coincidence detector*. It produced an immediate
signal when it saw a dot displayed on the screen, making it easy to tie the hit to the exact display instruction
that caused it.
But, with modern raster displays that are very decoupled in time from the display instruction, the job becomes
difficult.

A feature implemented in the pidp-1 display logic helps with this, a 2 axis cascaded
Position-Velocity-Acceleration continuous-time kinematic observer state filter with non-linear
predictive lead compensation.

Say what?

It's a special mathematical function that uses the history of lightpen position updates over time to be able to
compute the current location of the 'pen' very accurately, removing much of the time uncertainty.
This allows the logic that matches the coordinates of a detection with the IOT instruction that caused it to be
more precise.

In combination with special support in the t30dpy display program, the lightpen responds as fast and as accurately
as the original real lightpen did.

## What uses it?

On the PDP-1 side, the Type 30 and Type 340 display drivers support the pen as it was handled in the original.
The replacement for p7sim, t30dpy, fully supports the lightpen with special optimizations that dramatically
improve tracking over the other displays below.

The p7sim Type 30 display emulator and the integrated gui also have support, but tracking is much slower.
There is also support in the alternate Type 30 implementation found at https://github.com/Isysxp/PDP1-DPY.git,
also with the same tracking limitations.

## The mouse-pen

The lightpen is simulated by the mouse.\
As long as the left buttion is pressed, the lightpen is on the screen and the cursor is the lightpen.
Any pixel being drawn that is within the aperture around the cursor will be a hit.

When the mouse button is released, the lightpen is off the screen.
