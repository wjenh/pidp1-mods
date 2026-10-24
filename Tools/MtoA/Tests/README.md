# mactoam1 -- tests

A program's observable result is the words it loads and its start address, so
the test of a conversion is that am1 loads exactly the words macro1 loads. No
emulator run is involved.

For each source the kit assembles the original with macro1 (built from this
checkout's `src/macro/macro1_1.c`, as a plain RIM tape with `-r`), converts it
with `mactoam1`, assembles the conversion with `am1 -T`, and compares the two
with `m1cmp.py`.

## Running

    ./run_tests.sh                      # everything: t1 t2 t3 control
    ./run_tests.sh t2 t3                # some of it
    ./run_tests.sh -m path/to/mactoam1  # a converter already built
    ./run_tests.sh -w ~/mtoatest        # where everything is written

Without `-m` the converter is built from `Tools/MtoA` into the work
directory. `-a` names the am1; the default is `/opt/pidp1-mods/bin/am1`. The
work directory defaults to `./mtoatest` under the current directory: macro1
and am1 write their outputs into their working directory, so run it from
somewhere other than the checkout, or give `-w`. The checkout itself is only
read.

It needs gcc and python3, and flex and git for the control legs. It prints a
line per check and a summary, and exits 0 only when every check passes. The
whole kit takes under a minute.

## What each part covers

- **t1, the corpus** (`corpus.txt`): every `.mac` file in the checkout outside
  `Projects/`. Each line names the file and what is expected of it: the same
  words (`words`; `words-lf` for a file whose lines end in a bare CR, which
  macro1 is given with LF; `words-title` for a file whose first line is code,
  which is given a title line), a clean am1 assembly where macro1 cannot
  assemble the original (`am1`), or a clean refusal (`refuse`). The corpus is
  never edited; the kit works on copies.
- **t2, the construct fixtures** (`fixtures/*.mac`): one small program per
  construct the converter maps, each through the same compare.
- **t3, the refusals** (`refuse/*.mac`): one program per construct the
  converter refuses, its refused line marked `/ refused`. The converter must
  exit nonzero, name that line first on stderr, and write output am1 refuses
  with the converter's message. Then the program with the marked line removed
  must convert and pass the compare, which shows the refusal comes from that
  line.
- **control**: the checks must be able to fail. The old flex converter, built
  from the last git commit that has it, must fail the compare on
  `maindec1_14.mac`, `maindec1_20.mac` and each fixture whose title says "old
  fails". A word changed by hand in a passing conversion must fail the
  compare, once in plain code and once in a constants pool.

## The compare

`m1cmp.py file.rim file.lst file.dmp` decodes macro1's tape, reads where each
constants pool starts from macro1's listing, and reads am1's `-T` output. Every
word must be the same at the same address, and so must the start address (a
tape with no start matches am1's `stop`). The exception is a constants pool:
am1 lays a pool out in its own order and may give equal values separate
slots, so a pool's words may be in any order, and a word that names a pool
slot may name another slot holding the same value. Each difference is printed.
