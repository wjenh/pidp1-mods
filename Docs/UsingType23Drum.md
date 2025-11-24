# Using the Type 23 Parallel Drum

This document describes how to use the Type 23 paralell drum in your application.
The DEC document *H-23_parallelDrum_jul64.pdf* is a useful companion, although it does
have some significant errors in the IOT section.
This documentation is correct.

## What is the Type 23 Parallel Drum?

The original drum was a 32 track add-on mass-memory unit for the PDP-1.
Each track contained 4096 words, the same size as a core memory bank.
It could do a simultaneous read and write of 1 to 4096 words into two different core memory banks at
a rate of one 18-bit word in each direction every 8.5 usecs, or completely save one bank and restore another
in approximately 35 milliseconds.

This replacement is implemented as a set of dyamic IOTs, IOT_61, 62, and 63 and requires no changes to the emulator
as long as it has the Dynamic IOT extension. Just copy the IOT61-63\.so files to the IOTs directory, that's it,
not even a restart is needed.

These IOTs duplicate the original behavior, even to the timing, and pass DEC's diagnostic, which is included.

## How does it work?

The programming interface is remarkably simple and easy to use.
The three IOTs are used in sequence, and the transfer starts when the third is issued.
Two of the IOTs have an additional oprating mode.
All contol bits are passed in the IO register.

-IOT 61, dia, 72xx61, drum initial address

-IOT 2061, dia, 722061, drum break address

One of these is executed first.

IO register bits
```
bit 0, 1 to enable a read
bits 1-5, the drum field to read from, 0-37 octal
bits 6-17, the drum address to start reading from and/or writing to, 0-7777 octal, 0-4095 decimal
```
The difference between the two is that the latter will initiate a sequence break on channel 5
when the drum address specified is reached. A dwc and dcl should then be executed.

-IOT 62, dwc, 72xx62, drum word count

This is executed second.

IO register bits
```
bit 0, 1 to enable a write
bits 1-5, the drum field to write to, 0-37 octal
bits 6-17, the number of words to read from and/or write to the drum, 0-7777 octal, 0-4095 decimal
    A value of 0 means the full track, all 4096 words.
```

-IOT 63, dcl, 72xx63, drum core location

This is executed third.

IO register bits
```
bits 2-5, the core memory bank to read from and/or write to, 0-17 octal, 0-15 decimal
bits 6-17, the address within the core memory back to begin from, 0-7777 octal, 0-4095 decimal
```
When this is executed the transfer begins asynchronously.

The timing is generally preserved, which means there will be an approximate 8.5us delay for each
drum word that must be passed until the requested drum address is reached, then an additional 8.5us for
each word transferred.

The transfer uses the High Speed Data Channel implementation, which must have been included in the emulator.
This is included by default in the modified distribution.

Also note that extended memory mode does **not** have to be enabled, the HSC bypasses that and always writes to
full 16-bit addresses.

-IOT 2062, dra, 722062, drum request address

IO register bits *returned*
```
bit 0, 1 if an error occurred
bit 1, 1 for a parity error
bit 2, 1 for transfer incomplete
bits 6-17, the current drum address, 0-7777 octal, 0-4095 decimal
```
An error will never occur, this is a software drum, not a hardware drum.

## How do I know it's done?

If you aren't using the interrupt mode, the only indication is that
bit 17 will be set if a cks, check status, instruction is executed.

## Interrupts aka Sequence Breaks

Although the drum uses sbs channel 5, it can still be used in non-SBS16 mode.
In this case, the interrupt will be to the single channel, channel 0.
However, in the distibuted modified system, SBS16 is enabled by default.
