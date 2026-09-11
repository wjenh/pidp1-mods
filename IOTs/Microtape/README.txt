This is an implementation of the DEC Type 550 Microtape Control with up to eight Type 555
Microtape drives, implemented from the 1963 DEC manual (Magtape/550_prelimManual.pdf).

See Docs/UsingType550Microtape.md for how to use it, and TYPE550_IOTs.txt for the IOTs.

The tape IOTs share device 01 with the paper tape reader's rpa and are told apart by the
sub-device field, instruction bits 7-11.
The tape has no plugin of its own, the reader (IOTs/Reader/IOT_2.c) hands the device-01 IOTs
with sub-devices 2-7 to microtape.c, and forwards its I/O poll, start, stop and update calls.

Files
  microtape.c/.h   the shim in the reader plugin: the IOTs, the I/O poll, microtapes.txt, and the
                   mount IOT 201
  control550.c/.h  the Type 550 control: flags, modes, what happens at each word of a block
  transport555.c/.h the Type 555 drive: motion, word timing, the tape image and its file
  mkmicrotape.c    the image tool: blank, check, and simh 18b import/export
  TYPE550_IOTs.txt the IOTs in brief
  Tests/           the test suite

Build
  IOTs/Reader/Makefile compiles microtape.c, control550.c and transport555.c from here into
  IOT_2.so, make install here builds only mkmicrotape.

Configuration
  /opt/pidp1-mods/microtapes.txt, one line per drive:
      1 microtapes/tape1.img
      2 /home/pi/tapes/system.img,locked
  The drive is 1-8 in decimal.
  A relative path is relative to /opt/pidp1-mods.
  A missing unlocked image is created as a blank tape.
  pidp1.config microtapesbs=n  sets the sbs channel to use

Images
  A full image is 576 blocks of 258 words, 594,432 bytes.
  A shorter file holds the first blocks and the rest of the tape is blank.
  An empty file is a blank tape.
  The file grows with no holes as blocks beyond its end are written and mode 7 truncates it.
