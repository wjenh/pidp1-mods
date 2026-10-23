# Using the CalComp plotter

This document describes the emulated CalComp incremental drum plotter on device 73, and how a
program draws with it.

This is version 1.1\
Edit date 24-Oct-2026
Fix typos

## What is it?

This is an emulation of the Calcomp plotters DEC provided for the PDP-1.

A drum plotter draws on a roll of paper with one pen.
The paper runs over a drum that turns a small step at a time, forward or back,
and the pen rides on a carriage that steps left or right across it.
Every motion is one step, 0.01 inch, and the pen can be lowered onto the paper or lifted off it.
Two motions at once give a diagonal step. Anything else,
a line at another angle, a circle, a letter, is drawn by the program as a staircase of steps.

DEC sold a "Graph Plotter and Control" for the PDP-1 with three CalComp models according to the
PDP-1 price list from June 1964:

| Model | Paper | Plotting width | Speed | Step time |
|---|---|---|---|---|
| 560 | 12 inch | 11 inches, 1100 steps | 12,000 steps a minute | 5.0 ms |
| 565 | 12 inch | 11 inches, 1100 steps | 18,000 steps a minute | 3.33 ms |
| 563 | 31 inch | 29.5 inches, 2950 steps | 12,000 steps a minute | 5.0 ms |

The IOT draws on a bitmap of one pixel per step and writes it as a PNG file at 100 dots an
inch; it prints at the paper's true size.

## Where the design comes from

What is DEC's, CalComp's or IBM's:
- the three models, their speeds and the paper widths from DEC's price list, CalComp's 565 and 566
  manuals, IBM's 1627 manual, which was IBM's name for the 565 and 563, for the plotting widths, the Type
  350's description for the 563's paper, which DEC's price list calls 29-inch;
- the timing: a step in 5 ms or 3.33 ms, a diagonal in the same time, a pen change in 100 ms
  from IBM 1627 manual and CalComp's "5 up and 5 down per second";
- the command set from DEC's Type 350 Incremental Plotter Control for the PDP-7, PDP-7 User Handbook,
  page 106, which drove the same CalComp plotters.
  One movement per pulse, pulses in a combined group, and a done flag.

It is implemented as IOT 73.
No document giving the actual IOTs of DEC's PDP-1 plotter control has been found.
CalComp's 566 manual says DEC supplied the PDP-1's interface,
and the DECUS library had a PDP-1 CalComp plotting package, No. 81, so the hardware existed.
If its IOTs turn up, the plugin will follow them.

DEC lists IOT 73 as the Type 51 magnetic tape control, which is not supported.
The 560's step, 0.01 inch, is inferred too, no source found gives it but every other
12,000-step-a-minute CalComp model has that step size.

## The IOT

The IO register is not used.
Bits 7-8 of the instruction choose a group and bits 9-11 the pulses in it.
These are the Type 350's three device codes and their pulses.
The names, in *Am1Includes/PLOTTER/plotterdefs.ah*, are the Type 350's in lower case:
| Instruction | Name | Action |
|---|---|---|
| `721473` | `plpu` | pen up |
| `722173` | `plpr` | pen right (carriage +1) |
| `722273` | `pldu` | drum up (paper up, drum +1) |
| `722473` | `pldd` | drum down (drum -1) |
| `723173` | `plpl` | pen left (carriage -1) |
| `723273` | `plud` | drum up, the same as `pldu` |
| `723473` | `plpd` | pen down |
| `720073` | `plnop` | nothing, completes at once |

Pulses in the same group combine in one instruction:
| Instruction | Names | Action |
|---|---|---|
| `722373` | `plpr pldu` | pen right and drum up, one diagonal step |
| `722573` | `plpr pldd` | pen right and drum down |
| `723373` | `plpl plud` | pen left and drum up |
| `723573` | `plpl plpd` | pen down, then a step left |
| `723673` | `plud plpd` | pen down, then a step drum up |

Pen left with drum down is not one instruction, give `plpl` then `pldd`.
Names from different groups cannot not be combined; `plpl pldd` is `723573`, pen down and a step left.
Drum up with drum down (`722673`) does not move the drum at all.

Each plotter IOT starts the plotter and returns at once.
As with other devices:
- `i` (`010000`) waits until the plotter has finished, `plpr i`.
- `c` (`004000`) asks for a completion pulse which a later `ioh` waits for.
- Neither, the program continues and must not give the next command until the proper delay has passed.

The group 1 pulses 01 and 02 (the Type 350's skip and clear flag) are reserved and do nothing.

## Is the plotter free?

`cks` bit 7 (`002000`, `pltcks` in the include file) is 1 when the plotter is ready for a command,
0 from the start of each command until it has finished.
The include file has `PLT_WAIT_READY`, which waits for completion, leaves AC alone and changes IO.

Until a program has given an initial command, bit 7 reads 0.
Start a plotter program with `plnop`, that initializes the plotter and does nothing else.

The bit can be moved with `plottercks` in pidp1.config, since the actual `cks` assignment isn't known.

## How long does each command take?

| Command | Time |
|---|---|
| A step, straight or diagonal | 5 ms (560, 563), 3.33 ms (565) |
| Pen up or pen down | 100 ms, even when the pen is already there |
| A pen command and a step together | the pen time, then the step time: 105 ms on the 560 |
| Nothing, `plnop`, the reserved pulses | completes at once |

As usual, the actual time is emulated, a program draws at the speed it drew on a real PDP-1
and keeps running in a halt, a step under way when the emulator stops still finishes.

## What if a program does not wait?

A command given while the plotter is busy is lost, nothing moves, and the command in progress
is unaffected.
The real plotter did the same; a step pulse that came too soon was dropped.
If the lost command asked for a completion (`i` or `c`) it gets one at once so a careless program
loses steps but never hangs.
A program that waits, or checks `pltcks`, never loses one.

## Sequence breaks

With `plottersbs` set to a channel, 1-15, the plotter requests a break on that channel each time
a command finishes.
By default it requests none.
Without the 16-channel break system, every channel's break goes to the first channel.

## The paper

The pen starts at the left edge of the carriage, drum position 0, pen up, where an operator
would have set it.
`plotterorigin` starts it farther right.

The carriage stops at the paper's edges.
A step past either edge does not move the pen, but still takes the usual time.
The drum has no ends, the paper is a roll, and the program can run it as far as it likes either way.
Memory is used only where the pen has drawn.

With the pen down, each step marks the paper as it moves, lowering the pen leaves a dot.
The pen is one step wide; `plotterpen` makes it wider, a round pen of that many steps.

## The PNG file

The picture is the paper as you would read it off the plotter: the carriage across, pen right
to the right; the drum down the page, drum up moving the pen toward the bottom.
It is the plotting width wide, and as tall as the drawing plus 10 rows above and below.
A sheet with nothing drawn is one blank row so you can tell "drew nothing" from "never ran".
It is 1 bit a pixel, black ink on white, uncompressed, about 14 KB for each inch of 560 paper.

The sheet is written:
- when the plotter has been idle 5 seconds after drawing (`plotteridle`) so you can watch a long
  plot grow in a viewer that reloads the file;
- when the machine halts via a `hlt` or the STOP switch;
- at power off.

It is written to `plotterfile.tmp` and renamed over `plotterfile` when complete.
The file is *plotter.png* in the /opt/pidp1-mods's directory unless `plotterfile` gives another name and location.

A new sheet starts at the first plotter IOT after the power comes on.
Without `%d` in `plotterfile`, it replaces the last sheet's file.
With `%d`, each sheet gets its own number: `plotterfile=/home/pi/plots/sheet%d.png`.

## Settings

In pidp1.config, read when the IOT loads and again if a reload command is given.

| Setting | Meaning | Default |
|---|---|---|
| plottermodel | 560, 563 or 565 | 560 |
| plotterstep | the step time in microseconds | the model's default |
| plotterpendown | the pen-down time in milliseconds | 100 |
| plotterpenup | the pen-up time in milliseconds | 100 |
| plotterfile | the PNG's path; a `%d` numbers the sheets. At most 63 characters, no spaces | plotter.png |
| plotteridle | seconds idle after drawing before the sheet is written, 0 for never | 5 |
| plotterpen | the pen's width in steps, 1-50 | 1 |
| plottersbs | the sequence break channel, 1-15; 0 or off for none | off |
| plottercks | the `cks` bit of the plotter's free flag, 0-17 | 7 |
| plotterorigin | where the pen starts, in steps from the left edge | 0 |

A new `plottermodel` changes the paper width beginning wi the next sheet.

## An example

This draws a triangle 200 steps wide and 100 tall, two inches by one, waiting for each command.

```
Plotter example: a triangle
#include <PLOTTER/plotterdefs.ah>

100/
go, plnop                   // loads the plugin, so pltcks means something
    plpd i                  // pen down, and wait for it
    law i 0d100
    dac cnt
up, plpr pldu i             // 100 steps right and up, one diagonal each
    isp cnt
    jmp up
    law i 0d100
    dac cnt
down,   plpr pldd i         // 100 steps right and down
    isp cnt
    jmp down
    law i 0d200
    dac cnt
back,   plpl i              // 200 steps left, back to the start
    isp cnt
    jmp back
    plpu i                  // pen up
    hlt                     // the halt writes the sheet

cnt, 0

    start go
```

It takes about 2.2 seconds.\
*IOTs/Plotter/Tests/plottest.am1* draws more, polls the flag, and uses the break system.

## What is not emulated

- The front panel controls that move the pen by hand and paper loading.
  Each sheet starts clean, with the pen at its start.
- More than one pen, or a change of pen color.
- The 564 and 566, which had 0.005-inch steps.
