# SHRDLU as a native Linux program

The PDP-1 SHRDLU's code as one Linux executable: no emulator, no drum, no room to run out of.
The interaction is via a telnet connection, the display is the Type 340's point stream, which
the current `t30dpy` shows as a pdp-1 program would generate.

## Why?

It provides an interesting comparison between what was possible then and what can be done on
a modern cpu, even on one like a Raspberry Pi that costs about what a single roll of paper tape
cost then, in adjusted dollars.

Imagine how much longer the Symbolist camp could have continued if modern processing power and memory
was available then.

Oh, so, how much faster?

Abou 100,000 times faster on a not-very-current x64 Linux box.
A single Raspberry pi-4 is faster than all the PDP-10s ever built running simultaneously and has
more memory than existed on the entire planet when SHRDLU came out.

## Building and running

```
make
./shrdlu
telnet localhost 2042
t30dpy
```

Gcc and POSIX threads are all it needs.
The compiler takes about 15 seconds, most of it for `code.c`.
| Option | What |
|---|---|
| `-p PORT` | the telnet port, 2042 by default |
| `-d PORT` | the display's port, 3400 by default (`t30dpy`'s `-p` reaches another) |
| `-f` | the hand jumps directly to its destination |
| `-s SEED` | the random numbers' first state, 1 by default |

Only one client can connect at a time.
A client that terminates resets the program.
A new connection gets the banner and the initial world.
A display client that connects shows the scene as it currently is.

What it prints wraps at a space as the PDP-1 program's does.
The banner and the `WORDS:` echo.
All other output is what the program issues.
Teplies are no more than 79 characters a line, what you type excepted.
The output of '/' commands is not wrapped.
This follows the behavior of the assember program.

The environment variable `SHRDLU_GC_CELLS`, makes the program garbage collect every time
that many cells are used.
Without it collections are rare, the whole dialog runs without one.
It's really just for testing.

A two-letter color label `RD`, `GR` or `BL` shows the color of all the objects except the box and the table.
The labels are never clipped, so stay readable wherever an object is.
The environment variable `SHRDLU_NOLABELS=1` turns them off.

## What is in it

| File | What |
|---|---|
| `main.c` | The program's start, its big stack, and the stack's guard. |
| `term.c` | The telnet side, the line reader, and the `/` commands. |
| `top.c` | One sentence or one goal. |
| `scene.c` | The world as seen, the projection, the hidden lines, the hand and the point stream. |
| `rt.c`, `rt.h` | The Lisp run-time: values, cells and their collector, symbols, dynamic binding, non-local exits, an evaluator and the primitives. |
| `code.c`, `image.c` | The original's Lisp as C, and the Lisp world as loaded, generated from the original source. |

The original's grammar, semantics, answers and Micro-Planner run as compiled C.
The blocks world's theorems stay data and the compiled Micro-Planner runs them through the evaluator.

## How fast

The 42 sentences of the dialog take 16 ms of parsing and answering in all, and the slowest, 37,
1.3 ms, on a low-end x84 Linux.
The PDP-1 program takes a mean of 24.9 s a sentence of compute,
The hand takes one step on every frame, as the PDP-1 program's does,
so a command takes as long to watch as it does there.

## Where it differs from the PDP-1 program

The replies, the plans and the placements are the PDP-1 program's, and so is everything typed at
the reader.
What differs:

- **`NO ROOM` is gone.** So are the drum, the code cache and the arenas, so a sentence that ran out
  of room there goes on here, and `/ROOM` gives this program's cells, collections and events.
- **A sentence that runs away is abandoned** after 10 s of the processor, or at 32M cells live, or
  at a stack of about 1 GiB, as one not understood: the reply is `I DON'T UNDERSTAND.`, or `I CAN'T`
  to `/DO` and `/TRY`. A `/DO` that is stopped leaves the world as it was. A sentence that is
  stopped may leave the planner's picture of the world part way through a plan: `/RESET` puts it
  right. A clarifying question that keeps being answered and keeps coming back stops the same way
  after four answers.
- **`/TIMES`** has a second line, `MICROSECONDS PARSE p ANSWER a`, in decimal, since this program's
  sentences rarely take a whole millisecond. Neither line counts the hand's pace or the user's
  typing.
- **`/RESET`** puts everything back as at the start, learned words and dialog memory too.
- **`/TRACE ON`** is accepted and adds nothing.
- **`/FIND`** lists the same facts in another order.
- **`/TREE`**'s unit lines have the same features, sometimes in another order.
- **The scene** has about one point in 250 more, a corner of a diagonal that the emulator's stepping
  leaves dark when its x and y steps all but coincide.
