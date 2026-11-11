## Include files for the Type 550 Microtape

This also documents the filesystem routines.

This is version 1.1\
11-Nov-2026

## What's here?

- type550defs.ah, the six IOTs (`mmt`, `mse`, `mlc`, `mrd`, `mwr`,
  `mrs`), the unit numbers, the `mlc` control word and its modes (`mtfmt`,
  mode 7, erases a tape), the status bits, the results of `mmt` and the
  search-word fields.
- `mmt` is a nonstandard extension, it mounts a tape image named by a program.
- A file system for the Microtape, an include library, that manages named files of any
  size, up to the tape's maximum, on a tape.
  Its routines are callable from any bank.

  | File | Gives |
  |---|---|
  | mtfs.ah | the status codes, the parameter blocks, the layouts, `MTF_TAPE` and `MTF_FILE` to reserve storage, `MTF_INFO_TYPE`, `MTF_INFO_APP` and `MTF_INFO_RO` |
  | mtblock.ac | `mtbRead`, `mtbWrite`: whole blocks between a drive and memory; usable alone |
  | mtfdir.ac | `mtfEntry` (a directory entry, for listing), `mtfFree` (free blocks and gaps); the directory code the others use |
  | mtfmount.ac | `mtfMount` |
  | mtfformat.ac | `mtfFormat` |
  | mtfopen.ac | `mtfOpenName`, `mtfOpenIndex`, `mtfClose` |
  | mtfcreate.ac | `mtfCreate` |
  | mtfio.ac | `mtfRead`, `mtfWrite`, `mtfSeek` |
  | mtfinfo.ac | `mtfProtect`, `mtfSetType`, `mtfSetApp` |
  | mtfdelete.ac | `mtfDelete` |
  | mtfpack.ac | `mtfPack` |

See Docs/UsingType550Microtape.md for how to program the tape itself.

## The file system

A tape holds 576 blocks (01100) of 256 words (0400).
The file system uses them as:
| Block | Holds |
|---|---|
| 0 | nothing, allows a reverse search to complete without entering the end zone |
| 1 | the directory |
| 2 | a copy of the directory |
| 3-1077 | the files, each a run of contiguous blocks |

The directory has 31 (037) entries.
Each holds a name, the first block, the blocks allocated, the length in words, and an 18-bit information word.

- **Names** are 1 to 9 characters, stored as `ascii` strings.
  They are kept in sixbit by `UTIL/sixbit.ac`'s `am1ToSixbit`, upper case only; `Beta.Dat`
  and `BETA.DAT` are equivalent.
  Characters 0140-0177 are mapped into 0100-0137.
  The character `~` is `^` and `{` is `[`.
  Control characters are refused, and the first character cannot be a blank.
- **The information word**: bit 0 (0400000) is read-only, which the library enforces.
  Read-only file refuses `mtfWrite` and `mtfDelete` unless `mtfProtect` clears it.
  Bits 1-5 (0370000) and 6-17 (007777, the type) have no meaning to the library,
  they are for an application's use.
- **The directory is written twice**, block 1 and a copy in block 2 with a generation count for
  each write.
  Mounting uses the directory in block 1 if it is valid, else block 2.
  A halt during a directory write loses at most one change.
- **A file never grows past its allocation**, given when it is created, 1 to 511 (0777)
  blocks. Its first free block is the first gap large enough when it is created, first fit.
  `mtfPack` rewrites the tape, packing files and recovering gaps.
- **A tape is formatted only by `mtfFormat`**, never by mounting.
  Format refuses a tape whose blocks 1 and 2 hold another program's data unless told to force it.

## Using the library

Include `MICROTAPE/mtfs.ah`, then the `.ac` files of the calls the program uses,
in any bank, don't forget to issue a 'constants' afterwards.
Each `.ac` includes the files it needs, for example, `mtfio.ac` brings in open, close, the directory,
and the block layer.
If needed files have already been included, they can be in any bank, all the routines are bank-safe
and cross-bank callable.
The library uses the PDP-1D instructions `lai`, `lia` and `sni`, so the emulator needs
`core1D` (or `all1D`) on in `pidp1.config`, which is the default.

Working buffers are needed.
The program reserves the storage, anywhere in any bank, using the macros:
- `MTF_TAPE(name)`, a tape handle, 01010 words, the drive, the directory and a block buffer.
  One per mounted tape.
- `MTF_FILE(name)`, a file descriptor, 011 words.
  One per open file.
  It contains the tape handle's address, the slot, the first block, the blocks, the length,
  the position (`MTF_FPOS`), the information word, the flags and the handle's mount count when it was opened.
  A program may read them, but should never set them.

A program restarted without reloading keeps them as the last run left them.

### Calling

Every call is made with `farjda` (`memory.ah`) from any bank, AC the 16-bit address of the
call's parameter block and returns with extend mode on.
AC holds the status, `MTF_OK`, 0, or an `MTF_E` code, and IO the result if the call has one:

```
    lac [mypb:*]                // mypb: the handle, the drive
    farjda(mtfMount:*)
    sza                         // MTF_OK?
    jmp failed
```

Addresses in parameter blocks are 16-bit so the block, the buffers, the handle and the
descriptors can each be in any bank.
`mtfClose` and `mtfDelete` expect the descriptor's address in AC.

A descriptor is valid until it is closed and calls then return `MTF_EBADFD` when its tape handle is mounted
or formatted again, or when its file is deleted or moved by `mtfPack`.
A close that finds it invalid marks it closed and writes nothing.

- **Sequence breaks are off during every call that moves the tape** and turned on again at
  the end only if they were on.
  The tape IOT automatically requests a break at every flag so before turning
  them on again the call clears every pending request with `cbs`; a request any other device made
  during the call is cleared.
  **Never call the library from a break routine.**
  While a call runs, nothing driven by breaks runs.
  For example, a display refreshed by breaks will not update.
  A call takes from a fraction of a second to about 30 seconds for a search from one end of the tape to the other.
- **A program that keeps breaks off** and turns them on later, after calls, should do `cbs`
  before its `esm`: a flag the tape raised while breaks are off is held as a request and
  `esm` will cause an immediate break.
- **Every call blocks** until it is done and leaves the tape stopped.
- **No call retries.** A missed word or a bad block is reported.
  A halt during a call stops the tape and the call returns `MTF_EHALTED`.
  The call can be repeated.
- **Not reentrant**: one call at a time.
- **Drives are 1 to 8.**

### The calls

The parameter block's content, by the `mtfs.ah` names:

| Call | Parameter block | Result |
|---|---|---|
| `mtfFormat` | `MTF_PHANDLE` the handle, `MTF_PDRIVE` the drive, `MTF_PFORCE` nonzero to format the tape | the handle mounted on an empty directory |
| `mtfMount` | `MTF_PHANDLE`, `MTF_PDRIVE` | the handle is mounted |
| `mtfEntry` | `MTF_PHANDLE`, `MTF_PINDEX` the slot, 0-036, `MTF_PDEST` where its 8 words go | the entry; a free slot's first word is 0 |
| `mtfFree` | `MTF_PHANDLE`, `MTF_PNEED` blocks wanted | `MTF_PTOTAL` free blocks, `MTF_PLARGEST` the largest gap, `MTF_PFIT` the first gap of `MTF_PNEED` blocks or -1, set in the block |
| `mtfCreate` | `MTF_PHANDLE`, `MTF_PNAME` the name's address, `MTF_PBLOCKS` 1-0777, `MTF_PINFO` the information word, `MTF_PCFD` the descriptor | the file, open at position 0, length 0 |
| `mtfOpenName` | `MTF_PHANDLE`, `MTF_PNAME`, `MTF_PFD` the descriptor | the descriptor open at position 0 |
| `mtfOpenIndex` | `MTF_PHANDLE`, `MTF_PINDEX` the slot, `MTF_PFD` | the same |
| `mtfClose` | AC = the descriptor | the entry is written if it changed |
| `mtfRead` | `MTF_PFDESC` the descriptor, `MTF_PBUF` the buffer, `MTF_PCOUNT` the words | IO = the words read, can be shorter than requested as C's `read()` behaves |
| `mtfWrite` | `MTF_PFDESC`, `MTF_PBUF`, `MTF_PCOUNT` | IO = the number of words written; the length grows up to the allocation |
| `mtfSeek` | `MTF_PFDESC`, `MTF_PPOS` the position, 0 to the length | the position is set; the tape does not move |
| `mtfProtect` | `MTF_PFDESC`, `MTF_PVALUE` nonzero for read-only | set the descriptor, updated on the tape at the close |
| `mtfSetType` | `MTF_PFDESC`, `MTF_PVALUE` 0-07777 | the same |
| `mtfSetApp` | `MTF_PFDESC`, `MTF_PVALUE` 0-037, right-justified | the same |
| `mtfDelete` | AC = the descriptor | the entry cleared and the descriptor closed, the blocks are not erased |
| `mtfPack` | `MTF_PHANDLE`, `MTF_PSCRATCH` a buffer, `MTF_PSIZE`, at least 0400 | IO = the number of files moved |
| `mtbRead`, `mtbWrite` | `MTB_PDRIVE`, `MTB_PBLOCK` the first block, `MTB_PCOUNT` the blocks, `MTB_PBUF` 0400 words a block | the blocks, read in forward order, checksums kept |

- **Reads and writes** move entire blocks between the tape and the buffer; a part of
  a block at either end goes through the handle's block buffer, read first and, for a write,
  changed and written back.
  A transfer is at most three passes over the tape.
  A write that would exceed the allocation is refused completely giving `MTF_ERANGE` and a read-only file refuses
  every write.
- **A changed length or information word is only written to the tape at `mtfClose`.** A program that
  writes and never closes leaves the directory's length as it was. If the close fails, the
  descriptor stays open and the close can be repeated.
- **`mtfPack`** moves each file down to the lowest free block, in order, copying whole blocks
  of the scratch buffer size at a time.
  One block is enough, more is faster.
  Close every file of the tape first, the descriptor of a file that moves is no longer valid.
  Before a file moves, its entry's word 7 (`MTF_DEMOVE`) is set to the new first block and the directory written.
  After complete, the entry gets the new first block number and word 7 is cleared.
  A halt or an error during a move leaves the file marked with its words partly moved and `mtfEntry`
  gives the last location moved.
  Pack does not try to finish it and a later pack leaves a marked file where it is.
- **The block layer**, `mtblock.ac`, can be used alone, on any blocks of any tape, a file
  system's or not.
  It writes block 1 or 2 of a file-system tape as ordinary data blocks.

### Status codes

| Code | Name | Meaning |
|---|---|---|
| 0 | `MTF_OK` | done |
| 1 | `MTF_ENODRIVE` | the drive is not 1-8 |
| 2 | `MTF_ENOTAPE` | no tape on the drive, or it ran off its reel |
| 3 | `MTF_ELOCKED` | a write to a write-locked drive |
| 4 | `MTF_EHALTED` | a halt stopped the tape; the call can be repeated |
| 5 | `MTF_EBADBLOCK` | a block did not total zero or a word was missed |
| 6 | `MTF_EENDZONE` | the tape ran into an end zone |
| 7 | `MTF_ENOFS` | no file system on the tape or the handle is not mounted |
| 010 | `MTF_EFOREIGN` | format refused: blocks 1-2 hold another program's data |
| 011 | `MTF_ENAME` | a name is empty, too long, starts with a blank or with a control character |
| 012 | `MTF_EEXISTS` | a file of that name is already on the tape |
| 013 | `MTF_ENOTFOUND` | no such name or the slot is free |
| 014 | `MTF_EDIRFULL` | all 31 slots are in use |
| 015 | `MTF_ENOSPACE` | no gap is large enough; `mtfPack` can possibly make one |
| 016 | `MTF_ERANGE` | a block, slot, count, position, or value out of range, or a write past the allocation |
| 017 | `MTF_EREADONLY` | the file is read-only |
| 020 | `MTF_EBADFD` | the descriptor is not open or no longer valid |
| 021 | `MTF_ESMALLBUF` | pack's buffer is under 0400 words |
| 022 | `MTF_ETIMEOUT` | a search saw no block mark, the tape couldn't be read |

## An example

Mount drive 1, format a blank tape, write four words to a new file, read them back.

```
Microtape file system example

#include <MICROTAPE/mtfs.ah>

100/
go,
    lsm
    cbs
    eem
    lac [hnd:1]
    dac pb
    law 1
    dac pb+1
    dzm pb+2
    lac [pb:0]
    farjda(mtfMount:*)
    sad [MTF_ENOFS]
    jmp fmt
    sza
    jmp fail
    jmp mk
fmt,
    lac [pb:0]
    farjda(mtfFormat:*)         // refused unless blocks 1-2 are blank or a file system directory
    sza
    jmp fail

mk,
    lac [hnd:1]
    dac pb
    lac [name:0]
    dac pb+1
    law 1
    dac pb+2                    // one block
    dzm pb+3
    lac [fd:1]
    dac pb+4
    lac [pb:0]
    farjda(mtfCreate:*)
    sza
    jmp fail
    lac [fd:1]
    dac pb
    lac [out:0]
    dac pb+1
    law 4
    dac pb+2
    lac [pb:0]
    farjda(mtfWrite:*)
    sza
    jmp fail
    lac [fd:1]
    farjda(mtfClose:*)          // the data is written to the tape
    sza
    jmp fail

    lac [hnd:1]
    dac pb
    lac [name:0]
    dac pb+1
    lac [fd:1]
    dac pb+2
    lac [pb:0]
    farjda(mtfOpenName:*)
    sza
    jmp fail
    lac [fd:1]
    dac pb
    lac [in:0]
    dac pb+1
    law 4
    dac pb+2
    lac [pb:0]
    farjda(mtfRead:*)           // IO = 4
    sza
    jmp fail
    lac [fd:1]
    farjda(mtfClose:*)
    hlt
fail,
    hlt                         // AC = the status
    jmp go

pb,
    table 5
name,
    ascii "hello"
out,
    1
    2
    3
    4
in,
    table 4
constants

bank 1
#include <MICROTAPE/mtfio.ac>
#include <MICROTAPE/mtfmount.ac>
#include <MICROTAPE/mtfformat.ac>
#include <MICROTAPE/mtfcreate.ac>
constants
MTF_TAPE(hnd)
MTF_FILE(fd)

start go:0
```

The second run finds HELLO and `mtfCreate` refuses with `MTF_EEXISTS`.
Open it by name instead or delete it first.

# See also

UsingMtfs.md in the Docs directory for a description of mtfs, a file system maintenance utility.
