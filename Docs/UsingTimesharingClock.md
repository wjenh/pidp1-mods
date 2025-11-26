# Using the BBN Timesharing Clock

This document explains how to use the BBN timesharing clock.

## What is it?

This is an implementation of the BBN timesharing clock that was only available on the PDP-1D, perhaps
only on one machine.

It provides a millisecond clock that can be read, as well as providing a 32 msec and 1 minute SBS interrupt.
If SBS16 is enabled, a different channel can be assigned to each.

There seem to have been multiple clocks of varying functionality available, this one is simple and useful.
The documentation for the original is very sparse, extra features have been added to make it more useful.

The original version used IOT 32, but apparently had no way to enable or disable it, nor to enable or disable
interrupts or specify the channels to use.
Calling it returned the current millisecond timer value in the IO register, the 32 ms and 1 minute events
were only via an interrupt on an unspecified channel.
The timer was limited to a count of 59999, 1 minute, then wrapped around.

The reason for 32 msecs and the 1 minute wraparound are not totally clear, but *BBN Report Number 1673* from 1968
which describes their hospitial time-sharing implementation definitely refers to the 32 ms clock.
It is perhaps because the system used the Type 23 Parallel Drum, which could do a full 4096 word transfer
in 35 ms to allow the system some setup time for the next swap.

This original call has been preserved and does just that, return the value of the 1ms timer.

However, some control funtionality has been added that is invoked by using IOT 2032.
It uses the AC register to control the clock.

Why the AC? Because all IOTs 30-37 automatically clear the IO register when invoked!

# The IOT

This is implemented via a single IOT, 32 as mentioned.
However, there are two subcommands.

IOT 32,clr, 720022, read 1ms clock
```
Return the current millisecond timer value in the IO register.
The value will range from 0-59999 decimal, 0-165137 octal, and wraps around.
```

IOT 2032, cls, 722022, set clock parameters
```
The AC register flags are:
000 000 seI iMM MMm mmm

bit 6 s enables the SBS16 system, once enabled it remains so regardless of this bit
bit 7 e enables the clock, disabling also clears the interrupt enables
bit 8 I enables the 1 min interrupt
bit 9 i enables the 32 ms interrupt
bits 10-13 MMMM  is the channel to use for the 1 min interrupt
bits 14-17 mmmm is the channel to use for the 32 ms interrupt
```

If the SBS16 system is not enabled, the interrupt channels are ignored and use the single channel provided
by the SBS system.

The mnemonics are in the macro include file CLOCK/clockdefs.mh for use with the m1pp preprocessor.
