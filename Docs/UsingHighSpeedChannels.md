# Using High Speed Channels

This document describes using the Type 19 High Speed Channel emulation.

This is version 2.3

Edit date 2-Sep-2026\
add HSC_MODE_TRUESTEAL

## What is it?

The High Speed Channels were a hardware addition that allowed direct memory transfers by devices
such as the drums, mag tape drives, etc.
It could perform both a read and a write in one cycle and transfer up to 4096 words in one request.

It provided three independent channels that had priorites.
Channel 1 was the highest priority, 3 the lowest.
What this meant was that if there were multiple channels active, all transfers from a higher priority channel
would complete before those of lower priority channels.

All of the channels had priority higher than any other operation in the system, including sequence breaks.

It had no interface from the user side but rather was used by the hardware interfaces themselves.
It worked by 'cycle stealing', taking one 5us cycle to do a memory transfer for each word until it was done.
This meant that the processor was effectively halted while the transfer took place,
but this was transparent to the user.

However, there was an additional piece of hardware, the High Speed Data Control, Type 131, that provided some
IOTs for allowing a user program to interact with some devices, at least one of the mag tape drives used it.
It is of very limited use and has not been implemented. Yet.
It depended upon the behavior of the device hard-wired to a channel to understand its directives.

## Why have this emulation?

For more realistic timing and also for convenience for anyone writing new IOTs that need to transfer to/from memory.
It hides the details of bank selection, presenting a full 16 bit address to the user.
An example is the IOT_61, 62, and 63 implementation of the Type 23 drum.
The real drum used the HSC for all of its transfers.
It was also used by the Type 340 display system.

## How do I use it?

Inside any IOT you implement, add:
```
#include "highSpeedChannels.h"
```

The IOT Makefile properly adds the path to it.
There are five functions available:
- HSCallocateChannel
- HSCfreeChannel
- HSCexecute
- HSCwait
- HSCgetStatus

The original hardware dedicated a channel to a particular device, it was hard-wired.
This version is more flexible, mostly because there isn't any actual harware to wire to.

A channel is 'dedicated' by calling *HSCallocateChannel*. The channel is then dedicated to the caller until
it is freed or until the pidp1 emulator is restarted.

Once a channel is allocated, data is transferred to and/or from your 'device', an IOT generally, 
via the *HSCexecute* call, and completion waited for by *HSCwait*.

The current status of a transfer can be checked by calling *HSCgetStatus*.

There are four modes of operation, normal, *HSC_MODE_IMMEDIATE*, *HSC_MODE_THREADED*, and
*HSC_MODE_TRUESTEAL*. The last three are not standard, but are very useful for implementing device
emulations.

The default, normal, mode implements pseudo-cycle-stealing. While a purist might argue that the real hardware caused
break states and set and cleared various internal bits of hardware, this isn't real hardware.
The apparent functionality is pretty much correct, and that's what is needed.
In this mode, the emulator checks every machine cycle to see if a transfer is needed and if so bypasses
execution of the upcoming machine cycle, instead delaying one cycle time.

Since high speed channels have priority over other operations, the cycle delay will repeat as long as there are
data words to transfer.

Immediate mode completely bypasses the emulator, doesn't steal cycles, and completes immediately.
Of course, this is not at all like the original but it allows an IOT to implement its own timing or to not bother
with timing.

Threaded mode can actually be used inside or outside threads.
It runs in the current thread with no blocking waiting for the emulator.
It is a compromise between the original cycle-stealing mode and immediate mode.
It bypasses the emulator but it enforces a completion delay that matches what the original cycle-stealing did
and also sets a counter the emulator will see that will cause it to execute a pseudo-break
for each word transferred.

The break states aren't the actual hardware break states the original -1 did, but they are a 'black box' equivalent,
and so as far as programs and IOTs are concerned, will appear to work the same way.

So, why have it?
If you are using threads, Linux scheduling can introduce some pretty big delays because of scheduling.
A transfer you might expect to complete in 30 microsecods coudl take milliseconds.
Of course, this can happen even in normal mode if the emulator gets rescheduled, which happens.
But, this is far less susceptible to that.

Truesteal mode is threaded mode's sibling for a device whose native word rate is slower than the
5us memory cycle time, such as the Type 23 Drum's 8.5us/word.
Like threaded, the actual data transfer happens immediately inside *HSCexecute*.
The difference is in how the pseudo-break count is spent.
Threaded steals every one of its cycles back to back, up front, and only lets
the emulator run normal cycles again once the whole word count has been satisfied.
For a device whose real transfer takes noticeably longer than 5us/word, that front-loads every cycle the CPU
is going to lose into one solid block at the very start instead of spreading it out the way the
real hardware did, one memory-cycle request per device word, not one continuous burst.

Truesetal mode fixes that.
It works out the total number of 5us cycles the transfer must occupy for
correct wall-clock timing from the word count and the device's word time, see
*wordTime* below, and spreads the needed steals evenly across that whole span instead
of bunching them at the start.
The emulator still sees the same kind of pseudo-break count threaded mode uses,
just paced out rather than delivered all at once so normal instruction execution and anything
serviced by a sequence break gets to run throughout the transfer instead of only after it finishes.

One consequence worth knowing, because the channel now has to report busy for its true total
duration, not just the steal-only portion, a TRUESTEAL channel excludes any lower-priority channel
for that whole duration too, same as normal or threaded mode would, just for longer than a
threaded transfer covering the same word count would have taken.
However, this is the correct original behavior.

## Waiting for completion

For normal and TRUESTEAL modes, the drum operation must fully complete before the next operation on the
channel can be done.
Additinally, normal mode will not have finished transferring the data before completion

An HSCexecute() while the channel is busy will fail.

Check the status with HSCstatus() for other than HSC_BUSY,
or explicitly wait with HSCwait().

## A note on memory and addressing

The hardware version had, and it is implemnted here also, access to all the memory on a system without having to
worry about extended mode.
So, be aware that the bank setting, below, is actually used and is important.

A memory transfer is limited to 4096 words in a call, and the address wraps around in the given bank.
The Type 23 Drum makes use of this, allowing a full track transfer starting at any location in a bank.

## The status codes, request codes, and the request structure

These statuses can be returned, they are defined in the include file:
- HSC_OK - a call completed successfully
- HSC_BUSY - a transfer is in progress
- HSC_DONE - a transfer is done
- HSC_ERR - an error occurred

These modes can be passed to *HSCexecute*:
- HSC_MODE_FROMMEM - data is transferred from core memory to the caller's *from* buffer
- HSC_MODE_TOMEM - data is transferred to core memory from the caller's *to* buffer
- HSC_MODE_IMMEDIATE - the transfer completes immediately
- HSC_MODE_THREADED - the data transfer is immediate but the transfer status honors the proper transfer time
- HSC_MODE_TRUESTEAL - like HSC_MODE_THREADED, but for a device slower than memory speed
- HSC_MODE_UPDATEPANEL - a modifier for HSC_MODE_IMMEDIATE, HSC_MODE_THREADED, or HSC_MODE_TRUESTEAL
  to request hsc cycle light updating

The from and to modes can be used together or separately.\
Only one of immediate, threaded, or paced can be used.\
If none of immediate, threaded, or paced is specified, then normal mode is used.

A transfer request is made via a HSCRequest. It has the following fields:
```
request.mode - the mode flags to use
request.count - the number of words to transfer
request.memBank - the bank number, 0-15
request.memAddr - the 12 bit address to begin from in the bank
request.fromBufferP - a pointer to space large enough to hold the requested words, integers
request.toBufferP - a pointer to space holding the words to write, integers
request.wordTime - HSC_MODE_TRUESTEAL only, the device's per-word transfer time in
  tenths of a microsecond, ignored for every other mode, and
  must be at least 50 (5us, one memory cycle) or HSCexecute will return HSC_ERR
```

HSC_MODE_UPDATEPANEL is used thusly and has no effect for other modes:
```
request.mode = HSC_MODE_IMMEDIATE | HSC_MODE_UPDATEPANEL;
```

The buffer pointers can be null if the corresponding mode is not used.

**IMPORTANT** - the buffers must be valid until completion of the transfer!
Don't use a local buffer and then return from its scope until the operation completes.
For safety, a static buffer is advised.

## The calls

```
HSCChannelP HSCallocateChannel(int channelNumber)
```
The returned channel pointer is how the channel is referenced.
Don't lose it, it's the only way to access and free a channel.

The number of channels is configurable in the source code, the default is 5.
Channel 1 is used by the Type 23 drum, channel 3 by the Type 340 display.

If the channel can't be assigned, a null pointer is returned.

```
int HSCexecute(HSCChannelP chanP, HSCRequestP requestP)
```
For normal, threaded, and paced mode, the return will be one of HSC_ERR or HSC_BUSY.

IMMEDIATE will never steal cycles, the transfer completes when HSCexecute() returns.
It does not by default control the hsc cycle panel light.
If HSC_MODE_UPDATEPANEL is added, then the panel light will be cycled by the high speed channel controller.
However, it might not synchronize with any timing the application code is doing.

It keeps a count based upon the number of words the IMMEDIATE transfer requested, turns on the light at
when the request is made, then decrements the count every 5 usec emulator cycle, turning it off when it reaches
zero. However, standard channel priority is enforced; if a higher priority channel is active, the count is not
updated until this channel has priority.

Normal mode replicates the original behavior fairly closely, stealing all cycles until the transfer is complete.
Note that the original processing time depended upon the external hardware, it drove read/write timing.
The best we can do is assume 5us per word.

The default is to transparently transfer one word every cycle, 5us.

Paced mode also transfers at a rate other than 5us per word, but set by the caller via
*wordPeriodTenthUs* rather than assumed, and spread evenly across the transfer instead of
consumed one cycle at a time from the start, see above.

If multiple channels have made requests, the one with the lowest number will complete, then the next-lowest, etc.
This applies to paced mode too: a paced channel is busy, and so excludes lower-priority channels,
for its whole real duration, not just its steal-only portion.

The possible status returns are:

```
- HSC_OK - returned in immediate mode if there was no error, the transfer is already complete
- HSC_ERR - returned in any mode for an invalid channel, mode combination, count, bank, or address;
  also returned for paced mode if wordPeriodTenthUs is less than 50 (one memory cycle) or count is 0
- HSC_BUSY - returned for normal, threaded, and paced to indicate the transfer is in process
- HSC_ABORT - returned when the pidp-1 is started or stopped, if an in-flight operation is in progress
```

It is **mandatory** to call *HSCwait* after a normal, threaded, or paced transfer, it won't wait if the status is done.

```
int HSCgetStatus(HSCChannelP chanP)
```

Returns the current status of the channel, one of the status codes above.

```
int HSCwait(HSCChannelP chanP)
```
If a transfer is in progress, wait for it to complete, otherwise return immediately.
The return will be one of the status codes above.
If it is returning after waiting, the stats will be HSC_DONE.

If you want to avoid blocking, call *HSCgetStatus* to see if the status is HSC_DONE.
If so, then wait will not block.

Again, you must call *HSCwait* after a normal, threaded, or paced request, it doesn't hurt to call it after an
immediate request.

## Behavior starting and stopping

If a transfer is in progress when the stop, start, continue, examine, or read-in switches are used, any in-flight
transfer is immediately stopped.
This is the same behavior as the original hardware.
When this occurs, the status returned from *HSCwait()* will be HSC_ABORT.

It is good practice to pay attention to the return status.

## Final notes

The the buffers must be at least as large as the transfer count or expect crashes.
An **important** thing to remember is that the buffers must stay around for the duration of the transfer,
which for normal mode means until *HSCwait*/*HSCgetStatus* reports done, since normal mode copies words to/from
the buffers gradually, one per cycle, as the transfer proceeds.
**DO NOT** use a local buffer within a function unless IMMEDIATE, THREADED, or TRUESTEAL mode is being used, since
those three copy all the data synchronously inside *HSCexecute* itself.
By the time it returns, the buffers are no longer touched,
even though the channel may still report HSC_BUSY for timing purposes, or again expect crashes.
