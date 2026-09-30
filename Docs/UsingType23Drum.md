# Using the Type 23 Parallel Drum

This document describes how to use the Type 23 paralell drum in your application.
The DEC document *H-23_parallelDrum_jul64.pdf* is a useful companion, although it does
have some significant errors in the IOT section.
This documentation is correct.

This is version 1.7
Edit date 29-Sep-2026
Every transfer now ends with a sequence break, and a halt no longer drops or corrupts a transfer.
Writes to the drum file no longer hold up the PDP-1, and a change another program makes to the
file is seen with the PDP-1 running, see The drum file.

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

Just like the original drum, this implementation uses a high speed channel to handle all transfers.
It faithfully reproduces the behavior and timing (more or less) of the original.

However, there is debate as to if the Type 23 drum was ever deployed on a PDP-1, but it seems quite likely
that it was used for the BBN timesharing system implementation and probably the MIT PDP-1X.

Where the doubt enters in is that the drum has a word transfer time of 8.5 microseconds per word, but the PDP-1
memory cycle time is 5 microseconds per word.
It is unclear how the mismatch was handled.

This directly leads to the simulated timing for small transfers being off, 5 us doesn't fit 8.5 us.
However, over a large transfer the error is very small.
For all cases, the timing is computed as (total words * 8.5) / 5 PDP-1 cycles.

## How do I use it?

The programming interface is remarkably simple and easy to use.

The three IOTs are used in sequence, and the transfer starts when the third is issued.\
Two of the IOTs have an additional oprating mode.\
All contol bits are passed in the IO register.

The <DRUM>/type23drumdefs.ah> include file defines the mnemonics used here.

**WARNING!** - there is overlap between mnemonics used by the drum and those used by the DSC2 Data Communications
System. If you use both, be careful.

-IOT 61, dia, 72xx61, drum initial address \
-IOT 2061, dba, 722061, drum break address

One of these is executed first.

IO register bits
```
bit 0, 1 to enable a read
bits 1-5, the drum field to read from, 0-37 octal
bits 6-17, the drum address to start reading from and/or writing to, 0-7777 octal
```
The define *drmrd* is 0400000, bit 0.

The difference between the two is that the latter will initiate a sequence break on channel 5
when the drum address specified is reached. A dwc and dcl should then be executed.

The DEC manual gives DBA, DWC, DCL as the sequence for a program that uses sequence breaks.
The dwc and dcl keep the break armed, and since the transfer starts at the same drum address,
the break arrives as the transfer starts.
The transfer then takes a second break when it ends, see Interrupts.
A *dba* on its own works too, with no transfer following it.
A *dia* issued before the break arrives cancels it, and another *dba* replaces it.

By the time the break is processed the drum will have gone a few words past the set address.
A program that wants the break ahead of a transfer can use an address purposely earlier than the real start
address, then issue a *dia* along with the remaining instructions for the actual transfer.

-IOT 62, dwc, 72xx62, drum word count

This is executed second.

IO register bits
```
bit 0, 1 to enable a write
bits 1-5, the drum field to write to, 0-37 octal
bits 6-17, the number of words to read from and/or write to the drum, 0-7777 octal
    A value of 0 means the full track, all 4096 words.
```
The define *drmwrt* is also 0400000, bit 0.

-IOT 63, dcl, 72xx63, drum core location

This is executed third.

IO register bits
```
bits 2-5, the core memory bank to read from and/or write to, 0-17 octal
bits 6-17, the address within the core memory back to begin from, 0-7777 octal
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
bits 6-17, the current drum address, 0-7777 octal
```
A parity error never occurs, this is a software drum, not a hardware drum.
Bits 0 and 2 are set to indicate a transfer error if the machine was halted in the middle of a
transfer (see Halts).
A failed write to the drum file does not set them, see The drum file.

## One nonstandard IOT

While it appears that channel 5 is the original interrupt channel, there is some evidence that it
was not always true, possibly because of local modifications, a hallmark of the PDP-1.
It is also the case that the PiDP-1 emulator does not enable SBS16, although it supports it.
A new IOT has been added to address this:

- IOT 2063, dss, 722063, drum set sbs

IOT register bits set on call:
```
bit 12, 1 to enable SBS16, 0 to disable
bit 13, 1 to change the assigned interrupt channel number
bits 14-17, the new interrupt channel to use
```
On return, the IO register will contain the prior channel number

The define *drms16* is 040, bit 12.\
The define *drmsch* is 020, bit 13.

## How do I know it's done?

If you aren't using the interrupt mode, the only indication is that
bit 17 in IO will be set if a cks, check status, instruction is executed.
With sequence breaks on, the transfer's end break tells you, see Interrupts.

The define *CKSDRM* is 01, bit 17.

Note that this information came directly from the DEC maindec drum verification program.

A convenience macro is provided, DRUM_WAIT_READY, see the example.

## Interrupts aka Sequence Breaks

Although the drum uses sbs channel 5 or whatever was set by *dss*, it can still be used in non-SBS16 mode.
In this case, the interrupt will be to the single channel, channel 0.

SBS16 can be enabled either here or via the pidp1.config file.

As on the original drum, every transfer ends with a sequence break on the drum's channel, whether it
finished or ended in error, and the busy bit is clear by then.
A *dba* adds its own break, so the DBA, DWC, DCL sequence takes two: the *dba*'s as the transfer
starts, then the end break.

With sequence breaks off, the end break waits and is taken as soon as they are turned on.
A program that runs its transfers with breaks off and then turns them on should issue a *cbs*
first, or it takes a break it wasn't expecting.

## Halts

The drum keeps turning while the PDP-1 is halted, but no word moves.
A halt is any of them: a *hlt*, the STOP switch, an ad1 stop or a single step.
Continue, Start and read-in all resume a transfer the same way.
- **Halted before the first word moved:** the transfer waits after the resume for the drum to come
  around to its starting address again, up to one revolution, about 35 ms.
  A full 4096-word transfer starts at once, as it always does.
  A *dba* break not yet taken also waits for its address.
- **Halted after that:** at the resume the transfer ends with its transfer error set (*dra* bits 0 and 2),
  busy clears and the end break is requested.
  Core, for a read, or the drum, for a write, holds only the words that moved before the halt.

So after read-in, a transfer still pending finishes into the newly loaded program's memory.
A power cycle does not reset the drum yet: a transfer pending at power-off still runs after power-on
and Start.

## The drum file

The drum's contents live in the file */opt/pidp1-mods/pdp23drum*.
The drum reads the whole file into memory the first time it is used, and transfers use that copy.
A separate thread writes the words a transfer writes to the file, so a slow disk, an SD card
say, never holds up the PDP-1.
Anything still waiting to be written when the emulator exits is written first.

You can change *pdp23drum* from outside the emulator while the PDP-1 runs: another program can
write it (ZMachine's *zloader* does), or you can copy a saved drum over it, or rename another file
onto its name.
The drum reads the file in again at the first transfer after the other program closes it.
A transfer made while the other program is still writing gets the drum as it was before, whole.
A change the drum reads in is reported on the emulator's standard error.

Make the change while no program is using the drum.
A transfer that writes while the change is being made may overwrite part of it, and the words the
drum wrote just before can still be on their way to the file for a moment after, up to a few
seconds on a busy SD card.
Changing the file while the PDP-1 is halted also works; the drum reads it in at the first
transfer after the halt.

A write to the file that fails is reported on the emulator's standard error, once until a write
succeeds again.
The transfer has finished by then, as it would have on the real drum, so *dra* does not
report it.

## An example

This code fragment shows the generic use, a read in this case:
```
    lac track            // set the drum track,field, and read mode
    sal 6s
    sal 6s
    ior field           // the drum field, the address in the track 0-4095 decimal
    ior [drmrd          // read mode
    lia
    dia                 // tell the drum the track/offset/mode
    lio count           // how many words to transfer
    dwc                 // tell the drum the count
    lio [buffer         // the buffer to read into
    dcl                 // do the transfer
    DRUM_WAIT_READY     // a macro that spins until the drum is ready
```

Writing is similar, setting the drum track in the dwc instead of the dia.
Note this uses the same drum offset as dia, it must be set there.

The purpose of this comes from the original use as a paging drum for timesharing.
Setting both read and write would write the memory to the drum on one track while simultaneously
reading into memory from the other track.

You can of course do just a read or just a write.
