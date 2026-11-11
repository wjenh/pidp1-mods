## Using mtfs

Mtfs is a utility for managing Microtapes that are used with the Microtape file system
provided in the Am1Includes/MICROTAPE include directory, see it for more information.

This is version 1.0\
11-Nov-2026
Initial version.

## Usage

Mtfs directly modifies a tape image that contains a Microtape file system.

It lists the tape's files, extracts a file, adds a host file, deletes a file,
formats a tape, and packs a tape to recover space left from deleting files.
```
mtfs -l image
mtfs -a -n name [-t type] [-r] [-b blocks] [-f bytes|text|words] [-F] -s source image
mtfs -d -n name [-F] image
mtfs -x -n name [-f bytes|text|words] -o output image
mtfs -i [-F] image
mtfs -p image
```

| Option | Meaning |
|---|---|
| `-l` | list the directory |
| `-a` | add the host file `source` as `name` |
| `-d` | delete `name`, its blocks are freed, but not erased |
| `-x` | extract `name` to the host file `output` |
| `-i` | format creating an empty directory, generation 1; a missing image file is created |
| `-p` | pack the files to close gaps and recover space |
| `-n name` | the file's name |
| `-t type` | the type, octal 0-7777, default 0 |
| `-r` | the file is read-only |
| `-b blocks` | the blocks to allocate, octal, at least the file's data size and at most 0777, default is the size needed, one for an empty file |
| `-f form` | the host file's form, `bytes` by default |
| `-F` | force: add replaces a file of that name, delete removes a read-only file, format overwrites another program's data in blocks 1 and 2 |

Numbers are octal.
The image is an ordinary path, the last argument.
The exit status is 0 on success and 1 on any error, and an error gives the library's status code:
```
mtfs: tape.img: ALPHA: EXISTS, a file of that name is already on the tape
```
Warnings go to stderr and do not change the status.

**The forms.** A file's length is kept in words, so:
- `bytes`: two 8-bit bytes a word, the first in bits 0-8, as am1's `ascii` packs characters.
  An odd last byte leaves the low half 0.
- `text`: as `bytes`, with each line end, LF or CR LF, stored as CR (015).
  Extract converts each CR to LF and drops any trailing 0 bytes.
- `words`: each 18-bit word in a little-endian 32-bit word as the image file stores them.
  A value greater than 0777777 gives an error.
  Use this for a `.rim` image.

Extract refuses if the sum of an extracted block does not total zero, BADBLOCK, a checksum error.

A half word of 9 bits is extracted as `bytes` or `text` using only its low 8 bits
with a warning, `words` keeps all 9 bits.

**The listing**, in octal:
```
tape.img: generation 2, 01100 blocks, 01074 free, largest gap 01074
slot  name       type  app  ro  first  blocks    words
   0  ALPHA      0123   00         03      01        0
```
`type` is bits 6-17 of the information word and `app` bits 1-5 as set by a program.
`mtfs` leaves them 0.

It warns when block 1 was bad and the copy in block 2 was read, when a block of a file's data
does not total zero, a checksum error, and when an entry is marked as moving, which a halt during `mtfPack`
causes.

**Writing.** The image is changed in memory and written to a temporary file
which is renamed over it, so a failure leaves it unchanged.
It grows only to the last block written.
A read-only image file is not written, giving a LOCKED error.

**A running emulator.** The IOT mounts an image again at the next `mse` of its drive once
the file has changed, so a program that mounts the tape after `mtfs` has run sees the change.
But a program that already has the tape mounted keeps the directory it read in its tape
handle and its next directory write writes that copy back, undoing the mtfs change.
Modify a tape only between programs or have the program mount it again after the change.

When the image is listed in */opt/pidp1-mods/microtapes.txt*, `mtfs` gives a warning but it still writes.
