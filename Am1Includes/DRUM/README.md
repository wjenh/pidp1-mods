## Include files for the Type 23 Parallel Drum

This also documents the drum's layer of the file system.
The file system's calls are in `FILESYSTEM/README.md`.

The file system's layer is version 2.1\
12-Nov-2026

## What's here?

- type23drumdefs.ah, the iot definitions for the type23drum.
- type23alt.ah, the same IOTs, with `t23dra` and `t23dss` in place of `dra` and `dss`, which the
  Type 340 display also uses, see IMPORTANT.md.
- overlay.ac, the drum overlay manager, include it once in the root of a program
  with am1 overlays before the first %%ovcall.
- overlay.ah, the defines for the overlay descriptor table, the overlay header and
  the manager's error halts.
- The drum's layer of the file system: the drum's block layer, mount,
  and format that give a program a handle for the drum volume.
  Its routines are callable from any bank.

  | File | Contains |
  |---|---|
  | drum.ah | the mount's and format's parameter block, the drum's geometry, the layout |
  | drblock.ac | `drbHRead`, `drbHWrite`, `drbSetHandle`|
  | drmount.ac | `drMount` |
  | drformat.ac | `drFormat` |

See the Docs directory for detailed drum usage and Docs/UsingOverlays.md for overlays.

## The file system on the drum

A drum volume is a set of entire tracks, the first track starts a 0 to 037, the last no greater than 037.
A track holds 16 blocks of 256 words, so a volume of n tracks has 16n blocks.
A block never crosses a track boundary.
Its layout is that which the file system defines: block 0 is unused,
the directory and its copy in blocks 1 and 2, the files from block 3 to the end of the allocation.
A volume of one track has 13 blocks for files.

A program includes `DRUM/drum.ah`, `drmount.ac`, and `drformat.ac` and the file system's `.ac` files for
the calls it uses.
A program can also simultaneously use the Microtape drive.

- Multipe volumes share the dru, each with tracks of its own, each with its own handle.
  The library does not know which tracks other programs or overlays use.
  The separation is the program's responsibility.
- The volume's first track is recorded in its directory and checked at every mount.
  A volume copied to other tracks, by a host tool for example, is refused there with
  `FS_ENOFS`, and a mount of the wrong tracks will also give an error.
- The drum blocks do not have a checksum, a block holds 256 data words.
- **Sequence breaks are off during every call** and turned on again at the end only if they
  were on initially.
  Every drum transfer ends with an automatic break.
  Before turning them on again the call clears every pending request with `cbs`.
  A program that turns them on later should do `cbs` first.
  **Never call the library from a break routine.**
- **Timing.** A transfer moves a word every 8.5 us, 2.2 ms a block after waiting up to a
  revolution for the drum to reach the first word to transfer, about 35 ms.
  A call moves a run of blocks with one transfer for each track it uses so `fsRead` and
  `fsWrite` of many words cost little more than one wait.
  A program that reads or writes one block a call waits most of a revolution each time, read
  and write in runs.
- **The Type 340 display** IOTs `dra` and `dss` conflict with drum IOTs of the same name,
  the filesystem layer uses `t23dra` to avoid conflicts.
  Include the Type 340's definitions after the file system's files.

### The layout

The volume's track k holds its block j, 0-15 within the track, at drum address
((j + k) mod 16) x 0400, a skew of one block a track.
A run crossing from one track to the next then starts its second transfer about one block after
the first ended, some 2 ms.
Without the skew the drum would have just passed it and the
transfer would wait a whole revolution.
A run within a track is one transfer which wraps past the track's last word if needed.

A host program reading a volume from the drum file finds word a of track t at
word t x 010000 + a, each word a little-endian 32-bit word, and applies the skew.

### The calls

Each is called va `farjda` n`memory.ah` from any bank.
AC is the 16-bit address of the parameter block, and AC had the status on return.
Every parameter block here is `FS_PHANDLE` the handle's 16-bit address, then `DR_PFIRST` the
first track, then `DR_PTRACKS` the tracks, and for `drFormat`, `DR_PFORCE`.

| Call | Parameter block | Result |
|---|---|---|
| `drFormat` | `FS_PHANDLE`, `DR_PFIRST`, `DR_PTRACKS`, `DR_PFORCE` nonzero to force formatting | the handle for an empty directory |
| `drMount` | `FS_PHANDLE`, `DR_PFIRST`, `DR_PTRACKS` | the handle |
| `drbSetHandle` | `FS_PHANDLE`, `DR_PFIRST`, `DR_PTRACKS` | the handle set for the volume: the first track, the device words, the header's first track; not mounted |
| `drbHRead`, `drbHWrite` | an `FS_B` block: the handle, the first block, the count, the buffer | the file system's entries, accessed through the handle by `fsBRead` and `fsBWrite` |

- **`drMount`** reads the directory from block 1, or from the copy in block 2 if block 1 is
  not valid.
  It refuses with `FS_ENOFS` a volume without a file system, one whose directory's block
  count is not 16 times the tracks given, and one whose recorded first track is not the one
  given.
  A refused mount leaves the handle not mounted and descriptors of any earlier mount refused.
- **`drFormat`** writes an empty directory as both entries with the first track in its header and
  mounts the handle for it.
  Without force it refuses with `FS_EFOREIGN` a volume whose blocks 1 and 2 hold another
  program's data.
  Tracks never written read as zeros and are formatted.
  File blocks are not touched.
- **`drbSetHandle`** lets a program move a volume's blocks through `fsBRead` and `fsBWrite`
  without mounting it regardless of whether they are in the filesystem or not.
  Refused tracks leave the handle unchanged.
- The block entries move entire blocks and split a run where one crosses a track and where
  its buffer crosses a core bank, since a transfer wraps within both.
  A buffer can across bank boundaries.
  A run's words must fit in core,a count of 0400 blocks or more is not handled.

### Status codes on the drum

The file system's codes are in `FILESYSTEM/README.md`.
These are the codes the drum layer returns.

| Code | Name | Meaning on the drum |
|---|---|---|
| 1 | `FS_ENOUNIT` | a first track not 0-037, no tracks, or a last track past 037 |
| 016 | `FS_ERANGE` | a first block not on the volume, a count under 1, or blocks running past the volume |
| 023 | `FS_EXFER` | a transfer didn't complete |

`FS_EXFER` has two causes which the drum cannot tell apart:
- **A halt after the transfer's first word**: (a `hlt`, the STOP switch, an ad1 stop or
  breakpoint, or a single step.
  The words before the halt are moved, the rest not.
  The call can be repeated, and succeeds.
  A halt before the first word only delays the transfer, and the call ends normally.
- **The host could not write the drum file.**
  A repeat fails the same way until the file can be written, a program should not repeat forever.

### An example

The file system example in `FILESYSTEM/README.md`, on drum tracks 030-037 instead of a tape.
```
File system example, on the drum

#include <DRUM/drum.ah>

100/
go,
    lsm
    cbs
    eem
    lac [hnd:1]
    dac pb
    law 030
    dac pb+DR_PFIRST
    law 010
    dac pb+DR_PTRACKS
    dzm pb+DR_PFORCE
    lac [pb:0]
    farjda(drMount:*)
    sad [FS_ENOFS]
    jmp fmt
    sza
    jmp fail
    jmp mk
fmt,
    lac [pb:0]
    farjda(drFormat:*)          // refused unless blocks 1-2 are zeros or a file system directory
    sza
    jmp fail

mk,
    // ... the file system's calls, as on a tape
    hlt
fail,
    hlt                         // AC = the status
    jmp go

pb,
    table 5
constants

bank 1
#include <FILESYSTEM/fsio.ac>
#include <FILESYSTEM/fscreate.ac>
#include <DRUM/drmount.ac>
#include <DRUM/drformat.ac>
constants
FS_HANDLE(hnd)
FS_FILE(fd)

start go:0
```

# See also

`FILESYSTEM/README.md`, the file system's calls.
`Docs/UsingType23Drum.md`, the drum itself.
