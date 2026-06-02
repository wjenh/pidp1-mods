# Using t30dpy

This document describes the t30dpy simulated Type 30 display.

This is version 1.4

Edit date 3-June-2026\
changes to the alpha bending details

## Usage

**t30dpy** [*-g gamma*] [*-w bias*] [*-l*] [*-m*] [*-n*] [*-s size*] [*-p port*] [*-t*] [*-v*] [hostame]

```
g gamma - set the gamma value, a floating point value usually in the range 0.4 to 1.0
w bias - set the white bias, added to the r and g channels of the blue phospor to increase whiteness
l - ask SDL to use linear scaling instead of nearest-neighbor
m - use Mike C mode, see below
n - start borderless
s size - set the screen size, >= 256, default is 1024
p port - the port to connect to, default is 3400
t - accumulate statistics, printed on exit
v - ask SDL to use vsync for frame synchronization
hostname - the host to connect to, defaults to localhost
```
The defaults can be overridden in the configuration file.

If dots look too saturated, change the gamma setting in the configuration file, see below.\
The default setting is 0.4545, try 0.6. Higher numbers reduce the saturation but also dim the yellow fadeout faster.

While running the F11 key or the *f* character key toggles between regular and full screen mode.\
The *b* character key toggles between a bordered and borderless window.\
The *escape* key exits, as does the window close icon on the window title bar.
## What is it?

T30dpy is a replacement for the p7sim and p7simES display simulators for the pidp-1.
It is a completely new implementation with no common code.

It allows a screen size to be specied at startup, from 256x256 up to whatever your machine and SDL can manage.
It also supports borderless and full-screen operation and supports the pseudo-lightpen via the mouse.

It will work with any pidp-1 version, or with anything that uses the same display protocol.\
It is highly configurable in the code and via a configuration file and command line switches.

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

As for the p7sim phospor simulation, it needed a bit of adjustment

From the **RCA Phosphors TPM-1508A** technical book from October 1961, the P7 phosphor used in the Type 30
was a dual phosphor consisting of a blue-purple high intensity phosphor with a 25-75 microsecond decay time to 10%
of the initial intensity amd a color of 435 nanometers,
and a secondary yellow-green longer persistence phosphor with a 400 millisecond decay time
to 10% of intensity and a color of 555 nanometers.

The initial p7sim dot is essentially pure white while the decay time of the yellow-green phosphor
is much longer than 400 milliseconds.

T30dpy uses the correct power-law decay to give proper decay times, at least for the yellow-green phosphor.
The blue-purple phosphor also uses the proper decay time, but both p7sim and t30dpy really can't correctly model it.

Why? Given the 50 microsecond decay, no LCD monitor can reproduce that short an interval. Additionally, the initial dot
was displayed in 5 microseconds or less at an intensity no LCD can come close to, which is how it was visible.

The solution is to stretch the blue decay time considerably over several frames.

T30dpy runs at a fixed 30 frames per second and the alpha values for the colors are calibrated to that to give,
for the secondary phosphor, a 400 millisecond decay to 10%, as specified.
You will see that it decays signficantly faster than p7sim does.

Finally, the simulated beam spread done by p7sim is overly exaggerated.
Take a look at any real CRT with similar resolution, such as an oscilloscope, for comparison.

DEC stated a beam spot size of 0.030 inches, which works out to about 2 pixels on the original CRT.
T30dpy uses a simple rectangular pattern of pixels with more pixels added as intensity increases.
Given the scale, this is virtually imperceptible.

## Mike C mode?

Say what?

We all owe Mike Cheponis a debt of gratitiude.
He is responsible for making the last running PDP-1, now at the Computer History Museum in California, a reality.
Talking with him, he described the issues with the original P7sim Type 30 emulation (and the hardware front panel
driver).
Some of the issues are addressed as part of the core t30dpy functionality,
such as the simulated beam spread being far too much.

This mode is very specific and most people probably won't want to use it.
It completely disables simulated beam spread. A point is exactly one pixel in a 1024 by1024 screen, no imperfect
beam spread simulation, even though t30dpy does a better job at trying to simulate it.

However, if you want to see the spaceships in spacewar with the actual detail they were programmed with, give it a try.

## Implementation

T30dpy is a pure SDL2 implementation.
No GL libraries are needed.

It uses a streaming texture for composing the dots which is then passed to the standard renderer for display.

The decay time of a displayed point is based on time from when a point is initially displayed.
256 intensity steps are used occurring at 33.33 millsecond intervals, a frame rate of 30 frames per second.
Thus, the total aging span is up to 8.4 seconds.

A power law decay results in a long, low intensity tail and potentially that is simulated.
However, since LCDs can't duplicate the true analog intensity decay of a CRT, the intensity value drops below
what the LCD can display fairly rapidly. Still, the decay is apparent and the time is proper for the phosphor.

On startup, the rgba values needed by SDL at each step in the aging for both phosphors
following the proper power law decay is computed.
Additionally, a gamma correction is made to adjust for the human eye's nonlinear response to intensity.
Most modern displays also adjust gamma, so t30dpy's gamma can be ajusted or effectively turned off.

For each possible aging time which ranges from 0-255 and for each possible pdp-1 intensity which ranges
from 0-7, the rgba value for that combination is calculated and adjusted by the power law decay.

Alpha blending is then done of the two colors to get the displayed SDL RGBA8888 value.
The blended alpha has its value modified by the gamma correction factor and the point displayed.

Additionally, a white bias is applied to the blue phosphor r and g values to whiten the blue content.
The original display is almost white because of the mixing of the blue and the yellow-green colors from
the two phosphors, but not pure white.
The bias controls how much excess blue there is for the first displayed frame of a point.
After that, the normal blending suffices.

Increasing the bias makes increases the whiteness of the point.

The decay parameters are set so that the secondary phosphor drops to 10% of its initial intensity in
400 milliseconds.

The results are all computed at startup and kept in a 2 diminsional array,
*rgbaValues[pdp-1 intensity][lifetime step]*.
The main display loop can then get the proper rgba value for a point with no floating point calculations needed.

A table of point data 1024 x 1024 in size, to match the Type 30 resolution, is kept.
As points come from the host, the proper cell is marked as in use and its lifetime set to 0.
Each frame increments the lifetime after the rgba value is selected and if the 256 step limit has been reached
or the rgba value is 0, the point is marked as inactive and will no longer be drawn.

In practice, the primary value goes to 0 in a relatively few frames, the secondary trails off slowly
as you would expect.

Theoretically, the value never reaches 0, but since discrete steps are used, a small value of 1 or 2 can persist
for the entire aging span but not be visible.
A minimum value is used to end the lifetime to avoid this.
The termination value is configrable in the code, as are many other values.

See the source code for much more information, it is well commented and structured.

## Window size and pixel scaling

The texture that is drawn to is always 1024x1024, but the window that the texture is rendered to is whatever
size was specified, and in full screen mode, whatever size that is.

Unfortunately, SDL2 sometimes doesn't do a very good job of rendering pixels when the window size doesn't
match the texture size, it depends on the size and seems to be generally worse for screnn sizes lexx
that 1024x1024.
But, that isn't always the case.

The code uses nearest-neighbor texture scaling by default, but if that doesn't give good results for a given
screen size linear scaling can be used, which is actually bilinear scaling.
Bilinear also gives a smoother, softer appearance to dots when scaling occurs.

## White bias

Rather than a single rgba value for a displayed point, the blue and yellow/greem phosphors are both simulated
and then alpha-blended to get the displayed color.

However, the blending algorithm can overemphasize the blue color, so a bias factor is added to the r and g
values for the blue color.
This factor adjusts the blue color towards white as the bias increases and is internally limited to not
exceed the maximum value allowed of 255.

This is only applied to the point's first frame, then normbal blending is done.

If you want more blue, which while not authentic looks pretty nice, decrease the bias.
A bias of 0 will be a bright blue-purple, not realistic but pretty.
A bias near 255 will be pure white.

## Gamma

The human eye does not respond to intensity in a linear fashion.
However, the 8 intensity levels that are supported are internally a linear progression.
To make this match what the eye perceives as equal steps, a gamma correction is applied.
This is another power-law adjustment using a power exponent of 0.4545, the standard value.
Some modern monitors also do gamma corrections as do some display drivers,
so these might interact with the t30dpy setting.

The gamma that t30dpy uses can be changed via the command line or in the configuration file.
Values are generally 1.0 or less.
1.0 effectively removes any gamma correction, smaller values boost the intensity of dimmer points
giving a brighter display.
Values greater than 1.0 greatly diminish the intensity of dimmer points, and can cause the decay trails to disappear.

The actual adjustmwent is new = pow(original, gamma).
Note that many discussions of gamma use a value greater than 1, then use it as 1/gamma.
This is not done in t30dpy, a gamma of this form can be converted by setting the t30dpy gamma to 1/gamma.

Experiment to see what works for you and your display.

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

The figures for t30dpy are affected by various configuration settings, especially the lifetime of the yellow-green
phosphor.

T30dpy can also report some timing statistics.
Running type340stress, it is rendering over 5 million dots per second without any lost data.
Note that these are Type 30 dots, the number of pixels being rendered is over 4 times as many.

Tests have also been run on a Raspberry Pi 5 with similar results.\
On a Pi 4, performance differences are not as extreme.
This seems to be because the gpu on the Pi 4 is far less capable than that on the Pi 5
and the SDL library does not optimize its use as well.

## Settings that can be changed in code

These are in the source code, change with care, or just to play around.
The code default values are a good approximation of the original display.
Not all are listed here, just the ones that are useful for display appearance.

| Name             | Default     | Action                                                   |
|------------------|-------------|----------------------------------------------------------|
| BLUEDECAYALPHA   | 1.5         | determines the blue fade time, larger is faster |
| YELLOWDECAYALPHA | 0.85        | determines the yellow fade time |
| GAMMA            | 0.4545      | default, changes how much dim points are enhanced, smaller is more |
| BLUER,G,B        | 35,0,255    | the true r, g, and b values for the blue phosphor |
| YELLOWRGB        | 179,255,0   | the true rgb value for the yellow phosphor, a combined setting |
| WHITEBIAS        | 180         | the value added to the blue r and g values |
| LOWCUTOFF        | 5           | consider a point done when its intensity falls to this value\* |

## The configuration file

T30dpy supports a configuration file that can override some of the settings.
A search is made first for the file *~/.t30dpy.config*, then for */opt/pidp1-mods/t30dpy.config*.

The file settings override the compiled-in settings but any that have command line equvalents
are overridden if set on the command line.

If found, the following values can be changed.\
Lowercase change names are also command-line settable.

| Setting     | Changes     |
|-------------|-------------|
| hostname    | hostname    |
| port        | port        |
| vsync       | vsync       |
| linear      | linear      |
| gamma       | gamma       |
| whitebias   | whitebias  |
| cutoff      | LOWCUTOFF   |

A sample file is included as */opt/pidp1-mods/t30dpy.config.example* for reference.
