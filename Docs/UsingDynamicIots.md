# Using Dynamic Iots

This document describes the funcionality and now to create your own IOTs.

## What is a dynamic IOT?

A dynamic IOT is a compiled shared object that has a specific name and implements specific methods.
When a previously unknown IOT is executed by the pidp-1 emulator, a search is made for a matching dynamic IOT and
if found, the IOT is then handled by the dynamic IOT.
You can add a handler for any IOT that isn't built in by implementing one or two functions, see the sample
`IOT_57.c`.
No changes to the core emulator are needed to add a dynamic IOT, the binding is automatic at runtime.

## What does one look like?

All dynamic IOTS follow a specific naming format and must be in a specific directory.
IOTs are installed in the `opt/pidp1/IOTs` directory and must be named `IOT_nn.so`, where *nn* is the IOT number
**in octal** of the -1's IOT device code.

## How do I install one?

If you have written one in **C**, just put the source \.c file in the above directory and type `make`.<br>
You can see the required three includes in any of the examples, they are
```
#include "common.h"
#include "pdp1.h"
#include "iotHandler.h"
```
The Makefile properly adds the paths to those.

## How do I implement one?

At a minimum, implement the `iotHandler(PDP1 *hardware, int device, int pulse, int completion)` function.
This method is called twice for each time the corresponding IOT is executed.
Why twice? This emulates the way the original hardware worked.
The first call with the `pulse` argument being 1 mimics the first pulse that would have been sent
to an actual hardware implementation.
The second call with the `pulse` argument being 0 mimics the second pulse that would have been sent.
Technically, the first occurs at hardware subclock time TP7, the second at TP10.
The function *must* return a 1. If it returns 0, that causes the emulator to treat it as an unknown IOT.

It and several other functions are passed a PDP1 \* argument. This is a pointer to the entire state of the emulator.
It can be used to access various registers and even change the pc. But, use with care, don't set random things.

The typically useful members are:

- ac       the accumulator
- io       the IO register
- mb       the memory buffer, usually the last word read from memory
- pc       the program counter
- ss       the sense switches
- pf       the program flags
- sbs16    enable/disable the 16 chan break system
- cksflags additional flags to add to the word returned by the `cks` instruction
- simtime  the current simulator time in nanoseconds

These can be modified by your code, even the pc.
Adding 1 to the pc will cause the next instruction following your IOT call to be skipped, good for implementing
skip on some condition functionality.
There are a number of other members, but be very careful. You need a good knwolege of the internals of the enulator
to not cause random behavior or total failure.
Some functions are defined to help with some of the more useful ones, see below.

Note that the panel numbering of the sense switches and the program flags is reversed from the actual bit positions.
That is, flag or switch 6 is the lsb, 01, 1 is 040.

## Other common functions

If you have any special initialization to do before your IOT is called, implement `iotStart()`.
It will be called whenever the pidp1 goes into run state, or after your IOT is first loaded but
before iotHandler() is called.

If you have any special clenup to do, as the fclose() in the example IOT_57, implement `iotStop()`.
It will be called whenever the pidp1 is halted either by the `hlt` instruction or by the front panel switch.

## Interrupts aka Sequence Breaks

The -1 implements a simple interrupt system that your IOTs can use.
First, read the -1 documentation on the Sequence Break System.
A handler can reqest a sequence break by calling the builtin `initiateBreak(int channel)`.
This is typically called when pulse is 0.

If the 16 channel break system is installed, the channel numbers are 0-15.
The channel in question must have been enabled via the activate sequence break IOT `asb`, which has the format
`72nn51` where *nn* is the channel number 0-15 decimal, 0-17 octal,
Similarly, a channel can be disabled with the disable sequence break IOT `dsb`, 72nn50.

If the standard one channel sbs is installed, any channel number is ignored and will always be treated as 0.
IOTs asb, dsb are also ignored in this case.

For either system, sequence break must be enabled in general via the enter system break mode IOT, `esb`, 72xx55.
It can be disabled via the leave system break moode IOT, `lsb`, 72xx54.

## Special functions
If the IOT being inmplemented should be processed by another IOT, just implement the `iotAlias()`
function, which should return the IOT number of the handler to actually process it. Remember to keep your octal
vs decimal numbers correct.
That IOT will be loaded if it hasn't already been.
No other functions need to be implemented, they will be ignored.
Whenver your alias IOT is executed, the alias target code will actually be invoked.
It can tell what IOT device code caused the invocation by looking at the `dev` parameter.

If your code needs to be periodially activated, implement the `iotPoll(PDP1 *hardwareP)` function.
If polling is enabled in your IOT via `enablePolling()`, then the emulator will call
iotPoll() once every specified number of instruction  cycles at the end of TP7,
which is just after the IOT is processed.
See IOT_40 for an example.

The `enablePolling(int value)` method disables polling for your IOT if `value` is 0.
Otherwise, you will be polled every `value` instruction cycles.
One cycle is 5 microseconds, so the minimum granularity is that.
If you don't need to be polled as frequently, set a longer poll interval to reduce processor loading.

If an IOT is called in 'wait' mode, i and c, bits 5 and 6, are 1,0, then you **must** call `IOCOMPLETE()`
at some point, either in the IOT or in a poll. The same applies for 'need complete', bits are 0,1.
Until this is done, the emulator will continuously call your IOT until the complete is issued.

However, if your IOT does not support waiting, call `IOTNOWAIT()` before returning from your handler.
This should be done when the pulse argument is 1 to be effective.
The state of these 2 is passed in the `completion` argument to your handler.
if it is 1, then bits 5 and 6 are either 1,0 or 0,1.
This is consistent with the real PDP-1 behavior, a wait state does just that, waits before proceeding.

## Logging

A logging facility is provided:
```
#define DOLOGGING
#include Logger/iotLogger.h
```
within your code:
```
iotLog("format like printf", ....);
```
`iotCloseLog()` to close the log file can be called if you have an iotStop() implemented,
but it's not actually required. The output to the log file is flushed after each log call.
The debug output will be written to the file `/tmp/iot.dbg`.

If DOLOGGING is not defined then any log statements are dropped.
This allows debugging to be turned on and off.

## All functions and defines

- int iotHandler(PDP1 \*hardwareP, int device, int pulse, int completion)
- void iotStart(void)
- void iotStop(void)
- void enablePolling(int cycles)
- void iotPoll(PDP1 \*hardwareP)
- void initiateBreak(int chan)
- int iotIsAlias(void)

Defines

- DEVICE(PDP1 \*hardwareP)     extract the device number directly from the memory buffer register
- IOTNOWAIT(PDP1 \*hardwareP)  tell emulator to ignore the wait bits in the IOT instruction
- IOCOMPLETE(PDP1 \*hardwareP) tell the emulator the wait state is ended

## Final notes

The MACRO1 assembler only recognizes the 3 letter names for esm and lsm for any other than the basic IOTs.
You have to construct the instructions for asb and dsb and some others, typically with a `define` in the source.

As of this writing the 16 channel system is not enabled in the emulator, nor is there any way to do so
other than via a dyamic IOT.
IOT_60, included, is an example of how to enable/disable it.

IMPORTANT - once loaded, a handler stays loaded until the pidp1 emulator is shut down and restarted. So, if you change your handler, restart or it won't work properly.

Finally, remember that this is just an emulation of the hardware. There are no acutal electrical start or continue
pulses going to anything. So,the emulator will be blocked until your `iotHandler()` function returns.
If it takes too long, the following emulator cycle will be delayed.
This will always happen if an IOT instruction is called with a wait condition on until a completion is issued.
