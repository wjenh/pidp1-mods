## Using the enhanced audio system for the pidp-1

The orginal has been highly modified so that quite good quality music is produced by Peter Samson's music program.

Per-channel digital filtering, adjustable gain, and adjustable sample rate has been added and the SDL2 audio
settings optimized.

The default values give excellent results, but if you want to experiment, continue reading.

Updated 24-Mar-2026

## What can go wrong

The biggest pitfall is adjusting filter and gain values such that the sample values sent to SDL exceed the
allowed range.
This causes clipping which is quite audible.
This can occur when modifying any of the paraneters including the sampling rate.

The sampling rate really doesn't need changing, but the filter and gain values can be set for individual preference.

## Monitoring

The **pdp1audio** program allows control over every aspect of the audio system, allowing you to query and set the gain,
sample rate, overall filter alpha (see below), or the alpha for each channel individually.

Doing *pdp1audio query* will show you current settings.
Type *pdp1audio* by itself to see what it can do.

The *pdp1audio overflow* command is essential for tuning.
If there are any overflows, then the gain is too high.

## What is alpha?

Alpha is a parameter that sets the frequency response of the digital filters.
It ranges from 0.0 to 1.0, increasing values move the frequency cutoff point higher.

A value of 0.0 is a cutoff frequency of 0, not useful.
A value of 1.0 is a cutoff frequency of infinity, no filter, again not terribly useful.

This interacts directly with the sample rate, it is suggested the sample rate remain as it is by default.

## What kind of filters does it use?

For those that want the details, the digitial filters are implemented as classic *IIR digital lowpass filters*.
There is plenty of documentation online about these.

All of the filter processing is done in 32 bit floating point.

## More details, please

The audio system uses the **SDL2** library, a widely-used way to get machine-independent audio processing.

The filter values are mapped from float to the *SDL2* audio setting *AUDIO_S16*, signed 16 bit integers.
Using integers gives completely acceptable quality with the least processor loading.

During each sample cycle, the timing of which is determined by the *rate* setting, the current value for each
channel is computed and sent to the **SDL_QueueAudio()** function, which eventually sends the audio out via
a USB port to a USB-to-audio conversion dongle.

It seems that the Raspberry Pi 5 generates a fair of low-level spurious signal noise,
power supply filtering is the likely culprit.
This noise is intrinsic, not a result of the audio implementation and is not excessive.

If you want the full details, look at *audio.c* and *lowpass.c* in /opt/pidp1-mods/src/blicolnlights/pdp1.

## Ok, finally, changing parameters

As noted above, *pdp1audio overflow* will be your best friend.

If you want more smoothing, decrease the alpha setting.
Adjust all channels at once using the *pdp1control alpha 0.xx* command until you have the overall sound you want.
You can then adjust the individual channels.

The Computer History Museum PDP-1 has analog low-pass filters, as was originally implemented.
They set channel 1 for a relatively low frequency response, 1 for a midrange response,
2 and 3 set for the least filtering.

However, different music uses channels differently, so an overall setting seems to work the best.

As alpha is adjusted, the sound quality can suddenly go very much downhill.
This is a clear sign that the gain needs adjusting, check for overflows and reduce the gain until they stop.
It is generally the case that the gain setting will be < 1.0.

You can hear the effect by changing the default gain of 0.95 to 1.1.

Finally, just have fun!
