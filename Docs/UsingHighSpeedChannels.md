# Using High Speed Channels

This document describes using the Type 19 High Speed Channel emulation.

This is version 2.4

Edit date 23-Sep-2026\
per-cycle priority, one word per stolen cycle, owed cycles for threaded mode, HSCsteal

## What is it?

The High Speed Channels were a hardware addition that allowed direct memory transfers by devices
such as the drums, mag tape drives, etc.
It could perform both a read and a write in one cycle and transfer up to 4096 words in one request.

It provided three independent channels that had priorites.
Channel 1 was the highest priority, 3 the lowest.
Priority was decided one memory cycle at a time: when two channels wanted the same cycle, the higher
priority one got it, and the other had the next free cycle.
A busy channel whose device had no word ready did not hold up the others.

All of the channels had priority higher than any other operation in the system, including sequence breaks.

It had no interface from the user side but rather was used by the hardware interfaces themselves.
It worked by 'cycle stealing', taking one 5us cycle to do a memory transfer for each word until it was done.
The processor lost that one cycle and otherwise carried on, so this was transparent to the user,
apart from the program running a little slower.
The breaks were held off while the processor was in the middle of a multiply or divide, and were taken
as soon as it finished.

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
There are six functions available:
- HSCallocateChannel
- HSCfreeChannel
- HSCexecute
- HSCwait
- HSCgetStatus
- HSCsteal

The original hardware dedicated a channel to a particular device, it was hard-wired.
This version is more flexible, mostly because there isn't any actual harware to wire to.

A channel is 'dedicated' by calling *HSCallocateChannel*. The channel is then dedicated to the caller until
it is freed or until the pidp1 emulator is restarted.
Requests on one channel must come from one thread at a time.

Once a channel is allocated, data is transferred to and/or from your 'device', an IOT generally, 
via the *HSCexecute* call, and completion waited for by *HSCwait*.

The current status of a transfer can be checked by calling *HSCgetStatus*.

There are four modes of operation, normal, *HSC_MODE_IMMEDIATE*, *HSC_MODE_THREADED*, and
*HSC_MODE_TRUESTEAL*. The last three are not standard, but are very useful for implementing device
emulations.

Every cycle a channel steals is one 5us cycle the processor does not run, in the emulator's own time,
so a program runs as much slower as it would have on the original.

The default, normal, mode implements pseudo-cycle-stealing. While a purist might argue that the real hardware caused
break states and set and cleared various internal bits of hardware, this isn't real hardware.
The apparent functionality is pretty much correct, and that's what is needed.
In this mode, the emulator checks every machine cycle to see if a transfer is needed and if so bypasses
execution of the upcoming machine cycle, instead moving one word.
The channel wants every cycle until its words are done, so it gets every cycle a higher priority channel
does not take.

Immediate mode completely bypasses the emulator, doesn't steal cycles, and completes immediately.
Of course, this is not at all like the original but it allows an IOT to implement its own timing or to not bother
with timing.
A device that uses immediate mode to fetch ahead, and then uses the words later, charges the cycles
with *HSCsteal* as it uses them; the Type 340's cache does this.

Threaded mode can actually be used inside or outside threads.
It runs in the current thread with no blocking waiting for the emulator.
It is a compromise between the original cycle-stealing mode and immediate mode.
The data moves at once, inside *HSCexecute*, and one cycle per word is scheduled in the processor.
The emulator pays the owed cycles one per machine cycle as if a regular transfer was done.
*HSCwait* enforces the 5us per word in the requesting thread, so the device sees the right delay.

The break states aren't the actual hardware break states the original -1 did, but they are a 'black box' equivalent,
and so as far as programs and IOTs are concerned, will appear to work the same way.

So, why have it?
If you are using threads, Linux scheduling can introduce some pretty big delays because of scheduling.
A transfer you might expect to complete in 30 microsecods coudl take milliseconds.
In threaded mode the device never waits for the emulator thread, nor the emulator for the device.

Truesteal mode is for a device whose native word rate is slower than the
5us memory cycle time, such as the Type 23 Drum's 8.5us/word.
It works out the total number of 5us cycles the transfer must occupy from the word count and the device's
word time, see *wordTime* below, and spreads one steal per word evenly across that whole span.
Each word moves to or from core at its own steal, as the original did, so the buffers are in use until
the transfer is done.
The span is counted in the emulator's own time, so a multiply or divide that runs across several
memory cycles delays the words that came due during it until it finishes, as on the original.
If the emulator falls so far behind that its throttle skips time, the words that came due in the
skipped time move without stealing, since the processor never ran those cycles.

The channel reports busy for its whole span, but it only wants the cycles its steals fall on,
so lower priority channels get the rest.

## Waiting for completion

For normal and TRUESTEAL modes, the drum operation must fully complete before the next operation on the
channel can be done.

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
- HSC_ABORT - a transfer was stopped by the front panel
- HSC_ERR - an error occurred

These modes can be passed to *HSCexecute*:
- HSC_MODE_FROMMEM - data is transferred from core memory to the caller's *from* buffer
- HSC_MODE_TOMEM - data is transferred to core memory from the caller's *to* buffer
- HSC_MODE_IMMEDIATE - the transfer completes immediately and steals nothing
- HSC_MODE_THREADED - the data transfer is immediate, one cycle per word is owed to the processor,
  and the transfer status honors the proper transfer time
- HSC_MODE_TRUESTEAL - one word per stolen cycle, spread over a device slower than memory speed
- HSC_MODE_UPDATEPANEL - a modifier for HSC_MODE_IMMEDIATE, HSC_MODE_THREADED, or HSC_MODE_TRUESTEAL
  to request hsc cycle light updating

The from and to modes can be used together or separately.\
Only one of immediate, threaded, or truesteal can be used.\
If none of immediate, threaded, or truesteal is specified, then normal mode is used.

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
For normal, threaded, and truesteal mode, the return will be one of HSC_ERR or HSC_BUSY.

IMMEDIATE will never steal cycles, the transfer completes when HSCexecute() returns.
It does not by default control the hsc cycle panel light.
If HSC_MODE_UPDATEPANEL is added, then the panel light will be lit by the high speed channel controller
for the number of cycles the transfer would have taken.
However, it might not synchronize with any timing the application code is doing.

The light is only ever changed by the emulator's own thread, once per machine cycle.
A request asks for it to stay lit for a number of cycles, at least 20 so that single-word transfers
can be seen; the emulator counts that down whatever the other channels are doing.
A stolen cycle is counted on the panel with the light in whatever state it is in.

Normal mode replicates the original behavior fairly closely, stealing cycles until the transfer is complete.
Note that the original processing time depended upon the external hardware, it drove read/write timing.
The best we can do is assume 5us per word.

The default is to transparently transfer one word every cycle, 5us.

Truesteal mode also transfers at a rate other than 5us per word, but set by the caller via
*wordTime* rather than assumed, and spread evenly across the transfer, see above.

If multiple channels want a cycle at the same time, the one with the lowest number gets it.
Another channel is not held up by a busy one that has no word ready on that cycle,
so a truesteal channel leaves the cycles between its steals to lower priority channels.

The possible status returns are:

```
- HSC_OK - returned in immediate mode if there was no error, the transfer is already complete
- HSC_ERR - returned in any mode for an invalid channel, mode combination, count, bank, or address;
  also returned for truesteal mode if wordTime is less than 50 (one memory cycle)
- HSC_BUSY - returned for normal, threaded, and truesteal to indicate the transfer is in process
- HSC_ABORT - returned when the pidp-1 is started or stopped, if an in-flight operation is in progress
```

It is **mandatory** to call *HSCwait* after a normal, threaded, or truesteal transfer, it won't wait if the status is done.
For threaded mode, it is *HSCwait* that finishes the transfer.

```
int HSCgetStatus(HSCChannelP chanP)
```

Returns the current status of the channel, one of the status codes above.

```
int HSCwait(HSCChannelP chanP)
```
If a transfer is in progress, wait for it to complete, otherwise return immediately.
The return will be one of the status codes above.
If it is returning after waiting, the stats will be HSC_DONE, or HSC_ABORT if the front panel stopped it.

If you want to avoid blocking, call *HSCgetStatus* to see if the status is HSC_DONE.
If so, then wait will not block.
Never wait on a busy normal or truesteal transfer from the emulator's own thread, an IOT handler or
iotPoll(): the transfer only advances on that thread, so the wait would never end.

Again, you must call *HSCwait* after a normal, threaded, or truesteal request, it doesn't hurt to call it after an
immediate request.

```
int HSCsteal(HSCChannelP chanP, int count)
```
Schedules *count* cycles to the processor without moving any data, as if *count* words had just been fetched
through the channel, and lights the hsc cycle light as a threaded fetch would.
It is for a device that fetched its words earlier with immediate mode and is using them now
to provide correct timing.
Returns HSC_OK, or HSC_ERR for a bad channel or a negative count.

## Behavior starting and stopping

If a transfer is in progress when the stop, start, continue, examine, or read-in switches are used, any in-flight
transfer is immediately stopped.
This is the same behavior as the original hardware.

A normal or truesteal transfer stops at the word it had reached; the words already moved stay where they are.
Cycles still pending are dropped.
When this occurs, the status returned from *HSCwait()* will be HSC_ABORT, including for a thread already
waiting in it.

It is good practice to pay attention to the return status.

## Final notes

The the buffers must be at least as large as the transfer count or expect crashes.
An **important** thing to remember is that the buffers must stay around for the duration of the transfer,
which for normal and truesteal mode means until *HSCwait*/*HSCgetStatus* reports done, since those modes
copy words to/from the buffers gradually, one per stolen cycle, as the transfer proceeds.
**DO NOT** use a local buffer within a function unless IMMEDIATE or THREADED mode is being used, since
those two copy all the data synchronously inside *HSCexecute* itself.
By the time it returns, the buffers are no longer touched,
even though the channel may still report HSC_BUSY for timing purposes, or again expect crashes.
