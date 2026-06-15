## Using newpanel

This docuemnt describes newpanel, a replacement for panel_pidp1, and how to use it.

This is version 1.0\
Edit date 15-Jun-2026

## Installing newpanel

Installation is integrated into the *install.sh* script.\
During installation you will be prompted for the version to use, the old panel_pidp1 or newpanel.\
Whicever is selected is installed as panel_pidp1 so existing scripts will still work.

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

You can now manually start the one of your choice by running it from the **build** directory,
/opt/pidp1-mods/src/blicolnlights/panel_pidp1.

If you don't see both panel_pidp1 and newpanel there, just type *make*.


## History

Newpanel is a new implementation of the pidp-1 hardware panel driver replacing panel_pidp1.
It uses much less cpu resources, replacing the floating-point operations of exponential decay done constantly
with much simpler logic involving only one floating point multiply and one add per light.
It also has a completely new method for the pidp1 emulator to update it which is far more efficient.

While the original is functional, the operation can be simplified considerably
because our eyes can do a lot of the processing for us and some of its simulation is unnecessary, not even
perceptible to the eye. Its implementation was far from optimal for cpu utilization.
It also suffers from a simulation error, described below.

The hardware panel uses modern LED lights, but the PDP-1 used incansecent lights.
Those took time to turn on and to turn off, and this needs to be simulated.
The exact turn on and turn off characteristcs of the specific lights that were used is unknown,
but some research gives figures for the same class of light as a turn-on time of a few milliseconds and
a turn-off time of about 50 milliseconds.

The default filter values used replicate this but can be adjusted for personal preference.

## General algorithm

The original pidp1 emulator and panel_pidp1 did the communication of light state from the emulator
to the driver backwards.
The emulator set a single bit in shared memory every cycle to indicate whether a bit should be on or off.
Panel_pidp1 then sampled the state of the light bits in a loop every 3 microseconds doing
several thousand iterations to build up an average on time.
But, this is silly. The emulator can easily do this with insignificant overhead.

Now, an additional set of values, one per light, has been added to the shared data.
Each one is an integer counter that is incremented by the emulator every cycle if its associated light
is on.
A cycle counter is also incremented to allow the panel driver to compute the fraction of time the light was
on between reads of the data. This eliminates the high-overhead loop in the original.

The values are then used to impement a pulse-width-modulator (pwm) for each light.
A display thread goes through the list of light values, applies a simple iir low-pass filter to the values
to provide the turn on / turn off characteristics, and updates the leds.

The result is a cpu load of only 20% on a Raspberry pi 5 and frequently no need to run with real time priority.

## Flicker, priority, and power settings

The human eye is very sensitive to flicker when the flicker rate is relatively low, on the order of 20-50 Hz, the
*flicker fusion rate*.
This varies by individual.

Panel_pidp1 and to a lesser extent newpanel are subject to this.
Panel_pidp1 forced use of high-priority threads to solve this.
Newpanel is much less subject to flicker, but depending upon what else is running on a device it can be
noticeable to some people.

Newpanel has use of elevated thread priority a configurable parameter and so can be adjusted as needed.

There is also an alternative solution, but one that is more invasive.
By default, the pi 5 and other pi variants do agressive pwoer management, dynamically dropping cpu speed frequently.
But, if it is in a low speed state and an application uses gpio, there is a significant ramp-up time to get the cpu
and gpio back to the levels needed.

This is one of the causes of the flicker issues.

By default, the cpu runs in *ondemand* mode, which is the agressive power-saving mode.
You can turn this off temporarily by giving the command:
```
echo "performance" | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```
and revert it by:
```
echo "ondemand" | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```
You can make it the default by editing the file */etc/rc.local* and adding the above commant at the end, before the *exit(0)*.

But a warning: this forces the cpu to run at maximum speed and so uses more power and could even require the use of a cpu fan, but it's an option.

## Alphas

Alphas determine the charactersistics of the low-pass filters used to control turnon and turnoff times.
These are the classical parameters for a digital iir low-pass filter.

Alpha values range from 0.0 to 1.0, with increasing values giving a shorter delay.
A value of 1.0 is no delay, 0.0 is infinite delay, not useful.

You can adjust the delays to your preference althogh the default values are the realistic ones.

The filters are the same as used in the new audio system for the pidp1.

## Configuration parameters

Newpanel can have various settings changed in the usual place, */opt/pidp1-mods/pidp1.config*.\
These settings are available:

| parameter | values | default | effect |
|-----------|--------|---------|--------|
| panelbrigtness | 0.0 to 1.0 | 1.0 | change the maximum brightness, 0.0 off to 1.0 brightest |
| panelrealtime | true, false | false | use or don't real time thread priority |
| panelonalpha | 0.0 - 1.0 | 0.45 | change the turn on time, larger numbers are shorter times |
| paneloffalpha | 0.0 - 1.0 | 0.04 | change the turn off time, larger numbers are shorter times |

