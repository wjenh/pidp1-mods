# CalComp plotter -- tests

The host tests need no pidp-1.
`pngtest` drives the PNG encoder (`../pngWrite.c`) directly.
`plugintest` includes the IOT source, ../IOT_73.c, in a host program that stands in for the pidp-1.
It supplies the clock, the deadline, the break request and the
configuration, and calls the IOT entry points as the IOT loader would.
`plottest.am1` runs on the pidp-1.

`IOTs/install.sh` skips directories named `Tests`, nothing here is built or installed by it.

## Running

    make              # pngtest, then plugintest
    make pngtest      # the encoder
    make plugintest   # the plugin
    make plottest     # assembles plottest.am1
    make clean

Each test program prints a PASS or FAIL line for every check and a
summar, and exits 0 only if every check passed.
Each writes its PNGs with a PBM of the picture each should hold and a manifest listing them
into a subdirectory of its own here, `t1.d`, `plugintest.d`, and the Makefile
then runs `checkpng.py` over the manifest.
`plugintest` is built with the IOTs's logging on and reads `/tmp/iot.dbg` for the busy report.
Run it with no `pdp1` running.

## checkpng.py

Decodes a PNG with Python's  `zlib` and `struct` (no PIL) and
compares it with a plain PBM (`P1`), every chunk's CRC, the zlib stream
and its Adler-32, the `pHYs` density, the size and every pixel.

    python3 checkpng.py file.png file.pbm --ppm 3937
    python3 checkpng.py --manifest manifest.txt
    python3 checkpng.py --selftest file.png file.pbm 3937

A manifest line is `png pbm ppm`, or `png pbm ppm fail` for a control
that must not match.
`--selftest` proves the checker can fail: a pixel flipped, a byte of the image data changed (the CRC fails),
the Adler-32 changed with the CRC repaired (the Adler-32 fails),
a byte of the deflate data changed with the CRC repaired (some check must fail), and a wrong density.

## What pngtest covers

A blank sheet, one pixel, a diagonal, a width that is not a multiple of 8,
the 560's and 563's widths, a long 563 sheet over many deflate blocks,
a sheet of noise that deflate cannot shrink,
that the line-art sheets are compressed to at most an eighth of their image data,
and the encoder's refusals (a zero width or height, a picture too big).
The encoder links with zlib (`-lz`, the `zlib1g-dev` package).

## What plugintest covers

- Decoding: every value of the command bits, with and without `i` and `c`, with junk in IO.
- Timing: the step and pen times of each model, diagonals, a pen command
  with a step, completion only when the IOT asked for it, the `cks` flag
  falling at the IOT and rising at the deadline, the break, and the
  settings that change them.
- A command while busy: ignored, completed at once if it asked, and reported.
- The paper: the carriage's edges, the drum far both ways, the sheet's height.
- Writing: the idle write, the writes at a halt, at STOP and at power
  clear, the `.tmp` and rename, `%d` numbering, and the picture's orientation.

## plottest.am1

Draws a square, a star of eight rays, a dot and a line drawn by polling the flag.
It times 20 pen changes and 1000 steps with the timesharing clock (`iot 32`)  against a delay loop of known length,
reads the flag before the IOT loads, after `plnop` and during a step, and counts the breaks over 10 steps.
It halts at 130 with its results at 6000-6013.
Built with `-DNODRAW` it only loads the IOT which must leave a blank one-row sheet.

Run it with `plottersbs=1` in pidp1.config to count the breaks.
With the 560's timing, it takes about 17 seconds.
