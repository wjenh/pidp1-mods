## Using newpanel

This docuemnt describes newpanel, a replacement for panel_pidp1, and how to use it.

This is version 1.2\
Edit date 2-Jul-2026\
more flicker notes

## An important note about flicker and panel light blips

See the section below!

However, be sure you actually have a problem.

It could just be the running program's normal use of instructions.
The definitive way to tell is if lights that should be on constantly, such as the power light, flicker or
occasionally blink.

## Installing newpanel

Installation is integrated into the *install.sh* script.\
Newpanel is installed by default.

## Running from the command line

You can switch between old and new manually if you want to see the difference.

Start the emulator as usual via either autobooting or the pidp1-control icon.

To manually switch, find and kill the current panel:
```
ps -ea | grep panel
```
Find the *pid* for panel_pidp1, then:
```
sudo kill the-pid
```

Now start the one of your choice by running it from the **build** directory,
/opt/pidp1-mods/src/blicolnlights/panel_pidp1.

If you don't see both panel_pidp1 and newpanel there, just type *make*.

## History

Newpanel is a new implementation of the pidp-1 hardware panel driver replacing panel_pidp1.
It uses much less cpu resources, replacing the floating-point calculations of exponential decay done constantly
with much simpler logic involving only one floating point multiply and one add per light.
It also has a completely new method for the pidp1 emulator to update it which is far more efficient.

While the original is functional, the operation can be simplified considerably
because our eyes can do a lot of the processing for us and some of its simulation is unnecessary, not even
perceptible to the eye. Its implementation was far from optimal for cpu utilization.
It also suffers from a simulation error, described below.

The hardware panel uses modern LED lights, but the PDP-1 used incansecent lights.
Those took time to turn on and to turn off, and this needs to be simulated.
The exact turn on and turn off characteristcs of the original lights are discussed in the Alpah section below.

The filter values used can be adjusted for personal preference.

## General algorithm

The original pidp1 emulator and panel_pidp1 did the communication of light state from the emulator
to the driver backwards.
The emulator set a single bit in shared memory every cycle to indicate whether a bit should be on or off.
Panel_pidp1 then sampled the state of the light bits in a loop every 3 microseconds doing
several thousand iterations to build up an average over time.
But, this is silly. The emulator can easily do this with insignificant overhead and completely eliminate
the overhead in panel_pidp1.

Now, an additional set of values, one per light, has been added to the shared data.
Each one is an integer counter that is incremented by the emulator every cycle if its associated light
is on.
A cycle counter is also incremented to allow the panel driver to compute the fraction of time the light was
on between reads of the data. This eliminates the high-overhead loop in the original.

The values are then used to impement a pulse-width-modulator (pwm) for each light.
A display thread goes through the list of light values, applies a simple digital IIR first-order low-pass
filter to the values to provide the turn on / turn off characteristics, and updates the leds.

The result is a cpu load of only 20% on a Raspberry pi 5 and frequently no need to run with real time priority.

## The simulation error in panel_pidp1

The error is actually an error in the pdp-1 emulator itself.
The -1 has a number of 'microcycles' within the main 5 usec cycle.
During that time bits in various registers, especially the AC, can change.

The emulator samples the state of all the light bits at one random subcycle in the main cycle.

It seems the intent was to try to duplicate what the real lights would do, reflecting the microcycle changes
using a *Monte Carlo* style of statistical averaging.

BUT, sampling at random points ends up biasing the light updates way too much towards the microstate values,
smearing the light pattern.
You can think of it as stretching microstates to look like full 5usec states.
This is the cause of complaints about the unrealistic appearance of the panel lights.

An analysis determines that the microstates, which are fractions of a cycle and actually
contribute nothing to the real visible lights because they are of such a short interval,
end up having a signifant contribution to the light intensity.

With the old single-sample approach, that one random-subcycle sample was the displayed lamp state
so a rare 0.2us transient with a 4% chance of being sampled meant the lamp flickered fully on 4% of the time
and fully off 96% of the time.
A stark, visible binary flicker from something that should be imperceptible.

The new algorithm used completely avoids this because of its new sampling technique.

## Flicker, the Linux scheduler, and priority

The human eye is very sensitive to flicker and intermittent intensity changes, sensitivity varies by individual.

Unlike the original PDP-1, the panel driver has to live with the Linux scheduler which can suspend it at random
times causing intermittent, very brief, update blips.

Panel_pidp1 and to a lesser extent newpanel are subject to this.
Panel_pidp1 forced use of high-priority threads to solve this because of its high cpu demand.

Newpanel is much less subject to this, major work has been put into it to optimize performance around
scheduling interaction, but depending upon what else is running on a device it can be
noticeable to some people.
If you're running everything on the pi, pdp1, newpanel, t30dpy, all at once, you are much more likely
to see it.
If you connect the display remotely, you probably won't have an issue.

Newpanel has use of elevated thread priority, a configurable parameter, and so can be adjusted as needed.
While not usually needed, it has no significant impact on cpu loading, it just means it gets scheduled
when it needs it, not randomly.

So, use it if you have flicker issues!

## Alphas

Alphas determine the charactersistics of the low-pass filters used to control turnon and turnoff times.
These are the classical parameters for a digital IIR first-order low-pass filter.

Alpha values range from 0.0 to 1.0, with increasing values giving a shorter delay.
A value of 1.0 is no delay, 0.0 is infinite delay, not useful.

You can adjust the delays to your preference althogh the default values are the realistic ones.
If you want lights to turn on slower, make panelonealpha smaller.
Similarly, if you want them to turn off slower, make paneloffalpah smaller.

However, the slower you maake turn on and turn off, the more 'blurred' the display will be,
you are effectively averaging more and so missing shorter on/off changes.
THe default values are correct for the class of light bulb that was used, small incandescent in the 1 watt range,
but the precise performance of the bulbs used is not known.

The filters are the same as used in the new audio system for the pidp1.

The lights currently being used in the PDP-1 at the Computer History Museum are
TEC/CM 1762's, a T-1&1/4 28v 40 mA type C-2F filament bulb.

From the published information, the default alpha values in newpanel give on/off times that are a bit too fast.\
You can experiment, a good starting point is panelonalpha=0.040, paneloffalpha=0.018.

## Configuration parameters

Newpanel can have various settings changed in the usual place, */opt/pidp1-mods/pidp1.config*.\
These settings are available:

| parameter | values | default | effect |
|-----------|--------|---------|--------|
| panelbrigtness | 0.0 to 1.0 | 1.0 | change the maximum brightness, 0.0 off to 1.0 brightest |
| panelrealtime | true, false | false | use or don't real time thread priority |
| panelonalpha | 0.0 - 1.0 | 0.45 | change the turn on time, larger numbers are shorter times |
| paneloffalpha | 0.0 - 1.0 | 0.04 | change the turn off time, larger numbers are shorter times |

