# Using High Speed Channels

This document describes using the Type 19 High Speed Channel Control emulation.

This is version 2.0

Edit date 24-April-2026

## What is it?

The HSCC was a hardware addition that allowed direct memory transfers by devices such as the drums, mag tape drives,
etc.
It could perform both a read and a write in one cycle.
It provided three or more independent channels that had priorites.
Channel 1 was the highest priority, 3 the lowest.
What this meant was that if there were multiple channels active, all transfers from a higher priority channel
would complete before those of lower priority channels.
It had no accessible interface from the user side but rather was used by the hardware interfaces themselves.
It worked by 'cycle stealing', taking one 5us cycle to do a memory transfer for each word until it was done.
This meant that the processor was effectively halted while the transfer took place,
but this was transparent to the user.

## Why have this emulation?

For more realistic timing and also for convenience for anyone writing new IOTs that need to transfer to/from memory.
It hides the details of bank selection, presenting a full 16 bit address to the user.
An example is the IOT_61, 62, and 63 implementation of the Type 23 drum.
The real drum used the HSCC for all of its transfers.
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

There are three modes of operation, normal, *HSC_MODE_IMMEDIATE* and *HSC_MODE_THREADED*.
The last two are not standard, but are very useful for implementing device emulations.

The default, normal, mode implements pseudo cycle stealing. While a purist might argue that the real hardware caused
break states and set and cleared various internal bits of hardware, this isn't real hardware.
The apparent functionality is pretty much correct, and that's what is needed.

Immediate mode completely bypasses the emulator, doesn't steal cycles, and completes immediately.
Of course, this is not at all like the original but it allows an IOT to implement its own timing or to not bother
with timing.

Threaded mode can actually be used inside or outside threads.
It is a compromise between the original cycle-stealing mode and immediate mode.
It also bypasses the emulator but it enforces a completion delay that matches what the original cycle-stealing did.

So, why have it?
If you are using threads, Linux scheduling can introduce some pretty big delays because of scheduling.
A transfer you might expect to complete in 30 microsecods coudl take milliseconds.
Of course, this can happen even in normal mode if the emulator gets rescheduled, which happens.
But, this is far less susceptible to that.

## A note on memory and addressing

The hardware version had, and it is implemnted here also, access to all the memory on a system without having to
worry about extended mode.
So, be awaare that the bank setting, below, is actually used and is important.

A memory transfer is limited to 4096 words in a call, and the address wraps around in the given bank.
The Type 23 Drum makes use of this, allowing a full track transfer starting at any location in a bank.

## The status codes, request codes, and the request structure

These statuses can be returned, they are defined in the include file:
- HSC_OK - a call completed successfully
- HSC_BUSY - a transfer is in progress
- HSC_DONE - a transfer is done
- HSC_ERR - an error occurred

These modes can be passed to *HSCexecute*:
- HSC_MODE_FROMMEM - data is transferred from core memory to the caller's from buffer
- HSC_MODE_TOMEM - data is transferred to core memory to the caller's to buffer
- HSC_MODE_IMMEDIATE - the transfer completes immediately
- HSC_MODE_THREADED - the transfer completes after the proper per-word transfer time

The from and to modes can be used together or separately.\
Only one of immediate or threaded can be used.\
If neither immediate or threaded is specified, then normal mode is used.

A transfer request is made via a HSCRequest. It has the following fields:
```
request.mode - the mode flags to use
request.count - the number of words to transfer
request.bank - the bank number, 0-15
request.address - the 12 bit address to begin from in the bank
request.frombufferP - a pointer to space large enough to hold the requested words, integers
request.tobufferP - a pointer to space holding the words to write, integers
```

The buffer pointers can be null if the corresponding mode is not used.

**IMPORTANT** - the buffers must be valid until completion of the transfer! Don't use a local buffer and then
return from its scope until the operation completes. For safety, a static buffer is advised.

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
For normal and threaded mode, the return will be one of HSC_ERR or HSC_BUSY.

IMMEDIATE will never steal cycles, the transfer completes when HSCexecute() returns.

Normal mode replicates the original behavior fairly closely, stealing all cycles until the transfer is complete.
Note that the original processing time depended upon the external hardware, it drove read/write timing.
The best we can do is assume 5us per word.

The default is to transparently transfer one word every cycle, 5us.

If multiple channels have made requests, the one with the lowest number will complete, then the next-lowest, etc.

The possible status returns are:

```
- HSC_OK - returned in immediate mode if there was no error, the transfer is already complete
- HSC_ERR - returned in any mode for an invalid channel, mode combination, count, bank or address.
- HSC_BUSY   returned for normal and threaded to indicate the transfer is in process
```

It is **mandatory** to call *HSCwait* after a nornal or threaded transfer, it won't wait if the status is done.

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

Again, you must call *HSCwait* after a normal or threaded request, it doesn't hurt to call it after an immediate
request.

## Final notes

The the buffers must be at least as large as the transfer count or expect crashes.
An **important** thing to remember is that the buffers must stay around for the duration of the transfer.
**DO NOT** use a local buffer within a function unless IMMEDIATE mode is being used or again expect crashes.
