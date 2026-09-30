## Using the pidp-1 configuration file

This document describes /opt/pidp1-mods/pidp1.config and how to use it.

This is version 1.2; it will be updated as needed.

Edit date 27-Sep-2026

## What is the configuration file?

It is a text file that allows runtime configuration of many aspects of the pidp-1 emulator as well
as various IOTs.

The config file has a simple format.\
Lines starting with '#' are comments and are ignored.\
Empty lines are ignored.\
Embedded spaces are ignored.\
Otherwise, a line of the form 'xxx=yyy' is expected.\
The meaning of 'yyy' depends upon the option.\
For an option that is on or off, 'y', 'yes', or 'on' means enable, anything else means disable.\
For a numeric option, it can be an integer or a floating-point number *nn.nn*.\
Anything else is a string option, the string following the *=* is its value.

Most options are built-in but new ones can be added for use in IOTs.\
If an option that is not built in is seen, it is added to a list of extra options that
can be accessed via functions in the .../src/blincolnlights/pdp1/configuration.c library.
An example can be found in the source code for the IOT_45 line printers.

- If it is a boolean setting, the onOff field in the extra option is set.
- If it is a string of digits 0-9 and minus signs, the ivalue field is set and the fvalue field will be NAN.
- If it also has a '.', the fvalue field is set and the ivalue field is set to the integer part.
- Otherwise, the setting is kept as a string and the strvalueP field set to it.

For any value type not seen, onOff is false, strvalueP is null, fvalue is NAN, ivalue is zero.
Use isnan() from math.h to test fvalue.

## Settings

These are the settings read by pidp-1, by the provided IOTs and by the programs that come with
them.
The sample configuration file, /opt/pidp1-mods/pidp1.config.example, explains each one and gives
its default.
Read it before changing a setting.

"Reader" is the program that reads the setting:
- pdp1: the emulator itself
- IOT_n: the IOT plugin for that device
- newpanel: the hardware panel driver
- p7sim: the standalone Type 30 display
- pdp1_periph: the unified control gui

Most settings are read when the emulator starts.
Sending the emulator a SIGHUP reloads the file: the emulator re-reads its settings, including the
throttle and audio ones, and each loaded plugin that has an update function re-reads its own.
A setting read by another program is applied when that program starts.

### The debugger server

| Setting | Reader | What it does |
|---|---|---|
| ad1port | pdp1 | The loopback port ad1 and fastload connect to; 0 or off disables it |
| ad1remoteport | pdp1 | A port on all interfaces as well, off by default; it has no authentication |

### The throttle

| Setting | Reader | What it does |
|---|---|---|
| throttleburst | pdp1 | How far, in microseconds, simulated time must lead the wall clock before the throttle waits; 100 by default, 0 to 250 |
| throttlequantum | pdp1 | The longest single wait, in microseconds |
| throttlespin | pdp1 | The end of each wait, in microseconds, spent spinning instead of sleeping |
| throttlemaxlag | pdp1 | How far, in milliseconds, the emulator may fall behind before the rest is forgiven |

### The CPU

| Setting | Reader | What it does |
|---|---|---|
| lailia | pdp1 | The PDP-1D lai and lia instructions, and lsw/swp |
| core1D | pdp1 | The core PDP-1D extensions |
| all1D | pdp1 | All the PDP-1D extensions |
| muldiv | pdp1 | Hardware multiply and divide |
| sbs16 | pdp1 | The 16-channel sequence break system |
| newmemfile | pdp1 | Save memory images in the newer, readable format |

### Displays and the lightpen

| Setting | Reader | What it does |
|---|---|---|
| sdb | IOT_7 | The Type 33 symbol generator's extended dpy |
| dpyshift | IOT_7 | The origin-shifting extended dpy; can conflict with sdb |
| twoscreens | IOT_7 | Dual Type 30 screens, untested |
| lightpen | IOT_7, IOT_27 | Lightpen support on the Type 30 and the Type 33 |
| motionPrediction | pdp1 | Predict the lightpen's motion |
| aperture | pdp1, IOT_7 | The lightpen's aperture, for the Type 340 and the Type 30 |
| two340charsets | Type 340 plugin | Lower-shift characters from a second character set |
| t340cachesize | Type 340 plugin | A Type 340 instruction cache, up to 1024; read only at startup |
| guilightpen | pdp1_periph | The lightpen in the unified control gui |
| type30lightpen | p7sim | The lightpen in the standalone Type 30 display |
| type30size | p7sim | The standalone Type 30 display's size |
| type30border | p7sim | The standalone Type 30 display's border |

### Audio

Read Docs/UsingAudio.md before changing these.

| Setting | Reader | What it does |
|---|---|---|
| audio | pdp1 | Allow audio; pdp1audio still turns it on |
| cutoff | pdp1 | The low-pass cutoff of all four program flag channels, in Hz |
| cutoff1, cutoff2, cutoff3, cutoff4 | pdp1 | The cutoff of one channel, overriding cutoff |
| gain | pdp1 | The mixer gain |
| tuning | pdp1 | Shifts the frequency; SDL3 only |
| samplerate | pdp1 | Samples a second sent to SDL |
| audiodepth | pdp1 | Milliseconds of sound kept queued ahead of the device |

### Devices

| Setting | Reader | What it does |
|---|---|---|
| fasttyo | IOT_3 | Typewriter output at 200 characters a second instead of 10 |
| lptType64 | IOT_45 | A Type 64 line printer instead of a Type 62 |
| lptLineSpacing | IOT_45 | Lines advanced for each spacing setting |
| lptLines | IOT_45 | Lines per page |
| lptNoFF | IOT_45 | Newlines instead of a form feed character |
| microtapesbs | Microtape plugin | The Type 550 microtape's sequence break channel; 2 by default |

### The hardware panel

| Setting | Reader | What it does |
|---|---|---|
| panelbrightness | newpanel | The brightness cap, 0 to 100 |
| panelonalpha, paneloffalpha | newpanel | The lamps' turn-on and turn-off rates |
| panelrealtime | newpanel | Realtime priority for the panel driver |
| panellevels | newpanel | Brightness levels, 8 to 64 |
| panelcycletime | newpanel | How often the lamp brightnesses are recomputed, in microseconds |
| panelscantime | newpanel | The time to scan one row of lamps, in microseconds |

### Timing statistics

| Setting | Reader | What it does |
|---|---|---|
| pidp1timing | pdp1 | Append the emulator's timing to /tmp/pidp1-timing.txt at each stop |
| displaytiming | pdp1, Type 340 plugin | Append the display threads' timing to /tmp/pidp1-340timing.txt and /tmp/pidp1-dpytiming.txt |

### No longer used

| Setting | What happens |
|---|---|
| shared | Ignored, with a message. ad1 and fastload reach the emulator over TCP |
| alpha, alpha1, alpha2, alpha3, alpha4 | Ignored, with a message. The audio filters are set by cutoff, in Hz |
