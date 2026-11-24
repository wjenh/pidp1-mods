# Using the IBM 1301 disk

The Stanford PDP-1 accessed an IBM 1301 disk through an IBM 7631 file controlller shared with an IBM 7090.
This IOT recreates what the PDP-1's programs saw, the disk adapter's IOTs, the 131D
data channel, the 7631's orders and status word, and a two-module 1301 that spins and seeks in
real program time.
The 7090 is not recreated, file control always belongs to the PDP-1.

The IOT names and codes are the Stanford values.
What each does follows IBM's 7631 manual A22-6725 with the choices it leaves open made as below.
The am1 definitions are in `Am1Includes/IBM1301/ibm1301defs.ah`.
Every name there has the prefix `S_`.

## The devices

A program talks to two things.
- **The disk adapter, IOT 10**, has six IOT commands that start the 7631's commands, answer its
  signals, and reset it.
- **The 131D data channel, IOTs 46 and 36**, has an address, a word count, and a location counter.
  Every character the 7631 sends or receives, orders and status as well as data, moves
  between core and the 7631 through this channel via high-speed channel 4.

The IOTS are `IOT_10` with `IOT_36` and `IOT_46` as its aliases.

### The adapter's IOTs

| Name | Code | What it does |
|---|---|---|
| S_DRS | 720010 | Resets the adapter and the 7631 |
| S_DEN | 720110 | Answers whicever of end, unusual end, and attention is active |
| S_DSN | 720410 | Sense, sending the ten status characters to core |
| S_DRD | 720510 | Read, sending data to core |
| S_DWR | 720610 | Write, format write or write check, taking data from core |
| S_DCT | 720710 | Control, taking the order from core |

None of them skips or waits, and none touches AC.
Each leaves the **adapter word** in IO.

| Bit | Mask | Define | Meaning |
|---|---|---|---|
| 0 | 400000 | S_AEND | end, not yet answered; the last command ended normally |
| 1 | 200000 | S_AUEND | unusual end, not yet answered; the last command failed |
| 2 | 100000 | S_AATTN | attention, not yet answered; an access finished |
| 3 | 040000 | S_ABUSY | a command is in progress |
| 4 | 020000 | S_AREFUSED | this IOT's command was not started |
| 5 | 010000 | S_ACHAN | the channel is moving words |
| 6 | 004000 | S_ASIX | six-bit mode, clear in eight-bit mode |

End is the sign bit so `spi i` tests it easily.
S_DEN leaves the word as it was before its answer so the program sees what it answered.

A command (S_DSN, S_DRD, S_DWR, S_DCT) is refused, with nothing done, while another is busy or
while its end or unusual end is unanswered.
An unanswered attention does not block a command.

**S_DRS** ends any command at once with no end signal, stops the channel, drops end, unusual end,
and attention, and clears the status word's error bits and any prepare-to-verify.
It keeps the mode, each access's position, an access in motion (whose attention still is signaled),
and the channel's address and count.
A write it cuts short has the rest of its record filled with 0.
A power clear is a S_DRS.

### The channel's IOTs

| Name | Code | What it does |
|---|---|---|
| S_SID | 721346 | IO bits 2-17 are the core address of the first word, the bank in bits 2-5 |
| S_SWD | 720546 | IO bits 6-17 are the word count, 1-7777; 0 is 4096 words |
| S_RLD | 721336 | IO is set to the address of the next word the channel would move |
| S_SDF | 720146 | Stops the transfer, as the count running out does |

S_SID and S_SWD given while the channel is transferring words are ignored.
The channel keeps its address and count from one command to the next, so give S_SID and S_SWD before each command,
before S_DCT and S_DSN as well as before S_DRD and S_DWR.

**When the count runs out**, or at S_SDF, the 7631 goes on to the end of the record it is in, a
read without sending and a write filling the rest of the record with 0.
The end comes where it would have.
**When the command ends first**, the channel stops with words left; S_RLD says how far it got.

### Packing

- **Six-bit mode** (the default, order DSBM) packs three characters to a word, the first in bits 0-5.
  Characters run on across areas with no padding, a read's last word has its unused characters set to 0.
- **Eight-bit mode** (order DEBM) packs two bytes to a word, each right-justified in its nine-bit
  half, the first in bits 1-8, the second in 10-17.

An order is 4 words in six-bit mode, ten characters, two left over, or 1 word for a two-character order.
A sense is 4 words, the last holding character 9 and two 0 characters.

## End, unusual end, and attention

**End** means the command succeeded, **unusual end** that it failed and the status word says why,
and **attention** that an access completed.
Each stays up until S_DEN answers it.
A program checks them in three ways.
- In IO, after any IOT 10.
- In `cks`, as one bit, 1 while any of the three is up.
It is bit 10, 000200 (S_CKS), unless `ibm1301cks` changes it.
- By a sequence break on channel `ibm1301sbs`, 6 by default with the 16-channel system, each
  time one is signaled.
  The break routine gives S_DEN and looks at IO.

A signal that occurs while the pdp-1 is halted stays up, and its break comes when if runs again.

## The orders

An order is ten characters, the two-digit operation code, an access digit, always 0, a module
digit, four track digits, and two record-identifier characters.
Digits are the six-bit values 0-11 octal for 0-9.
`S_ORDER(op, module, track, id)` builds one in six-bit mode with the track
given in decimal (`0d123`) and the two identifier characters as one 12-bit value.
`S_ORDER2(op)` builds a two-character order.
`S_ORDER8` and `S_ORDER28` are the same in eight-bit mode.

| Order | Define | What it does |
|---|---|---|
| DNOP | S_DNOP | Nothing; end |
| DREL | S_DREL | Release the file control; end |
| DEBM | S_DEBM | Eight-bit mode; end |
| DSBM | S_DSBM | Six-bit mode; end |
| DSEK | S_DSEK | Seeks to the track's cylinder and selects its head; ends at once, attention when it is already there |
| DVSR | S_DVSR | Prepare to verify a single record, the next read or write moves the record whose address matches |
| DWRF | S_DWRF | Prepare to write format; the next write writes the cylinder's format track, and needs `ibm1301format=on` |
| DVTN | S_DVTN | Prepare to verify a track without addresses, the next read or write moves every record |
| DWRC | S_DWRC | Prepare to write check, the next write compares instead of writing |
| DSAI | S_DSAI | Sets the access inoperative until the emulator next starts; end |
| DVTA | S_DVTA | Prepare to verify a track with addresses, moving each record address and its record |

DVCY and DVHA are not recreated and give invalid code.

**The sequence.**
A read must follow DVSR, DVTA or DVTN; a write must follow one of them or DWRF.
Otherwise the result is unusual end, invalid sequence, with no data.
A prepare-to-verify serves the one read or write after it.
A sense between them is allowed, any other order cancels it.
A write check is DWRC with the same address as the prepare-to-verify after the write it checks
then a write of the same data.

**The checks**, in order, are these:
An operation code that is not one of the orders above or
DWRF with the key-lock off gives invalid code.
A digit not 0-9 gives invalid address.
An access other than 0, a module not installedd, or an access set inoperative gives access inoperative.
DSEK, a prepare-to-verify or DWRF to a module that is accessing gives access not ready.
A failed check ends the order at once with unusual end.
A character the channel never sent counts as 077, which is no digit.

**Addresses.**
DSEK's track selects cylinder (track / 40) and head (track mod 40).
DVTA, DVTN and DWRF compare the track with the home address of the head selected, so the track must be in the
cylinder the access is at.
DVSR compares each record address on the selected head.

## The status word

The status word is ten characters sent by S_DSN.
`S_STW(c)` is the word character c is in, and `S_STB(c, m)` moves a character's mask into place in that word.

| Char | 20 | 04 | 02 | 01 |
|---|---|---|---|---|
| 0 summary | operator intervention | program check | data check | exceptional condition |
| 1 program check | invalid sequence | invalid code | format check | no record found |
| 2 | invalid address | response check | data compare check | parity check |
| 3 exceptional | access inoperative | access not ready | disk storage circuit check | file control circuit check |
| 4 data mode | channel interrupt | six-bit mode | | |
| 5 attention | module 0 | module 1 | module 2 | module 3 |
| 6 attention | module 4 | module 5 | module 6 | module 7 |
| 7 attention | module 8 | module 9 | | |

Every command but a sense clears characters 0-3 and the attention bit of the
module it addresses when it starts, a sense clears nothing.
These are raised here:
- invalid sequence, invalid code, invalid address, access inoperative, and not ready;
- no record found;
- format check, a bad format record, or a read or write of a cylinder in the other mode;
- response check, a halt during a transfer;
- data compare check;
- disk storage circuit check, the image file couldn't be read or written.

Operator intervention, parity check, file control circuit check, and channel interrupt are never
raised.

## The disk

**Modules.**
`ibm1301modules` sets how many, 1-10, 2 by default, numbered from 0.
Each has 250 cylinders of 40 tracks, tracks 0000-9999, track = cylinder x 40 + head, and an image file.

**The format track.**
Each cylinder's layout (HA2, each record address and record, and the mode)
is set by its format track, written by DWRF and a write of BCD 1-4 characters, as IBM describes.
It is the track identification area `444 333333333 4 3333333333 4`, then HA2's area, then for
each record an X gap, a record address area, a Y gap and a record area, then gap 3.
Ones and zeros are 1 and 2 in a six-bit cylinder, 3 and 4 in an eight-bit one.
A record that breaks IBM's layout rules, mixes the modes, or exceeds the track's capacity of
2840 positions in six-bit, 2205 in eight-bit, ends with format check immediately.
A format write takes about two revolutions.
A new format changes only the layout, the characters on the tracks stay where they were.

**HA1** is the track number and fixed.
**HA2** can be set only with the host tool.

**A fresh image** is formatted six-bit on every cylinder with HA2 `00 00` and five records of
528 characters (176 words), each with a six-character address of the four track digits, 0, and
the record number 1-5.
Tecord r of track t is found by DVSR with `S_ORDER(S_DVSR, module, t, r)`.

### Timing

The disk spins at 1790 rpm, 33.52 ms a revolution, and both rotation and seeks run on program
time, the time base the drum and the clock use, which keeps running through a halt.

| | Six-bit | Eight-bit |
|---|---|---|
| a character | 11.1 us | 14.2 us |
| a channel word | 33.3 us | 28.4 us |
| a ten-character order | 148 us | 190 us |
| a two-character order | 55 us | 75 us |
| a sense | 123 us | 158 us |

| Seek | Time |
|---|---|
| the same cylinder | 0 |
| another cylinder in the same group | 50 ms |
| another group in the same zone | 120 ms |
| another zone | 180 ms |

Zones are cylinders 0-49, 50-99, etc.
Within a zone the groups are its cylinders 0-9, 10-19, 20-24, 25-34, 35-44, 45-49.
While an access is in progress, its module answers DSEK,
prepare-to-verify, and DWRF with access not ready.
Other modules work as usual.
A seek to the cylinder the access is at gives attention with the seek's end, both can be answered by one S_DEN.
At emulator start every access is at cylinder 0, with no attention.

DVSR searches from wherever the track is and ends with no record found at the second passing of the index.
DVTA and DVTN start at the home address after the next index.
An order takes longer than the gap before a record, so a DVSR for the next record given at one record's end
finds it a revolution later.

Each channel word is one stolen memory cycle, 5 us of pdp-1 program delay for each word moved while it runs.

### A halt

The disk spins and the accesses move through a halt, an access that finishes during one raises
attention then.
A STOP, start, continue, examine, or read-in during a transfer stops the channel,
with unusual end, response check, at the end of the area.
A write keeps the characters that moved and has the rest of its record filled with 0.
A halt before the first word moves changes nothing.

## The image files

Each module is one host file named by `ibm1301file` with `%d` for the module number.
The IOT maps it as shared so a write costs no host I/O and a change the host tool makes while the emulator
runs is seen at once.
The files are flushed at a STOP and at exit.
A file that does not exist is created, formatted as above, at the first disk IOT.
Creating two images takes some tens of milliseconds on the emulator's thread, a one-time jump of program time.
make them beforehand with `dsk1301` to avoid it.

### dsk1301

```
dsk1301 make [-b] [-f] FILE MODULE
dsk1301 format FILE CYL[-CYL] 6|8 HA2 RA RECORD COUNT
dsk1301 ha2 FILE TRACK[-TRACK] C,C,...
dsk1301 dump FILE CYL [TRACK]
dsk1301 put FILE TRACK RECORD HOSTFILE
dsk1301 get FILE TRACK RECORD HOSTFILE
```

- `make` makes a fresh image of module 0-9, formatted as above; `-b` leaves it unformatted, `-f`
  replaces an existing file.
- `format` formats cylinders with the mode, HA2's length, the record address's and record's
  lengths and the records on a track; HA2 and the records become 0, the addresses as a fresh
  image has them.
- `ha2` sets HA2 on tracks, one octal character value for each of its characters.
- `dump` shows a cylinder's format, or a track's areas, in octal and as 7090 BCD.
- `put` and `get` copy a record (numbered from 1) of a track from or to a host file, one byte
  per character; `put` fills a short record out with 0.

## Configuration

These settings are in `pidp1.config`, read at start and again at a reload.

| Setting | What it does | Default | At a reload |
|---|---|---|---|
| ibm1301modules | Modules installed, 1-10 | 2 | when no command is busy |
| ibm1301file | Each module's image file, `%d` for its number | `/opt/pidp1-mods/ibm1301m%d` | when no command is busy |
| ibm1301format | The format key-lock switches at WRITE, allowing DWRF | off | at once |
| ibm1301sbs | The sequence break channel, 0-15 | 6 | at once |
| ibm1301cks | The `cks` bit of end, unusual end or attention, 0-17 | 10 | at once |

## An example

This program reads record 1 of track 123 on module 0 into 1000-1257.
Track 123 is in cylinder 3, so the access must seek there first.

```
IBM 1301 example: record 1 of track 123
#include <IBM1301/ibm1301defs.ah>

100/
// The seek, its order four words through the channel.
go, lio [osek]
    S_SID
    lio [4]
    S_SWD
    S_DCT
    jsp wait
sk1, and [S_AATTN]          // attention with the end, the access already there
    sza
    jmp rd
    jsp wait                // wait for the access to complete
    jmp sk1

// The record, read after a DVSR, 176 words.
rd, lio [ovsr]
    S_SID
    lio [4]
    S_SWD
    S_DCT
    jsp wait
    lio [buf]
    S_SID
    lio [260]
    S_SWD
    S_DRD
    jsp wait
    spi i                   // end, not unusual end
    jmp bad
    hlt                     // the record is in buf

// The status word, four words, says why.
bad, lio [stat]
    S_SID
    lio [4]
    S_SWD
    S_DSN
    jsp wait
    hlt

// wait waits for end, unusual end or attention, and answers it.
// Has no arguments; AC and IO are the adapter word as it was before the answer.
// Called via jsp.
wait, dap wx
wl, cks
    rir 8s                  // cks bit 10 into IO's sign
    spi i
    jmp wl
    S_DEN
    dio aw
    lac aw
wx, jmp .

aw, 0
osek, S_ORDER(S_DSEK, 0, 0d123, 0)
ovsr, S_ORDER(S_DVSR, 0, 0d123, 1)
stat, 0; 0; 0; 0

constants

1000/
buf, 0

start go
```
