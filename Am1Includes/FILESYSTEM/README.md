## The file system

Include files for a file system of named files on a block device.
The calls are the same on every device.
A device's specific layer mounts and formats its volumes and moves its
blocks, see `MICROTAPE/` for the Type 550 Microtape and `DRUM/` for the Type 23 Parallel Drum.

This is version 2.1\
12-Nov-2026
Initial version.

## What's here?

An include library that manages named files of any size on a volume, up to the volume's maximum.
Its routines are callable from any bank.

| File | Contains |
|---|---|
| fs.ah | the status codes, the parameter blocks, the layouts, `FS_HANDLE`, `FS_FILE` |
| fsblock.ac | `fsBRead`, `fsBWrite`, handles blocks through the handle's device |
| fsdir.ac | `fsLookup`, `fsEntry`, `fsFree`; `fsRdDir`, `fsWrDir`, `fsDirOk` for a device's layer |
| fsformat.ac | `fsFormatVol`, for a device's format |
| fsopen.ac | `fsOpenName`, `fsOpenIndex`, `fsClose` |
| fscreate.ac | `fsCreate` |
| fsio.ac | `fsRead`, `fsWrite`, `fsSeek` |
| fsinfo.ac | `fsProtect`, `fsSetType`, `fsSetApp` |
| fsdelete.ac | `fsDelete` |
| fsrename.ac | `fsRename` |
| fstrim.ac | `fsTrim` |
| fspack.ac | `fsPack` |

No file here includes a device's files.
A program gets a volume's handle from that device's specific mount or format, and every call here then
works the same regardless of the underlying device.

| Device | Directory | Mount, format |
|---|---|---|
| Type 550 Microtape | `MICROTAPE/`, see its `README.md` | `mtMount`, `mtFormat` |
| Type 23 Parallel Drum | `DRUM/`, see its `README.md` | `drMount`, `drFormat` |

A program can mount multiple volumes at once, on one device or several, each with its own
handle, and have files open on all of them.

## The volume

A volume holds blocks of 256 words (0400, `FS_BLKW`), on every device.
How many there are is specific to the device, a Microtape holds 576 (01100), a drum volume 16 for each track.
The file system uses them as:
| Block | Holds |
|---|---|
| 0 | nothing, allows a tape's reverse search to complete without entering the end zone |
| 1 | the directory |
| 2 | a copy of the directory |
| 3 to the last | the files, each a run of contiguous blocks |

The directory has 31 (037) entries.
Each holds a name, the first block, the blocks allocated, the length in words, and an 18-bit information word.
Its header records the number of the volume's blocks.

- **Names** are 1 to 9 characters, stored as `ascii` strings.
  They are kept in sixbit upper case only; `Beta.Dat` and `BETA.DAT` are equivalent.
  Characters 0140-0177 are mapped into 0100-0137.
  The character `~` is `^` and `{` is `[`.
  Control characters are refused and the first character cannot be a blank.
  Nine characters will not hold DEC's six-and-three names or
  MAC's two six-character names; a program porting such names has to shorten them.
- **The information word**: bit 0 (0400000) is read-only, which the library enforces.
  Read-only file refuses `fsWrite` and `fsDelete` unless `fsProtect` clears it.
  Bits 1-5 (0370000) and 6-17 (007777, the type) have no meaning to the library,
  they are for an application's use.
- **The directory is written twice**, block 1 and a copy in block 2 with a generation count for
  each write.
  Mounting uses the directory in block 1 if it is valid, else block 2.
  A halt during a directory write loses at most one change.
- **A file never grows past its allocation**, given when it is created, 1 to 511 (0777) blocks.
  Its first free block is the first gap large enough when it is created, first fit.
  `fsTrim` trims the blocks its length does not use, they will be recovered after packing.
  `fsPack` rewrites the volume, packing files and recovering gaps.
- **A volume is formatted only by its device's format**, never by mounting.
  Format refuses a volume whose blocks 1 and 2 hold another program's data unless told to force it.

## Using the library

Include the device's definitions then the device's mount and format files and the `.ac` files of the calls
the program uses, in any bank, don't forget to issue a 'constants' afterwards.
Each `.ac` includes the files it needs, for example, `fsio.ac` brings in open, close, the directory,
and the dispatch to the device's block layer.
If needed files have already been included, they can be in any bank, all the routines are bank-safe
and cross-bank callable.
The library uses the PDP-1D instructions `lai`, `lia` and `sni`, so the emulator needs
`core1D` (or `all1D`) on in `pidp1.config`, which is the default.

Working buffers are needed.
The program reserves the storage, anywhere in any bank, using the macros:
- `FS_HANDLE(name)`, a handle, 01047 words: the unit, the device, the directory, a block buffer
  and a count for each directory slot.
  One per mounted volume.
- `FS_FILE(name)`, a file descriptor, 012 words.
  One per open file.
  It contains the handle's address, the slot, the first block, the blocks, the length,
  the position (`FS_FPOS`), the information word, the flags, and the handle's mount count and
  the slot's count when it was opened.
  A program may read them, but should never set them.

A program restarted without reloading keeps them as the last run left them.

There is no unmount, a program done with a volume simply stops using its handle.
That is safe once every file of the volume is closed, the library keeps no information about a
volume outside its handle and the descriptors.

### Calling

Every call is made with `farjda` (`memory.ah`) from any bank, AC the 16-bit address of the
call's parameter block and returns with extend mode on.
However, if the call is within the same bank, a regular jda can be used.
AC holds the status, `FS_OK`, 0, or an `FS_E` code, and IO the result if the call has one:

```
    lac [mypb:*]                // mypb: the handle, the drive
    farjda(mtMount:*)
    sza                         // FS_OK?
    jmp failed
```

Addresses in parameter blocks are 16-bit so the block, the buffers, the handle and the
descriptors can each be in any bank.
`fsClose` and `fsDelete` expect the descriptor's address in AC.
The descriptor is not at the same word in every block.
It is in word 2 (`FS_PFD`) for `fsOpenName` and `fsOpenIndex`, word 4 (`FS_PCFD`) for `fsCreate`.

A descriptor is valid until it is closed.
Calls will then return `FS_EBADFD` if its handle is mounted
or formatted again, when its file is deleted, replaced by `fsRename`, or moved by `fsPack`.
A close that finds it invalid marks it closed and writes nothing.
Renaming a file leaves its descriptors valid.
A descriptor left open on a deleted or replaced file stays refused even after a later
`fsCreate` takes the same directory slot and the same first block.
Each slot has a count in the handle, incremented whenever the slot is cleared,
and a descriptor keeps the count it was opened with.

- **Sequence breaks are off during every call that moves blocks** and turned on again at
  the end only if they were on.
  Before turning them on again the call clears every pending request with `cbs`.
  A request any other device made during the call is cleared.
  **Never call the library from a break routine.**
  While a call runs, nothing driven by breaks runs.
  For example, a display refreshed by breaks will not update.
  How long a call takes is specific to the device, up to about 30 seconds on a Microtape,
  and on the drum a few revolutions of about 35 ms each, more for `fsPack`.
- **A program that keeps breaks off** and turns them on later, after calls, should do `cbs`
  before its `esm`.
  A flag the device raised while breaks are off is held as a request and
  `esm` will cause an immediate break.
- **Every call blocks** until it is done.
- **No call retries.** A missed word or a bad block is reported.
  A halt during a call stops the transfer and the call returns `FS_EHALTED` on a Microtape,
  `FS_EXFER` on the drum.
  The call can be repeated.

### The calls

The parameter block's content is the same for all devices.
The mount and format are device specific, defined in its own directory.

| Call | Parameter block | Result |
|---|---|---|
| `fsLookup` | `FS_PHANDLE` the handle, `FS_PNAME` the name's address, `FS_PNAMED` where its 3 sixbit words go | IO = the file's slot; `FS_ENOTFOUND` with IO the first free slot, or -1 when there is none; no block moves |
| `fsEntry` | `FS_PHANDLE`, `FS_PINDEX` the slot, 0-036, `FS_PDEST` where its 8 words go | the entry; a free slot's first word is 0 |
| `fsFree` | `FS_PHANDLE`, `FS_PNEED` blocks wanted | `FS_PTOTAL` free blocks, `FS_PLARGEST` the largest gap, `FS_PFIT` the first gap of `FS_PNEED` blocks or -1, set in the block |
| `fsCreate` | `FS_PHANDLE`, `FS_PNAME` the name's address, `FS_PBLOCKS` 1-0777, `FS_PINFO` the information word, `FS_PCFD` the descriptor | the file, open at position 0, length 0 |
| `fsOpenName` | `FS_PHANDLE`, `FS_PNAME`, `FS_PFD` the descriptor | the descriptor open at position 0 |
| `fsOpenIndex` | `FS_PHANDLE`, `FS_PINDEX` the slot, `FS_PFD` | the same |
| `fsClose` | AC = the descriptor | the entry is written if it changed |
| `fsRead` | `FS_PFDESC` the descriptor, `FS_PBUF` the buffer, `FS_PCOUNT` the words | IO = the words read, can be shorter than requested as C's `read()` behaves |
| `fsWrite` | `FS_PFDESC`, `FS_PBUF`, `FS_PCOUNT` | IO = the number of words written; the length grows up to the allocation |
| `fsSeek` | `FS_PFDESC`, `FS_PPOS` the position, 0 to the length | the position is set; no block moves |
| `fsProtect` | `FS_PFDESC`, `FS_PVALUE` nonzero for read-only | set the descriptor, updated on the volume at the close |
| `fsSetType` | `FS_PFDESC`, `FS_PVALUE` 0-07777 | the same |
| `fsSetApp` | `FS_PFDESC`, `FS_PVALUE` 0-037, right-justified | the same |
| `fsDelete` | AC = the descriptor | the entry cleared and the descriptor closed, the blocks are not erased |
| `fsRename` | `FS_PHANDLE`, `FS_PNAME` the old name's address, `FS_PNEWNAME` the new name's, `FS_PREPLACE` nonzero to replace a file of the new name | the entry renamed, and a replaced file's entry cleared, in one directory write |
| `fsTrim` | `FS_PFDESC` the descriptor | the allocation set to the blocks the length needs, at least 1, and the entry written to the volume |
| `fsPack` | `FS_PHANDLE`, `FS_PSCRATCH` a buffer, `FS_PSIZE`, at least 0400 | IO = the number of files moved |
| `fsBRead`, `fsBWrite` | `FS_BHANDLE` the handle, `FS_BBLOCK` the first block, `FS_BCOUNT` the blocks, `FS_BBUF` 0400 words a block | the blocks, through the handle's device |

- **Reads and writes** move entire blocks between the volume and the buffer; a part of
  a block at either end goes through the handle's block buffer, read first and, for a write,
  changed and written back.
  A transfer is at most three passes over the blocks.
  A write that would exceed the allocation is refused completely giving `FS_ERANGE` and a read-only file refuses
  every write.
- **A changed length or information word is only written to the volume at `fsClose`** (or by
  `fsTrim`). A program that
  writes and never closes leaves the directory's length as it was. If the close fails, the
  descriptor stays open and the close can be repeated.
- **`fsRename`** refuses with `FS_ENOTFOUND` when the old name is not on the volume and with
  `FS_EEXISTS` when the new one is, unless `FS_PREPLACE` is nonzero.
  A replace clears the other file's entry in the same directory write as the rename, so a halt
  leaves either both changes or neither; a read-only file is not replaced (`FS_EREADONLY`).
  A read-only file may itself be renamed.
  A new name that is the same as the old one succeeds and writes nothing.
- **`fsTrim`** writes the file's entry immediately, the new allocation with the length and the
  information word as `fsClose` would write them, the close that follows has nothing left to write.
  The freed blocks are a gap and a later write past the new allocation gives `FS_ERANGE`.
  A read-only file refuses (`FS_EREADONLY`).
  Another descriptor open on the same file keeps the old allocation, close it first.
  Together the two calls let a program write a file before it knows its name or its size:
  create a scratch file with room to spare, write it, trim it, and rename it with the real
  name with `FS_PREPLACE` set.
- **`fsPack`** moves each file down to the lowest free block, in order, copying whole blocks
  of the scratch buffer size at a time.
  One block is enough, more is faster.
  Close every file of the volume first, the descriptor of a file that moves is no longer valid.
  Before a file moves, its entry's word 7 (`FS_DEMOVE`) is set to the new first block and the directory written.
  After complete, the entry gets the new first block number and word 7 is cleared.
  A halt or an error during a move leaves the file marked with its words partly moved and `fsEntry`
  gives the last location moved.
  Pack does not try to finish it and a later pack leaves a marked file where it is.
- **`fsBRead` and `fsBWrite`** move whole blocks of a volume, a file system's or not, through
  the device its handle names; the file system moves every block through them.
  The device refuses a request past the volume with `FS_ERANGE`.
  A handle no device's mount or format has set is refused with `FS_ENOFS`, and nothing moves.
  They write blocks 1 and 2 of a file-system volume as ordinary data blocks.

### Text files

The library stores words and does not itself attach any meaning to them.
For text, programs use the form the `mtfs` tool reads and writes (`Docs/UsingMtfs.md`):
two 8-bit characters a word, the first in the high half (bits 0-8), as am1's `ascii` packs
them; each line ends in CR (015) alone; an odd last character leaves the low half 0.
A text file the tool added then reads the same in a PDP-1 program, and one a program wrote
extracts as host text.

### Status codes

The file system's own codes arise on every device.
The device's are its block layer's, and its `README.md` says which arise there and what each
means for it.

| Code | Name | From | Meaning |
|---|---|---|---|
| 0 | `FS_OK` | | done |
| 1 | `FS_ENOUNIT` | device | no such unit: on a Microtape, the drive is not 1-8; on the drum, tracks not on the drum |
| 2 | `FS_ENOMEDIUM` | device | nothing to read: on a Microtape, no tape on the drive, or it ran off its reel |
| 3 | `FS_ELOCKED` | device | a write to a write-locked unit |
| 4 | `FS_EHALTED` | device | a halt stopped a Microtape's transfer; the call can be repeated |
| 5 | `FS_EBADBLOCK` | device | a block did not total zero or a word was missed |
| 6 | `FS_EENDZONE` | device | a Microtape ran into an end zone |
| 7 | `FS_ENOFS` | file system | no file system on the volume, or the handle is not mounted, or no device has set it |
| 010 | `FS_EFOREIGN` | file system | format refused: blocks 1-2 hold another program's data |
| 011 | `FS_ENAME` | file system | a name is empty, too long, starts with a blank or with a control character |
| 012 | `FS_EEXISTS` | file system | a file of that name is already on the volume |
| 013 | `FS_ENOTFOUND` | file system | no such name or the slot is free |
| 014 | `FS_EDIRFULL` | file system | all 31 slots are in use |
| 015 | `FS_ENOSPACE` | file system | no gap is large enough; `fsPack` can possibly make one |
| 016 | `FS_ERANGE` | both | a block, slot, count, position, or value out of range, or a write past the allocation |
| 017 | `FS_EREADONLY` | file system | the file is read-only |
| 020 | `FS_EBADFD` | file system | the descriptor is not open or no longer valid |
| 021 | `FS_ESMALLBUF` | file system | pack's buffer is under 0400 words |
| 022 | `FS_ETIMEOUT` | device | a Microtape search saw no block mark, the tape couldn't be read |
| 023 | `FS_EXFER` | device | a drum transfer failed. If from a halt the call can be repeated, or the drum file couldn't be written |

## An example

Mount a Microtape on drive 1, format it if it is blank, write four words to a new file, read
them back.

```
File system example, on a Microtape

#include <MICROTAPE/microtape.ah>

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
    farjda(mtMount:*)
    sad [FS_ENOFS]
    jmp fmt
    sza
    jmp fail
    jmp mk
fmt,
    lac [pb:0]
    farjda(mtFormat:*)          // refused unless blocks 1-2 are blank or a file system directory
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
    farjda(fsCreate:*)
    sza
    jmp fail
    lac [fd:1]
    dac pb
    lac [out:0]
    dac pb+1
    law 4
    dac pb+2
    lac [pb:0]
    farjda(fsWrite:*)
    sza
    jmp fail
    lac [fd:1]
    farjda(fsClose:*)           // the data is written to the tape
    sza
    jmp fail

    lac [hnd:1]
    dac pb
    lac [name:0]
    dac pb+1
    lac [fd:1]
    dac pb+2
    lac [pb:0]
    farjda(fsOpenName:*)
    sza
    jmp fail
    lac [fd:1]
    dac pb
    lac [in:0]
    dac pb+1
    law 4
    dac pb+2
    lac [pb:0]
    farjda(fsRead:*)            // IO = 4
    sza
    jmp fail
    lac [fd:1]
    farjda(fsClose:*)
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
#include <FILESYSTEM/fsio.ac>
#include <FILESYSTEM/fscreate.ac>
#include <MICROTAPE/mtmount.ac>
#include <MICROTAPE/mtformat.ac>
constants
FS_HANDLE(hnd)
FS_FILE(fd)

start go:0
```

The second run finds HELLO and `fsCreate` refuses with `FS_EEXISTS`.
Open it by name instead or delete it first.

## Writing a device's layer

A device supports the file system with a layer in its own directory that includes
`FILESYSTEM/fs.ah` and none of the shared `.ac` files except those its mount and format call:
- **Two handle entries**, block read and block write, each entered by `jda` with the window
  open and AC the 16-bit address of an `FS_B` parameter block (`FS_BHANDLE` to `FS_BBUF`).
  They read the unit and `FS_HBLKS` from the handle, refuse a request past the volume with
  `FS_ERANGE`, and return AC the status, `FS_OK` or a device code above.
  `fsBRead` and `fsBWrite` reach them through the handle, so they can be in any bank.
- **Setting the handle**: `FS_HUNIT` the unit as the device defines it, `FS_HDEV` the device
  (`FS_DEVMT`, `FS_DEVDRUM`), `FS_HBREAD` and `FS_HBWRITE` the entries' 16-bit addresses,
  `FS_HBLKS` the volume's blocks, and the directory header's words 4 to 7 in the handle's copy
  (below).
  `FS_HDEV` to `FS_HBLKS` are the handle's words 4 to 7, and with the header's words the only
  device state a handle has other than `FS_HUNIT`.
- **A mount**: sequence breaks off (`FS_SBS_OFF`), the handle set, `fsRdDir` with AC the
  handle, breaks restored (`FS_SBS_RESTORE`), its status returned.
  `fsRdDir` checks each copy with `fsDirOk`, so the directory's block count must equal the
  handle's `FS_HBLKS`.
- **A format**: the handle set, then `fsFormatVol` with AC the handle and IO the force flag.
  It turns breaks off and on itself.
- **The directory header's words 4 to 7** `FS_DDEV`, at `FS_HDIR+FS_DDEV` in the handle, are
  the device's to define.
  `fsFormatVol` writes into the new directory what the handle's copy holds when it is called,
  and every later directory write carries them from the handle's copy.
  A device that does not use them sets them to zeros, as the Microtape's does, so a handle
  another device used formats the same.
  A mount can check them after `fsRdDir`.
  The drum's refuses a directory whose first track is not the one given.

The Microtape's layer, `MICROTAPE/mtblock.ac`, `mtmount.ac` and `mtformat.ac`, and the drum's,
`DRUM/drblock.ac`, `drmount.ac` and `drformat.ac`, are the models.

# See also

`MICROTAPE/README.md`, the Microtape's read/write,  mount, and format logic.\
`DRUM/README.md`, the drum's read/write, mount, and format logic.\
See UsingMtfs.md in the Docs directory for a description of mtfs, a file system maintenance utility.
