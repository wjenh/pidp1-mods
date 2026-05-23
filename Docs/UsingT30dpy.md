# Using t30dpy

This document describes the t30dpy simulated Type 30 display.

This is version 1.1

Edit date 23-May-2026\
Minor typo corrections, more test results

## What is it?

T30dpy is a replacement for the p7sim and p7simES display simulators for the pidp-1.
It is a completely new implementation with no common code.

It allows a screen size to be specied at startup, from 256x256 up to whatever your machine and SDL can manage.
It also supports borderless and full-screen operation and supports the pseudo-lightpen via the mouse.

It will work with any pidp-1 version, or with anything that uses the same display protocol.

## Why have it?

The original p7sim has multiple issues. Most obviously, it is a massive cpu hog.
Aside from that, there are some smulation issues around the phosphors and it is unble to handle the data
rate the Type 340 vector display system can generate.

A tweaked version improved performance signficantly, and is what is provided in the pidp1-mods repository.
However, even so the above issues still exist.

## Ok, so what's different?

P7sim is quite complex, overly so. It uses GL for its graphics, which requires writing custom renderers and shaders.
Given the lack of any comments or design documents, trying to decipher it is painful.
It simulates each Type 30 displayed dot using 7(!) triangles in an apparent effort to mimic the round CRT dot and the
beam spread that happens with increasing intensity.
But, the actual displayed dot is only a few pixels in size, totally pointless to simulate that in this much detail.

For some reason, p7sim gets aging time updates from the pidp-1.
This is strange, aging was a characteristic of the display, not of the computer.
T30dpy ignores the aging commands and handles all aging itself, more true to the original.

As for the p7sim phospor simulation, it does not seem correct.

From the **RCA Phosphors TPM-1508A** technical book from October 1961, the P7 phosphor used in the Type 30
was a dual phosphor consisting of a blue-purple high intensity phosphor with a 25-75 microsecond decay time to 10%
of the initial intensity amd a color of 435 nanometers,
and a secondary yellow-green longer persistence phosphor with a 400 millisecond decay time
to 10% of intensity and a color of 555 nanometers.

The initial p7sim dot is whiite while the decay time of the yellow-green phosphor is much longer than 400 milliseconds.

T30dpy uses the correct power-law decay to give proper decay times, at least for the yellow-green phosphor.
The blue-purple phosphor also uses the proper decay time, but both p7sim and t30dpy really can't correctly model it.

Why? Given the 50 microsecond decay, no LCD monitor can reproduce that short an interval. Additionally, the initial dot
was displayed in 5 microseconds or less at an intensity no LCD can come close to, which is how it was visible.

The solution is two-fold.
First, the decay time is stretched considerably to several frames.
Second the initial frame uses a color value that is brightened considerably by adding red and green, but in the proper
ratios to essentially add white to the base color.
This only lasts for one or two frames, then the accurate color is used.

T30dpy runs at a fixed 30 frames per second and the alpha values for the colors are calibrated to that to give,
for the secondary phosphor, a 400 millisecond decay to 10%, as specified.
You will see that it decays signficantly faster than p7sim does.

Finally, the simulated beam spread done by p7sim is overly exaggerated. Take a look at any real CRT with similar resolution, such as an oscilloscope for comparison.

DEC stated a beam spot size of 0.030 inches, which works out to about 2 pixels on the original CRT.
T30dpy uses a simple sparse rectangle ranging from 2 pixels square up to 4 square at the highest intensity.
That is a slight exaggeration but again needed to try to get an LCD to look close to the CRT.

## Implementation

T30dpy is a pure SDL2 implementation.
No GL libraries are needed.

It uses a streaming texture for composing the dots which is then passed to the standard renderer for display.

On startup, it computes the alpha values needed at each step in the aging for both phosphors to follow the power law
decay. The decay parameters are set so that the secondary phosphor drops to 10% of its initial intensity in
400 milliseconds.

256 intensity steps are used occurring at 33.33 millsecond intervals, a frame rate of 30 frames per second.
Thus, the total aging span is up to 8.4 seconds.
A power law decay results in a long, low intensity tail and potentially that is simulated.
However, since LCDs can't duplicate the true analog intensity decay of a CRT, the intensity value drops below
what the LCD can display fairly rapidly. However, the primary decay interval is apparent.

At each frame step, each phosphor color has its alpha value modified by the alpha aging determined at startup by
the power law generator.
Initially, alpha blending is done of the two colors, but once the primary phosphor decays to an insignificant
value, blending is no longer needed and the secondary color is used by itself.

A table of point data 1024 x 1024 in size, to match the Type 30 resolution, is kept.
As points come from the host, the proper cell is marked as in use and its lifetime set to 0.
Each frame increments the lifetime after the alpha computations are done and if the 256 step limit has been reached
the point is marked as inactive and will no longer be drawn.

A point is inactivated after its intensity falls to 0 for the primary phosphor and to a very small value for the
secondary phosphor.
In practice, the primary value goes to 0 in a relatively few frames, the seconddary dominates as you would expect.
The secondary termination value is configrable in the code, as are many other values.

See the source code for much more information, it is well commented and structured.

## Window size and pixel scaling

The texture that is drawn to is always 1024x1024, but the window that the texture is rendered to is whatever
size was specified, and in full screen mode, whatever size that is.

Unfortunately, SDL2 sometimes doesn't do a very good job of rendering pixels when the window size is less
than the texture size, it depends on the size.
Power of 2 submultiples generally seem to work well, e.g. 768, 512.
But, that doesn't seem to always be the case, experiment.

The code uses nearest texture scaling, which seems to work the best.

## Performance

Ok, great. What about performance?

Here's a table of average CPU load by program and by display emulator.\
It was run on a middling performance Intel CPU of some age running Ubuntu Linux with no special graphics hardware.

| Program       | Emulator  | CPU load reported by top |
|---------------|-----------|--------------------------|
| snowflake     | type30dpy | 80%     |
|               | p7sim-mod | 184%    |
|               | p7sim     | 284%    |
| spacewar48    | type30dpy | 86%     |
|               | p7sim-mod | 314%*   |
|               | p7sim     | 280%    |
| lines         | type30dpy | 80%     |
|               | p7sim-mod | 170%    |
|               | p7sim     | 285%    |
| type340lines  | type30dpy | 109%    |
|               | p7sim-mod | 168%    |
|               | p7sim     | 304%**  |
| cg340full     | type30dpy | 95%     |
|               | p7sim-mod | 180%    |
|               | p7sim     | 298%    |
| type340stress | type30dpy | 140%*** |
|               | p7sim-mod | 204%    |
|               | p7sim     | 314%    |

\* - this is an anomoly, but repeatable\
\** - p7sim can't handle the 150K+ pixels/sec type304lines sends, loses many pixels\
\*** - this is a brutal test sending over 500K pixels/sec. p7sim fails miserably, dropping many many pixels

T30dpy can also report some timing statistics.
Running type340stress, it is rendering over 5 million dots per second without any lost data.
Note that these are Type 30 dots, the number of pixels being rendered is over 4 times as many.

Tests have also been run on a Raspberry Pi 5 with similar results.
However, tyoe30dpy seems to get resheduled more often, not clear why.
P7sim uses much more memory.

## Usage

Finally.

**type30dpy** [*-b*] [*-s size*] [*-p port*] [*-t*] [hostame]

```
b - start borderless
s size - set the screen size, >= 256, default is 1024
p port - the port to connect to, default is 3400
t - accumulate statistics, printed on exit
hostname - the host to connect to, defaults to localhost
```

While running the F11 key or the *f* character key toggles between regular and full screen mode.\
The *b* character key toggles between a bordered and borderless window.\
The *escape* key exits, as does the window close icon on the window.

## Settings that can be changed

These are in the source code, change with care, or just to play around.
The code values are a good approximation of the original display.
Not all are listed here, just the ones that are useful for display appearance.

| Name             | Default    | Action                                                   |
|------------------|------------|----------------------------------------------------------|
| BLUEDECAYALPHA   | 1.5        | determines the blue fade time, larger is faster |
| YELLOWDECAYALPHA | 0.85       | determines the yellow fade time |
| GAMMA            | 0.7        | changes how much dim points are enhanced, smaller is more |
| BLUEFIRSTRGB     | 101,40,255 | the rgb value for the initial dot color |
| BLUERGB          | 61,0,255   | the true rgb value for the blue phosphor |
| YELLOWRGB        | 179,255,0  | the true rgb value for the yellow phosphor |
| BLUEHOLD         | 6          | how many frames the first blue rgb is used for|
| YELLOWDEFER      | 2          | how many frames before the yellow rgb is blended in |
