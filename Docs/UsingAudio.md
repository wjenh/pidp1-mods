## Using the enhanced audio system for the pidp-1

The original has been highly modified so that Peter Samson's music program sounds the way the hardware made it.

Each of the four voices goes through its own low-pass filter, modeled on the music interface of the
Computer History Museum's PDP-1, and the gain and sample rate can be adjusted.

The default values reproduce that interface. If you want to experiment, continue reading.

Updated 26-Sep-2026

## What can go wrong

The biggest pitfall is setting the gain so that the sample values sent to SDL exceed the allowed range.
This causes clipping, which is quite audible.

The filters never go beyond the program flag's own swing, so with a gain of 1.0 or less nothing clips,
whatever the cutoffs.

## Monitoring and changing settings

The **pdp1audio** program controls the audio system while the emulator runs.
Type *pdp1audio* by itself to see what it can do.

| Command | What it does |
|---|---|
| `pdp1audio query` | shows the current settings |
| `pdp1audio overflow` | shows how many samples clipped since the last time you asked |
| `pdp1audio on`, `pdp1audio off` | turns the sound on or off |
| `pdp1audio cutoff 800` | sets all four filters' cutoff, in Hz |
| `pdp1audio cutoff1 1020` | sets one voice's cutoff, `cutoff1` to `cutoff4`; 0 puts back the interface's own |
| `pdp1audio gain 0.95` | sets the volume |
| `pdp1audio tuning 1.0` | shifts the pitch (SDL3 only) |
| `pdp1audio rate 48000` | sets the sample rate |
| `pdp1audio alpha 0.2` | the older filter setting, `alpha` or `alpha1` to `alpha4`; see below |

The *pdp1audio overflow* command is essential for setting the gain.
If there are any overflows, the gain is too high.

The config file keys for the filters are `cutoff` and `cutoff1` to `cutoff4`, in Hz.

## The music interface

The filters follow the museum's music interface, as drawn for its 2023 rebuild.

- Each program flag, PF1 to PF4, drives a 5K resistor into a capacitor to ground.
- A 20K pot sits across each capacitor, and its wiper is the output.
- PF1 and PF2 go to the left channel, PF3 and PF4 to the right.

The pot is across the capacitor so each capacitor charges through 5K in parallel with 20K, which is 4K.
The pot sets the level, not the cutoff.
That assumes whatever the wiper feeds has a high input impedance, and the emulator assumes it too.

| Flag | Capacitor (measured) | Cutoff |
|---|---|---|
| PF1 | 0.039 uF | 1020 Hz |
| PF2 | 0.043 uF | 925 Hz |
| PF3 | 0.094 uF | 423 Hz |
| PF4 | 0.181 uF | 220 Hz |

These are the defaults.
The cutoff is the -3 dB point of a first-order filter, 1 / (2 pi R C).

Voice 1 of the music program drives PF1, voice 2 PF2 and so on, so the fourth voice gets the heaviest
filtering.

The interface is DC-coupled, and its output runs from 0 up.
The emulator maps each flag to +1 or -1 instead.
Once anything downstream blocks DC, the two sound the same.

## How the filters work

Each filter is a classic infinite-impulse-response RC low-pass filter that is updated at each
5us emulator cycle.

The sample rate does not change the cutoffs, and no edge is moved to the next sample.
Samson's music program changes the pf flags every 175 microseconds or 5714 Hz.
The highest audio frequency that can be reproduced is then 2857 Hz, approximately F7 musically.

The filter processing is done in double-precision floating point.

Before 26-Sep-2026 the filters were per-sample digital filters set by an *alpha*.
An alpha gave the intended cutoff only at the sample rate it was worked out for.
Sampling the flags first also moved each edge by up to a sample.
At 22,000 samples a second that left an error 24 to 28 dB below the music, in its own frequency band.

The *alpha* commands of pdp1audio still work.
Each alpha is taken as the cutoff it gave at the current sample rate, and the reply says which cutoff that is.
The config file's *alpha* keys are no longer read; the emulator says so when it starts.

## More details, please

The audio system uses the **SDL** library, a widely-used way to get machine-independent audio processing.
It builds with SDL3 when that is installed, and with SDL2 otherwise.

The filter values are mapped from float to signed 16 bit integers, SDL's *S16* format.
Using integers gives completely acceptable quality with the least processor loading.

Samples are timed by the emulated machine's own clock, at the *rate* setting.
The default is 48,000 a second, the native rate of most USB audio devices, so SDL has nothing to convert.
Any rate works, and the cutoffs stay where they are.
Samples are handed to SDL in groups of 64 (about 1.3 ms at the default rate).
SDL eventually sends the audio out via a USB port to a USB-to-audio conversion dongle.

It seems that the Raspberry Pi 5 generates a fair amount of low-level spurious signal noise.
Power supply filtering is the likely culprit.
This noise is intrinsic, not a result of the audio implementation, and is not excessive.

If you want the full details, look at *audio.c* and *lowpass.c* in /opt/pidp1-mods/src/blincolnlights/pdp1.

## Keeping the sound with the lamps

The program flag lamps show the flags as the program sets them, and the sound is those same flags, filtered.
SDL plays from a queue, so the sound always trails the lamps by the amount queued.
The emulator keeps that amount near the *audiodepth* setting, 40 ms by default.
It fills the queue to that depth each time the machine starts or continues, and keeps it there however
long the music plays.
It also measures how fast the audio device really plays and matches it, since no two devices run at
exactly the same rate.

The depth is also a cushion.
If the emulator is held up for less than the depth, you won't hear it, because the device keeps playing
from the queue.
A longer hold-up gives a short gap, after which the sound is back with the lamps.
Don't set the depth below about 30 ms: the audio device takes its samples in chunks, commonly about 20 ms,
and a shallower queue keeps running dry.

A *tuning* other than 1.0 plays the sound faster or slower than the program runs, so the sound cannot stay
with the lamps; the emulator then only keeps the queue from growing past 250 ms.

With `pidp1timing=on` in the config file, the emulator writes one line a second about the queue to
`/tmp/pidp1-audiotiming.txt`.

## Ok, finally, changing the sound

For more smoothing, lower a cutoff; for a brighter sound, raise it.
Try all four at once with *pdp1audio cutoff*, then adjust the voices one at a time.
*pdp1audio cutoff1 0* (and so on) puts a voice back to the museum's value.

Different music uses the voices differently.
Earlier versions used one setting for all four voices, *alpha=0.20* at 22,000 samples a second.
That is *pdp1audio cutoff 781*, a somewhat more organ-like sound.

If you raise the gain, keep an eye on *pdp1audio overflow*.
You can hear clipping by changing the default gain of 0.95 to 1.1.

Finally, just have fun!
