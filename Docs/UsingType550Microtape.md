# Using the Type 550 Microtape

This document describes how to use the emulated DEC Type 550 Microtape Control and its
Type 555 drives.

The DEC manual *550_prelimManual.pdf* (1963) is the companion; a program written to it
should run unchanged.
Where DEC's later documents fill its gaps or correct it, the emulation follows them:
*H-550, DECtape Control Unit 550*, the maintenance manual, 1965, Hantman's DECUS paper
*Microtape: Its Features and Applications* (1963), the F-03 brochure (1964), and DEC's
field-service memos.

This is version 1.3\
Edit date 13-Sep-2026\

Version 1.0, 10-Sep-2026: initial version\
Version 1.2, 13-Sep-2026: warning about halting during a write, the example stops the tape on an error\
Version 1.3, 13-Sep-2026: matched to DEC's later documents: a halt stops the tapes, any error stops
writing, the block mark and the deadlines are DEC's, the end-of-block latch; `mse` rereads
*microtapes.txt*; the *mtp* tool added.

## What is the Type 550 Microtape?

The Microtape was DEC's small block-addressed magnetic tape, the ancestor of DECtape.
A 260 foot reel holds 576 blocks (0-1077 octal) of 256 18-bit words, about 147,000 words.
Every block carries its own number, so a program can go straight to a block, read it and
rewrite it in place, in either direction, without disturbing its neighbors.

The Type 550 control handles up to eight drives, four dual Type 555 transports.
It moves one word at a time between the tape and a one-word buffer, and raises a flag for
the program each time.
The program must answer each flag within one word time, 200 microseconds.
The control does no memory transfers of its own; the program must move every word.

The tape's IOTs share device 01 with the paper tape reader's `rpa`, and are told apart from
it by the sub-device field of the instruction, as the real machine's decoding did.

The timing is DEC's, a word every 200 usec, 52.8 ms per block, 200 ms to
start, 150 ms to stop and 300 ms to reverse.

## What you need

- The image tool *mkmicrotape*, only needed to check images and to convert simh tapes.
- A line in */opt/pidp1-mods/microtapes.txt* for each drive that should have a tape.
The tool */usr/local/bin/mtp* changes a drive's line for you.
A program can also mount tapes itself via the *mmt* IOT, below.

## Setting up

List the drive to file mapping in */opt/pidp1-mods/microtapes.txt*, one line per drive.
```
# drive  image
1 microtapes/tape1.img
2 /home/pi/tapes/system.img,locked
```
- A line is the drive number, 1-8 in *decimal*, drive 8 is 010 in the `mse` field, then
  spaces, then the image's path, which runs to the end of the line and may contain spaces.
  Add `,locked` to write-lock the drive. It can then only be read, not written, and mode 7 is
  refused. An image file that is read-only is also mounted locked.
- A path that does not start with `/` is relative to */opt/pidp1-mods*.
- Blank lines and lines starting with `#` are ignored. A drive with no line has no tape.
  With no *microtapes.txt* at all, no drive has a tape.
- A file that is not a tape image is created as a blank tape when first used unless the line specifies locked.
  If specified as locked but it is not a tape image, it will give a read error if used.

In *pidp1.config*, `microtapesbs` is the sequence break channel, 0-15, to use if sbs is enabled,
the default is 2.
```
microtapesbs=2
```
To change tapes while the emulator runs, edit *microtapes.txt*, or let *mtp* do it:
```
bin/mtp 1 microtapes/tape2.img
bin/mtp 2 /home/pi/tapes/system.img,locked
bin/mtp -f /some/other/list.txt 3 scratch.img
```
*mtp* replaces the drive's line or adds one.
The image name is written into the list as given, so a relative name is relative to
*/opt/pidp1-mods* here too.
`-f` names a list other than */opt/pidp1-mods/microtapes.txt*.

The change takes effect at the program's next `mse`.
An unchanged file changes nothing, so a program's `mse` never rewinds a tape or undoes its `mmt`.
To make a change take effect at once instead, reload the configuration with
`bin/pdp1control.sh reload`, which applies the list whether or not it changed.

Either way, only drives whose line changed are remounted, plus any drive whose tape ran off its
reel, and any drive whose line could not be mounted last time.\
A drive whose line did not change is not disturbed, even if it is running.\
A file rewritten with the same lines changes nothing, but it does retry a line that failed,
for instance, a locked line for an image that has since been made.\
A drive whose tape a program chose with `mmt` keeps it until its line in *microtapes.txt* is changed.\
A freshly mounted tape is stopped at the load point, just before the end zone at the start of the tape.\

## How does it work?

Within the tape, each block is 264 word spaces, as DEC's figures show them, counted forward:

| Space | Mark | What it is |
|---|---|---|
| 0 | M | block mark, where search raises its flag |
| 1 | -G | reverse guard |
| 2 | L | lock: writing, the control loads -0 here for the leading checksum |
| 3 | -C | leading checksum |
| 4-259 | | the 256 data words |
| 260 | C | trailing checksum |
| 261 | -L | reverse lock |
| 262 | G | guard |
| 263 | -M | reverse block mark, where a reverse search raises its flag |

The mark track, which the emulation keeps, tells the control what each space is.
The control senses the block mark, the checksums, and the data words as they pass the virtual head
in either direction.

The checksum rule makes every block total zero.\
When a block is written the control puts -0 (777777) in the leading checksum space itself,
and the program writes the trailing checksum, which DEC defines as
the complement of the 1's-complement sum of the leading checksum (-0) and the 256 data words.
Adding -0 changes a 1's-complement sum only when the data words sum to +0, and the block totals
zero either way.
Read back, the 258 words, both checksums included, add up to zero.
Note that 1's complement has two zeros.
The PDP-1 `add` turns a -0 result into +0, so a
running total kept with `add` ends at +0 for a good block.

Beyond the last block at each end of the tape are 10 feet of end zone and then the leader.
A tape in any mode that runs into the end zone ahead of it is stopped with an error.

## The IOTs

The *<MICROTAPE/type550defs.ah>* include file defines the mnemonics used here.

All six IOTs act immediately and do not need nor support the `i` or the `C` bit.
All pass their argument in the IO register.

Note that `mmt` is a new convenience feature allowing a program to mount a tape itself.

-IOT 201, mmt, 720201, mount a tape

IO -
```
bits 2-5, the drive, as for mse (mtu1 to mtu8); the other bits are ignored
```
AC -
```
bits 2-17, the address of the image's file name; 0 unmounts the drive
```
The name is packed ascii as the am1 `ascii` directive generates, two characters to a word,
the first in the high 9 bits, ending with a character whose value is octal 0, a null byte as in C.
It must end in the same memory bank it starts in; the zero may be in the bank's last word.
A name that does not start with `/` is relative to */opt/pidp1-mods*, the same as in *microtapes.txt*.

The tape replaces whatever the drive held, unlocked, stopped at the load point.
A missing image is created as a blank tape.
The drive's line in *microtapes.txt*, if it has one, no longer applies unless it is changed or
the drive is unmounted.

IO returns:
```
0       mtmok    mounted, or unmounted, for AC = 0
1       mtmlck   mounted write-locked, the image file is read-only
777776  mtmerr   failed
```
It fails for a drive number outside 1-8, changing nothing, otherwise leaving the
drive with no tape for a name that is empty, holds a character outside 040-176,
has no end in its bank or is too long, for a file that is not a tape image or that is already on
another drive, or for a file that cannot be created.

```
    lio [mtu3]
    law name
    mmt                     // IO = mtmok, mtmlck or mtmerr
    ...
name,
    ascii "microtapes/scratch.img"
```

-IOT 301, mse, 720301, select

IO -
```
bits 2-5, the drive, 1-7, or 10 for drive 8; 0 or 11-17 select no drive
```
The defines *mtu1* to *mtu8* are the drive numbers already in place.

Clears the data, block end and error flags, and with them END, MISS, MTE and UNABLE.
Writing stays off after an error until the next `mlc` in write mode.\
Selecting a different drive starts a 34 ms selection delay during which no flags are asserted.
DEC's field-service memo of June 1964 gives the same 34 ms, as something the program must allow
for: when changing drives, deselect the last one (select drive 0) for 34 ms.\
The drive that was selected keeps moving as it was told, but is no longer visible.
If left running, it will run off its reel.

Before selecting, `mse` rereads */opt/pidp1-mods/microtapes.txt* and, if it has changed, brings
the drives in line with it; see Setting up.

-IOT 401, mlc, 720401, load control

IO -
```
bit 12, go: 1 = move, 0 = stop
bit 13, reverse
bits 15-17, the mode: 0 move, 1 search, 2 read, 3 write, 7 erase, 4-6 act as move
```
The defines are *mtgo* 040, *mtrev* 020, and the modes *mtmove*, *mtsrch*, *mtread*,
*mtwrit* and *mtfmt*.

Clears the same flags as `mse`.\
From rest the tape takes 200 ms to reach speed, and a change of direction takes 300 ms.\
No flags are asserted until the tape is at speed.\
Stopping takes 150 ms.\
Changing mode while the tape is at speed takes effect at once, except at the end of a block
being written: an `mlc` given after the flag for the last data word, before the trailing
checksum is on the tape, waits until the checksum has been written.
This is DEC's D256 latch; see Write, below.\
Only an `mlc` with go and write mode turns the writers on.
Any other `mlc`, a halt and any error turn them off.

Mode 7 erases the whole reel at once, back to a blank initialized tape with every block numbered,
every block reading without error, its data words zero.
Then it acts as move, raising no flags.

For the original, control mode 7 wrote the mark track with a switch on the control panel to allow it.
The function here is the same, formatting a tape.

The control refuses the command, setting UNABLE and the error flag and changing nothing
if no drive is selected, the drive has no tape, its tape ran off the reel, or the mode is
write or erase and the drive is locked.

-IOT 501, mrd, 720501, read

Puts the buffer in IO and clears the data and block end flags.

-IOT 601, mwr, 720601, write

Puts IO in the buffer and clears the data and block end flags.

-IOT 701, mrs, 720701, read status

Puts the status in IO and clears nothing.

IO returns:
```
bit 0, DF, the data flag                        mtdf   400000
bit 1, BEF, the block end flag                  mtbef  200000
bit 2, ERF, the error flag: END, MISS or UNABLE mterf  100000
bit 3, END, the tape ran into the end zone      mtend  040000
bit 4, MISS, a flag came before the last one was answered
                                                mtmiss 020000
bit 5, reverse was commanded                    mtrevs 010000
bit 6, go was commanded; END and a halt clear it
                                                mtgos  004000
bit 7, MTE, a mark track error; never set       mtmte  002000
bit 8, UNABLE, the last mlc was refused         mtunab 001000
bits 9-17 are 0
```

## Searching, reading and writing a block

The times below are for a tape at speed, moving forward.
Moving in reverse is the mirror image.

**Search** raises the data flag once per block, 52.8 ms apart, as the block mark passes.
The buffer then holds the mark code and the block number, 260000 plus the block moving forward,
450000 plus the block in reverse.
The define *mtblk* masks the block number.
To read or write that block, change to read or write mode within the allowed timing window, see Deadlines.

**Read** raises the data flag 600 us after the block mark's flag with the leading checksum, then
256 more data flags 200 us apart for each data word, then for block end flag with the
trailing checksum.
Take each word with `mrd` within the time between the reads.

When read in reverse a block gives exactly the words that were written, last word first.
Note that this is the manual's rule, simh's microtape doesn't work this way.

**Write** raises the data flag at once when it is commanded, asking for the first data word.
Answer every data flag with `mwr` and the next word; each flag asks for a word two word
times before it is written.
After the 256th word the block end flag is raised, answer it with `mwr` and the trailing checksum.

The checksum reaches the tape 200 us after the block end flag.
An `mlc` given from the flag for the last data word until then is held, and carried out as the
checksum is written (DEC's D256 latch), so the program can search or stop right after the
checksum's `mwr`.
Only the last `mlc` given in that time is carried out, and an `mse` to another drive carries it
out at once, before the checksum is written.
Written in reverse, the words go on the tape last word first, and the two checksums change
places, the block still totals zero.

Staying in read or write mode will continue on into the next block, its first data flag comes
1.4 ms (read) or 1.6 ms (write) after the block end flag.

**Move** raises no flags.
It is for positioning to the other end of the tape without having to process intermediate words.

Changing from read to write inside a block loses two word spaces, the first word written
lands in the third space after the last one read.

## Deadlines

The program has one word time, 200 us, to answer every data flag and block end flag.\
The other deadlines count from a flag to the `mlc` that changes the mode.
Every window is DEC's figure (the DECUS paper's Figs. 4 and 5 and the brochure).

| From | To | Emulation | DEC |
|---|---|---|---|
| every data or block end flag | `mrd` or `mwr` | 200 us | 200 us |
| search flag | write, the `mlc` | 400 us | 400 us |
| search flag | write, the first `mwr` | 600 us | 600 us |
| search flag | read, to get the leading checksum | 600 us | 600 us |
| read block end flag | search, to see the next block | 800 us | 800 us |
| read block end flag | write the next block | 1.2 ms | 1.2 ms |
| write block end flag | search, to see the next block | 1.2 ms | 1.2 ms |

The 600 us for reading after a search is the time DEC allows to change from search to read.
It ends where the first data flag says the leading checksum has been read.
A read commanded later starts with the first data word; a block written forward has -0 as its
leading checksum, so its total still comes out zero, but a program should not count on that.

A write commanded after 400 us misses the lock: the leading checksum space keeps what was on
the tape, and the block fails its check.

A search flag need only be answered before the next one, 52.8 ms later.

## Errors

The error flag is raised with a sequence break for any of these and the matching bit is
set in the status.
Only `mse` and `mlc` clear them.

- **MISS** -- a data flag or block end flag came due while the last one was still up.
  The new flag is raised anyway, and the tape keeps going.
  Reading, the word that wasn't taken is lost.
  Writing, the control stops writing at the missed word: that space and the rest of the block
  keep what was on the tape, and the block fails its check.
- **END** -- the tape ran into the end zone in any mode. The tape is stopped
  and GO is cleared. Move the other way, reversing out of an end zone is not an error.
- **UNABLE** -- the last `mlc` was refused because of no drive, no tape, a tape off the reel, or
  write or erase on a locked drive.

MTE, the mark track error is never set, emulator tapes are always perfect.

Every error switches the writers off, as DEC's control did (H-550: the error conditions hold
WRITE ENABLE at 0).
Writing resumes only when the error has been cleared and write mode is commanded again with an
`mlc`; an `mse` alone clears the error but writes nothing.
So a program that falls behind spoils at most the block it was writing, never the ones after it.

A drive that is deselected while it moves is not watched, so no end zone stops it.
It runs to the end of the tape and off the reel, and then it is unusable (UNABLE) until
its tape is remounted: by `mmt`, by a configuration reload, or by any change to
*microtapes.txt* that an `mse` then reads (rewriting the drive's line with *mtp* will do).
The image file is not harmed.

## Halting the processor

When the processor halts, every moving drive stops, deselected ones included, and GO is
cleared.
This is DEC's "All Halt" (H-550): the drives' go relays were held only while the computer's
RUN flip-flop was 1.
A `hlt`, a breakpoint, the stop switch and ad1's `stop` all do it.

- The writers go off with it. A halt during a write spoils only the block being written: it
  keeps the words written before the halt and fails its check. The blocks after it are not
  touched.
- The mode, reverse and the flags are left as they were.
- When the program runs again the tape stays stopped until it gives an `mlc` with go.
  A program that was waiting for a flag will wait forever, so a wait loop should also watch
  GO (`mtgos`) or the error flag, or the program should restart the transfer after a halt:
  search back to the block and write it again.

**Single-step does not stop the tape.**
On the real machine each step dropped RUN, and so stopped the tapes.
The emulator sees too few of single-step's short RUN pulses, and a tape started by a stepped
`mlc` keeps moving.
Stop the tape (`cli` `mlc`) before stepping through tape code, or use a breakpoint.

## Sequence breaks

Every raise of the data flag, block end flag, or error flag requests a sequence break
on channel 2, or on the channel set by `microtapesbs` in the pidp1.config file.
Without SBS16, the break is to the single channel, channel 0.
A program that polls with `mrs`, as the example does, can leave the sequence break system off.

## The image file

A tape is 576 blocks of 258 words: the leading checksum, the 256 data words and the
trailing checksum.
Each 18-bit word is in a 32-bit container like the pdp23drum file, so a full
image is 594,432 bytes.
Block b's word k is at byte (b x 258 + k) x 4.

An image file need not be full, it holds blocks 0 up to the last block written and every block after
that is blank, as if it had just been formatted.

An empty file is a blank tape, and a file grows only as far as the highest block written.
When a block beyond the end of the file is written, the blank blocks before it are
written too, a file never has holes in it.
Mode 7 truncates the file.

A file must be a whole number of 1032-byte blocks, and no more than 576 of them.

Written blocks go to the file as soon as the block is finished, or when the mode changes,
or when the emulator stops.

*mkmicrotape* makes, checks and converts images:
```
mkmicrotape [-f] blank  <image>                  a blank tape: an empty file
mkmicrotape      check  <image>                  list the blocks that do not total zero
mkmicrotape [-f] import <simh-18b-file> <image>  from a simh tape
mkmicrotape [-f] export <image> <simh-18b-file>  to a simh tape
```
It won't overwrite a file unless given `-f`.
The exit status is 0 for success, 1 if `check` found bad blocks, and 2 for an error.
You don't need it to make a tape, a missing image named in *microtapes.txt* or by `mmt` is
automatically created for you.

A simh PDP-1 microtape file (18b format) is 578 blocks of 256 data words with no checksums.
Import computes the checksums as if each block had been written forward, and warns if it
has to drop anything in blocks 576-577, which this tape does not have.
Blank blocks at the end of the tape are left off the image.

Export writes the data words and fills blocks 576-577 with zeros.

## An example

This program writes block 12 of drive 1 with the words 1 to 400, searches back to it in
reverse, reads it backward, and checks the total.

It polls the status with `mrs`.
The data loops look at the data flag first, which `spi` tests straight from IO, so each
word takes 40-60 us of the 200 us allowed.

It looks at the error flag only when no flag is up, and once more at the end of the
block, the error flag stays up until the next `mse` or `mlc`, so a MISS cannot slip by.
The checksum is worked out before the write starts, and the total after the read ends.
On an error it saves the status and stops the tape before it halts.
The halt would stop the tape too (see Halting the processor), but a program that goes on after
an error has to stop it itself.
```
Type 550 microtape sample -- write a block, read it back in reverse

#include <MICROTAPE/type550defs.ah>

// Writes block BLK of unit 1 with the words 1, 2, ... 400, searches back to it in
// reverse, reads it backward into rbuf and checks that the block totals zero.
// Halts at good with AC = 0, or at bad with AC = the status word or the total.

#define BLK 12

100/
begin,
    lio [mtu1]
    mse                     // select unit 1; the 34 ms selection delay starts
    jsp fill                // buf = 1, 2, ... 400, cksum = its checksum
    lio [mtgo mtsrch]
    mlc                     // search forward: 200 ms to speed, then a flag per block
    lac [BLK]
    dac want
    jsp find                // take search flags up to block BLK's
    jsp write               // write it, then search forward again
    lac [BLK+1]
    dac want
    jsp find                // the next block's mark: the tape is past block BLK
    lio [mtgo mtrev mtsrch]
    mlc                     // search in reverse (300 ms to turn around)
    lac [BLK]
    dac want
    jsp find                // block BLK again, now from its far end
    jsp readrv              // read it backward into rbuf
    cli
    mlc                     // go = 0: stop the tape

    law rbuf                // add up the MTWDS+2 words of rbuf
    dap tot1
    law i MTWDS+2
    dac n
    dzm sum
tot1,
    lac .
    add sum
    dac sum
    idx tot1
    isp n
    jmp tot1
    lac sum
    sza                     // skip if the block totals zero
    jmp bad                 // AC = the total
good,
    hlt                     // AC = 0: rbuf holds the block as it was written
    jmp begin

fail,
    mrs                     // an error: IO = the status word
    dio stat                // saved first, the mlc below clears the error bits
    cli
    mlc                     // stop the tape (the halt below would stop it too)
    lac stat                // AC = the status word
bad,
    hlt                     // AC = the status word, or the block total
    jmp begin

// Takes search flags until the one for block want, and returns at once:
// the read or write deadline is counted from that flag.
find,
    dap findx
find1,
    mrs                     // IO = status
    spi i                   // skip if the data flag is up
    jmp find2
    mrd                     // IO = mark code | block number
    dio word
    lac word
    and [mtblk]
    sad want                // skip if it is not block want
findx,
    jmp .                   // it is: return
    jmp find1
find2,
    ril 2s                  // the error flag to IO bit 0
    spi i                   // skip if the error flag is up
    jmp find1               // nothing yet
    jmp fail

// Fills buf with 1, 2, ... MTWDS, and sets cksum to their checksum.
fill,
    dap fillx
    law buf
    dap fillp
    dzm n
    dzm sum
fill1,
    idx n                   // AC = n = 1, 2, ...
fillp,
    dac .
    add sum
    dac sum
    idx fillp               // the next word of buf
    lac n
    sad [MTWDS]             // skip while n is short of MTWDS
    jmp fill2
    jmp fill1
fill2,
    lac sum
    cma                     // the checksum is the complement of the sum
    dac cksum
fillx,
    jmp .

// Writes buf and cksum into the block whose search flag was just taken (the
// mlc within 400 us of that flag, the first mwr within 600 us), then sets the
// tape searching forward again. Every flag must be answered within 200 us, so
// the loop only looks at the data flag; a MISS leaves the error flag up,
// checked once at the end.
write,
    dap writex
    law buf
    dap wrp
    lio [mtgo mtwrit]
    mlc                     // write: the control asks for the first word at once
wr1,
    mrs                     // IO = status
    spi i                   // skip if the data flag is up
    jmp wr2
wrp,
    lio .                   // the next data word
    mwr
    idx wrp
    jmp wr1
wr2,
    ril 1s                  // the block end flag to IO bit 0
    spi                     // skip unless it is up
    jmp wr3                 // BEF: send the checksum
    ril 1s                  // the error flag to IO bit 0
    spi i                   // skip if it is up
    jmp wr1                 // nothing yet
    jmp fail
wr3,
    lio cksum
    mwr
    law i 24                // about 300 us: the checksum reaches the tape
    dac n                   // 200 us after the block end flag
wr4,
    isp n
    jmp wr4
    mrs
    ril 2s                  // the error flag to IO bit 0
    spi                     // skip unless it is up: nothing was missed
    jmp fail
    lio [mtgo mtsrch]
    mlc                     // back to searching forward
writex,
    jmp .

// Reads the block whose reverse search flag was just taken (mlc within 600 us
// of that flag), backward. The words come last first, so they are stored from
// the end of rbuf down, and rbuf ends up holding the block as written: the
// leading checksum, the MTWDS data words, the trailing checksum.
readrv,
    dap readx
    law rbuf+MTWDS+1        // the last of the MTWDS+2 words
    dap rdp
    lio [mtgo mtrev mtread]
    mlc                     // read in reverse: a flag with each word
rd1,
    mrs                     // IO = status
    spi i                   // skip if the data flag is up
    jmp rd2
    mrd                     // IO = the word
rdp,
    dio .
    lac rdp
    sub [1]
    dap rdp
    jmp rd1
rd2,
    ril 1s                  // the block end flag to IO bit 0
    spi                     // skip unless it is up
    jmp rd3                 // BEF: the last word
    ril 1s                  // the error flag to IO bit 0
    spi i                   // skip if it is up
    jmp rd1                 // nothing yet
    jmp fail
rd3,
    mrd                     // the leading checksum, which comes last in reverse
    dio rbuf
    mrs
    ril 2s                  // the error flag to IO bit 0
    spi                     // skip unless it is up: nothing was missed
    jmp fail
readx,
    jmp .

want,   0
cksum,  0
stat,   0
word,   0
sum,    0
n,      0

constants

buf,
    table MTWDS
rbuf,
    table MTWDS+2

start begin
```

Changing direction brings the tape back to where it was when the command was given.
That is why the example lets block 13's mark go by before it reverses, reversing straight
after the write would start just short of block 12's reverse block mark, and the first flag
in reverse would be block 11's.

A general search compares the block number it sees with the one it wants, and reverses
when it has gone past.

## What is not emulated

- The MIT PDP-1 controller, a different design
- Mode 7 as the manual has it, writing the mark track, and the WRTM switch that allows it.
  Mode 7 erases the current tape instead.
- Modes 5 and 6, reading and writing through block ends. The 1963 documents call them "not
  presently connected"; H-550 (1965) describes them as working. They act as move.
- The variation of the tape speed in reverse, +-20% in the brochures and the DECUS paper,
  +-30% in H-550. Both directions run at the nominal speed.
- Mark track errors; MTE is never set.
- The "dummy flag" of a change from write to read, and the 140-480 us latch that holds a
  stop or read command while a written word is being shifted out. The latch at the end of a
  block being written, DEC's D256, is emulated; see `mlc`.
- Mode 7 raising flags from a clock, as the real control did.

The block layout is confirmed by DEC's figures: Fig. 4 of the June 1964 brochure, and
Figs. 2, 4 and 5 of the DECUS paper.
