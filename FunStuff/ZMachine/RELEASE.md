# Releasing the PDP-1 Z-machine

This directory is the release tree, distributed as `FunStuff/ZMachine`. The
shipped sources in `ZMachine/Interpreter/` and `ZMachine/Loader/` are kept to
the coding standards (`Claude/AM1-CODING-STANDARD.md`,
`Claude/C-CODING-STANDARD.md`), so a release is a plain copy of them, never a
cleaned copy. This file and nothing else here describes how; it is left out
of the distributed tree.

## The owner's rules for the release tree (19-Sep-2026, standing)

- An installation has the am1 system include directories, `am1` on PATH, and
  `/opt/pidp1-mods`; the Makefiles' defaults stand.
- No binaries ship: `zloader` and `zmachine.rim` are built by `make` on the
  target.
- Test-harness code (`#if Z_TEST_HARNESS` blocks) ships.
- `ZMachine.md` is the release README and the owner's file: Claude offers
  text for it, and does not edit it. Its note on credit and on Claude stays
  as is.
- LF line endings only, no tabs outside Makefile recipes.

## What ships

Release path, relative to this directory, and its source, relative to
`ZMachine/`:

| Release file | Source |
|---|---|
| `Makefile` | kept here (builds both halves) |
| `ZMachine.md` | kept here (the owner's README) |
| `Interpreter/Makefile` | `Interpreter/Makefile` |
| `Interpreter/zmemcore.am1` | `Interpreter/zmemcore.am1` |
| `Interpreter/*.ac`, all 16 | `Interpreter/*.ac` |
| `Loader/Makefile`, `zloader.c`, `zloader.h` | `Loader/` |
| `Games/zork1.z3`, `zork2.z3`, `zork3.z3` | Microsoft's MIT-published story files |
| `Games/LICENSE-zork-microsoft.txt`, `Games/MANIFEST.md` | kept here |

Every `.ac` file ships because `Interpreter/Makefile`'s `SRCS` is
`$(wildcard *.ac)`, and `zmemcore.am1` includes every one of them.
`bitsets.ac` comes from the am1 include root (`UTIL/`), not from here.

Not shipped: the Python tools in `Interpreter/` and `Loader/`,
`Interpreter/Font3/` (the generator and template `zfont3.ac` is made from),
any binary, the source `Games/` disc files, `StoryFiles/`, `SaveTapes/`,
`Claude/`, `CompletedTasks/`, `PendingTasks/`, `History/`, `DESIGN.md`,
`NEXT-STEPS.md`, and this file. A shipped source must not point at any of
them.

The games, each verified at its declared length, which is also the file's
length:

| file | release | serial | md5 |
|---|---|---|---|
| `zork1.z3` | 119 | 880429 | 1d4606016ea58ee038da53d994392323 |
| `zork2.z3` | 63 | 860811 | e3fd4c0a04481c2b5f66fbee163d92a1 |
| `zork3.z3` | 25 | 860811 | 83fbb273398fe3c7b5f731188b1958cc |

## The procedure

1. **The gate.** The image to be released has passed the full sweep, run at
   the owner's direction, or the owner has ruled that it need not.
2. **Copy.** Copy each source in the table over its release file, and check
   each pair with `cmp`. List the release tree's `Interpreter/` and `Loader/`
   and remove any file the working tree no longer has.
3. **Check the tree**, in a private WSL directory, never the checkout:
   - `make` in a copy of this directory without this file, with `AM1` and
     `AM1INCDIR` pointing at the checkout's `bin/am1` and `Am1Includes` (the
     WSL `/opt` copy is stale on the development machine). It builds
     `zloader` and `zmachine.rim` with no errors;
   - that `zmachine.rim` has the md5 of the checkout's production image,
     `am1 -b -d -S -DZ_TEST_HARNESS=0`, built with the same `bin/am1`;
   - `make -C Interpreter story DRUM=<scratch file> STORY=../Games/zork1.z3`
     installs Zork I on a scratch drum, never the real `pdp23drum`. The
     `story` target is in `Interpreter/Makefile`, not the top-level one;
   - the greps: no CR, no tab outside the Makefiles, no reference to a file
     that does not ship.
4. **Package** this directory as `FunStuff/ZMachine`, leaving this file out,
   when the owner asks for it.

Record the release in the task that makes it: the `bin/am1` md5, the image
and loader md5s, and each copied source's md5.
