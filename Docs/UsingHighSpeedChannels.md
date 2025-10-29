# Using High Speed Channels

This document describes using the Type 19 High Speed Channel Control emulation.

## What is it?

The HSCC was a hardware addition that allowed direct memory transfers by devices such as the drums, mag tape drives,
etc.
It could perform both a read and a write in one cycle.
It provided three independent channels that had priorites.
Channel 1 was the highest priority, 3 the lowest.
What this meant was that if there were multiple channels active, all transfers from a higher priority channel
would complete before those of lower priority channels.
It had no accessible interface from the user side but rather was used by the hardware interfaces themselves.
It worked by 'cycle stealing', taking one 5us cycle after every processor 5us cycle to do a memory transfer.
This meant that the basic instruction time went from 5us to 10us while transfers were taking place, but all was
transparent to the user.

## Why have this emulation?

For realistic timing and also for convenience for anyone writing new IOTs that need to transfer to/from memory.
It hides the details of bank selection, presenting a full 16 bit address to the user.
An example is the IOT_61, 62, and 63 implementation of the Type 23 drum.
It used the HSCC for all of its transfers.

## How do I use it?

Inside any IOT you implement, add:
```
#include "highSpeedChannels.h"
```
The Makefile properly adds the path to it.
There are two functions available:
```
int HSC_request_channel(
    PDP1 *pdp1P,        // emulator context
    int chan,           // channel,1-3, channel 1 being highest priority
    int mode,           // HSC_MODE_FROMMEM, _TOMEM, etc. (or'd together)
    int count,          // number of words to transfer, 0-4096
    int memBank,        // memory bank, 0-15
    int memAddr,        // address in bank, 0-4095
    Word *toBuffer,     // the user buffer to copy to memory, must be at least count size
    Word *fromBuffer);  // the user buffer to copy memory into, must be at least count size
```

and

```
int HSC_get_status(int chan);   // returns one of the HSC statuses
```
Both return an HSC_x status, see following.
The parameters should be pretty clear, except for mode.

There are 4 mode flags that are or'd:

```
- HSC_MODE_FROMMEM     read from memory into the fromBuffer
- HSC_MODE_TOMEM       write to memory from the toBuffer
- HSC_MODE_IMMEDIATE   don't do any timing simulation, the transfers happen within the call, zero-delay
- HSC_MODE_NOSTEAL     keep the 5us timing per word but don't affect the cycle time, just the busy duration
```

IMMEDIATE will never steal cycles, so NOSTEAL isn't necessary.
NOSTEAL is useful if whatever you're simulating takes more than 5us per word it wants to transfer.
An example is the Type 23 drum. Each word takes 8.5us to transfer, which is greater than the 5us cycle time.
So, there is no need to steal an additional 5us. In fact, that would make the timing wrong.

The status returns are:

```
- HSC_OK     returned from HSC_request_channel() to indicate the channel was available and allocated
- HSC_ERR    returned from HSC_request_channel() to indicate an argument was out of bounds
- HSC_BUSY   returned from HSC_request_channel() and HSC_get_status() to indicate the channel is in use
- HSC_DONE   returned from HSC_get_status() to indicate the transfer is complete
```

It is up to the user to check for HSC_DONE for any mode other than IMMEDIATE to determine when all words have
been transferred.

See IOT_61.c for an example of use.

## Final notes

The the buffers must be at least as large as the transfer count or expect crashes.
An **important** thing to remember is that the buffers must stay around for the duration of the transfer.
**DO NOT** use a local buffer within a function unless IMMEDIATE mode is being used or again expect crashes.
