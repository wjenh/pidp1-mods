## Using fstool

Fstool is a utility for managing the file system provided by the Am1Includes/FILESYSTEM
on a Microtape or Type 23 drum image,
see Am1Includes/MICROTAPE or DRUM for more information.

This is version 1.2\
13-Nov-2026
Numbers can be decimal, octal, hex, or binary

## Usage

Fstool directly modifies a tape image or a drum file that contains the file system.\
Numbers can be entered as decimal, octal, hex, or binary.\
If the first digit is *not* a 0, it is decimal.\
If the first digit is 0 followed by a digit 0-7, it is octal.\
If the first 2 digits are 0x, it is hexadeclimal, 0-9a-fA-F are allowed.\
If the first 2 digits are 0b, it is binary and only the digits 0 and 1 are allowed.

It lists the files, extracts a file, adds a host file, deletes a file,
formats a tape or a drum volume, and packs it to recover space left from deleting files.
```
fstool [-D first,tracks] -l image
fstool -D all -l drumfile
fstool [-D first,tracks] -a -n name [-t type] [-r] [-b blocks] [-f bytes|text|words] [-F] -s source image
fstool [-D first,tracks] -d -n name [-F] image
fstool [-D first,tracks] -x -n name [-f bytes|text|words] -o output image
fstool [-D first,tracks] -i [-F] image
fstool [-D first,tracks] -p image
```

| Option | Meaning |
|---|---|
| `-l` | list the directory |
| `-a` | add the file `source` as `name` |
| `-d` | delete `name`, its blocks are freed, but not erased |
| `-x` | extract `name` to the host file `output` |
| `-i` | format creating an empty directory, generation 1; a missing image file is created |
| `-p` | pack the files to close gaps and recover space |
| `-n name` | the file's name |
| `-t type` | the type, decimal 0-4095, octal 0-7777, default 0 |
| `-r` | the file is read-only |
| `-b blocks` | the blocks to allocate, at least the file's data size and at most 511, octal 0777, default is the size needed, 1 for an empty file |
| `-f form` | the host file's form, `bytes` by default |
| `-F` | force: add replaces a file of that name, delete removes a read-only file, format overwrites another program's data in blocks 1 and 2 |
| `-D first,tracks` | the image is a drum file and the volume is tracks `first` to `first+tracks-1` |
| `-D all` | with `-l` only: list every volume on the drum file |

'image' is the image file, a tape image or  with `-D` a drum image.
The exit status is 0 on success and 1 on any error, and an error gives the library's status code:
```
fstool: tape.img: ALPHA: EXISTS, a file of that name is already on the tape
```
Warnings go to stderr and do not change the status.

**The forms.** A file's length is kept in words:
- `bytes`: two 8-bit bytes a word, the first in bits 0-8, as am1's `ascii` packs characters.
  An odd last byte leaves the low half 0.
- `text`: as `bytes`, with each line end, LF or CR LF, stored as CR (015).
  Extract converts each CR to LF and drops any trailing 0 bytes.
- `words`: each 18-bit word in a little-endian 32-bit word.
  A value greater than 262143, octal 0777777, gives an error.
  Use this for a `.rim` image.

Extract refuses if the sum of an extracted tape block does not total zero, BADBLOCK, a checksum error.
A drum block has no checksum.

A half word of 9 bits is extracted as `bytes` or `text` using only its low 8 bits
with a warning, `words` keeps all 9 bits.

**The listing**, always in *decimal*, except for the application word, which is octal.:
```
tape.img: generation 2, 576 blocks, 573 free, largest gap 572
slot  name       type  app  ro  first  blocks    words
   0  ALPHA        83   00          3       1        0
```
`type` is bits 6-17 of the information word and `app` bits 1-5 as set by a program.
`fstool` leaves them 0.

It warns when block 1 is bad and the copy in block 2 was read, when a block of a file's data
does not total zero, which is a checksum error, and when an entry is marked as moving, which a halt during `fsPack`
causes.

**Writing.** The image is changed in memory and written to a temporary file
which is renamed over it, a failure leaves it unchanged.
An image grows only to the last block written.
A read-only image file is not written, giving a LOCKED error.

**A running emulator.** The IOT mounts a tape image again at the next `mse` of its drive once
the file has changed.
A program that mounts the tape after `fstool` has run sees the change.
But, a program that already has the tape mounted keeps the directory it read in its tape
handle and its next directory write writes that copy back, undoing the fstool change.
Modify a tape only between programs or have the program mount it again after the change.

When the image is listed in */opt/pidp1-mods/microtapes.txt*, `fstool` gives a warning but it still writes.

## Drum volumes

A volume on the drum is a run of entire tracks as a program gives them to `drMount` and `drFormat`.
The first number is the starting track, the second is the number of tracks.\
Thus, the ending track will be (starting + number of tracks - 1).\

For example, -D 030,010` is the volume of tracks 030 to 037, 030 + 010 - 1).\
Both numbers are octal because of the leading 0.\
The drum file must be named, there is no default.\
The emulator uses */opt/pidp1-mods/pdp23drum*.\
More examples:
```
fstool -D 030,010 -i pdp23drum
fstool -D 20,10 -a -n notes -f text -s notes.txt pdp23drum  Decimal track 20, decimal 10 tracks long
fstool -D 0x5,10 -l pdp23drum Hex track 5, decimal 10 tracks long.
pdp23drum tracks 030-037: generation 2, 0200 blocks, 0173 free, largest gap 0173
slot  name       type  app  ro  first  blocks    words
   0  NOTES      0000   00         03      02     0412
```
A volume has 16 blocks a track, 13 for files on a volume of one track.
Messages show the volume the same as the listing does.

The volume's directory records its first track and a volume is only used over the tracks it was formatted on.
One whose directory records another track, a raw copy of another volume, 
or a volume named with the wrong number of tracks give a NOFS error.

Format writes the first track into the directory.
It refuses, unless forced, tracks whose blocks 1 and 2 hold another program's data or a directory of another size.
Tracks never written read as zeros and format without force.

**Finding the volumes.** `-D all -l` lists each track whose directory, in block 1 or its copy
in block 2, records that track as the volume's first:
```
fstool -D all -l pdp23drum
pdp23drum: tracks 030-037, generation 2, 0200 blocks, 1 file, 0173 free, largest gap 0173
```
`-D first,tracks -l` lists a volume's files.
A volume starting inside one listed before it gives a warning, one of the two is left from an
earlier format.
With no volume on the drum, a NOFS error is given.

**The drum file.** It holds 040 tracks of 010000 words, each in a little-endian 32-bit word and a
file shorter than the drum capaciy reads as zeros past its end.
Every word outside the volume's tracks is left unchanged.
The file grows only to the end of the last track written, a format of a volume past the
file's end lengthens it to the end of the volume's first track.
A word of more than 18 bits on the volume's tracks, or a file that is not a whole number of
words, or is longer than the drum, is refused as not a drum file.

**A running emulator.** The drum reads its file again at the first transfer after the file is
replaced, a program that mounts the volume after `fstool` has run sees the change.
A program that already has it mounted keeps its directory in its handle, and its next
directory write undoes the change and a transfer the drum makes while `fstool` runs can be lost as
well.
Change a volume only while the loaded program is not using the drum.
When the drum file is */opt/pidp1-mods/pdp23drum*, a command that writes gives a warning.
still writes.

`drumlist` lists each volume on the drum with its tracks as well as the drum's other tenants.
