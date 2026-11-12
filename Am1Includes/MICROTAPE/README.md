## Include files for the Type 550 Microtape

This also documents the Microtape's layer of the file system.
The file system's calls are in `FILESYSTEM/README.md`.

This is version 2.0\
12-Nov-2026

## What's here?

- type550defs.ah, the six IOTs: `mmt`, `mse`, `mlc`, `mrd`, `mwr`,
  `mrs`, the unit numbers, the `mlc` control word and its modes:
   `mtfmt`, the status bits, the results of `mmt` and the search-word fields.
- `mmt` is a nonstandard extension, it mounts a tape image named by a program.
- The Microtape's layer of the generic file system (`FILESYSTEM/`), the tape's block layer and the
  tape-specific handle.
  Its routines are callable from any bank.

  | File | Contains |
  |---|---|
  | microtape.ah | the block layer's parameter block|
  | mtblock.ac | `mtbRead`, `mtbWrite`; `mtbHRead`, `mtbHWrite`, `mtbSetHandle` for the file system |
  | mtmount.ac | `mtMount` |
  | mtformat.ac | `mtFormat` |

See Docs/UsingType550Microtape.md for how to use the tape itself in a program.

## The file system on a tape

A tape is a volume of 576 blocks (01100, `MTBLKS`) of 256 words (0400, `MTWDS`, the file
system's `FS_BLKW`).
Its layout, its directory and its files are the file system's (`FILESYSTEM/README.md`).
Block 0 is left unused so a reverse search back to the directory completes without entering
the end zone.

A program includes `MICROTAPE/microtape.ah`, which brings in `FILESYSTEM/fs.ah`, then
`mtmount.ac` and `mtformat.ac` and the file system's `.ac` files of the calls it uses.
`FILESYSTEM/README.md` has an example.

- **Drives are 1 to 8.**
  A handle manages one drive.
  A program can mount several drives at once, each with its own handle (`FS_HANDLE`).
- **Sequence breaks are off during every call that moves the tape** and turned on again at
  the end only if they were on.
  The tape IOT automatically requests a break at every flag, so before turning them on again
  the call clears every pending request with `cbs`.
  **Never call the library from a break routine.**
- **A call takes** from a fraction of a second to about 30 seconds for a search from one end
  of the tape to the other, and leaves the tape stopped.

### The calls

Each is called as the file system's are: `farjda` (`memory.ah`) from any bank, AC the 16-bit
address of the parameter block, AC the status on return.

| Call | Parameter block | Result |
|---|---|---|
| `mtFormat` | `FS_PHANDLE` the handle, `MT_PDRIVE` the drive, `MT_PFORCE` nonzero to format the tape | the handle mounted on an empty directory |
| `mtMount` | `FS_PHANDLE`, `MT_PDRIVE` | the handle is mounted |
| `mtbRead`, `mtbWrite` | `MTB_PDRIVE`, `MTB_PBLOCK` the first block, `MTB_PCOUNT` the blocks, `MTB_PBUF` 0400 words a block | the blocks, read in forward order, checksums kept |
| `mtbSetHandle` | AC = the handle, IO = the drive, not a parameter block; called by `jda` | the handle set for the tape: the drive, the device words; not mounted |
| `mtbHRead`, `mtbHWrite` | an `FS_B` block: the handle, the first block, the count, the buffer | the file system's entries, reached through the handle by `fsBRead` and `fsBWrite` |

- **`mtMount`** reads the directory from block 1, or from the copy in block 2 if block 1 is
  bad, and refuses a tape without this file system with `FS_ENOFS`.
- **`mtFormat`** writes an empty directory as both copies.
  Without force it refuses with `FS_EFOREIGN` a tape whose blocks 1 and 2 hold another
  program's data; a blank tape is formatted.
  Files' blocks are not touched.
- **The block layer**, `mtblock.ac`'s `mtbRead` and `mtbWrite`, can be used alone, on any
  blocks of any tape, a file system's or not.
  It writes block 1 or 2 of a file-system tape as ordinary data blocks.
- **`mtbSetHandle`** lets a program move a tape's blocks through `fsBRead` and `fsBWrite`
  without mounting it; the drive is checked at the first transfer.

### Status codes on a tape

The file system's own codes are in `FILESYSTEM/README.md`.
These are the codes the tape's block layer returns, with what each means on a tape:

| Code | Name | Meaning on a tape |
|---|---|---|
| 1 | `FS_ENOUNIT` | the drive is not 1-8 |
| 2 | `FS_ENOMEDIUM` | no tape on the drive or it ran off its reel |
| 3 | `FS_ELOCKED` | a write to a write-locked drive |
| 4 | `FS_EHALTED` | a halt stopped the tape, the call can be repeated |
| 5 | `FS_EBADBLOCK` | a block did not total zero which is a checksum error or a word was missed |
| 6 | `FS_EENDZONE` | the tape ran into an end zone |
| 016 | `FS_ERANGE` | a first block not 0-01077, a count under 1, or blocks running past 01077 |
| 022 | `FS_ETIMEOUT` | a search saw no block mark, the tape couldn't be read |

# See also

`FILESYSTEM/README.md`, the file system's calls, and an example on a tape.
UsingMtfs.md in the Docs directory for a description of mtfs, a file system maintenance utility.
