# SHRDLU as a native Linux program

This is version 1.0\
2-NOV-2026

This was done to show what could have been done if modern hardware had been available when
Winograd was implementing the original SHRDLU.

It is essentially the PDP-1 SHRDLU in C as one Linux executable, no emulator, no drum, no memory to
run out of.
It uses the same telnet connection and renders the same Type 340 point stream.
This allows a direct comparison.

The result?

It is over 100,000 times faster.

If a modern Pi had been available, at a cost about the same as a roll of paper tape for the tape punch,
but thousands of times faster and with vastly more memory, the AI world would probably be a lot different
today.

## Building and running

```
make
./shrdlu
telnet localhost 2042
t30dpy
```

Gcc and POSIX threads are all it needs.
The compiler takes a while, most of it compiling `code.c`.
It's still a big program, after all.

| Option | What |
|---|---|
| `-p PORT` | the telnet port, 2042 by default |
| `-d PORT` | the display's port, 3400 by default,`t30dpy`'s `-p` if you want another |
| `-f` | the hand jumps immediately instead of the slower move |
| `-s SEED` | the random number generator first state, 1 by default |

A telnet disconnect drops any pending input and the world is reset for the next connection.

The environment variable `SHRDLU_GC_CELLS`, makes the program garbage collect every time
that many cells are used.
Without it collections are rare, the whole Winograd demo dialogue runs without one.

While the program waits for you to start a line, a two-letter color label (`RD`, `GR` or `BL`)
is drawn beside each colored block and pyramid, as in the PDP-1 program.
The first character you type takes them off, and they come back at the next line.
The environment variable `SHRDLU_NOLABELS=1` turns them off, which the tests use.

What it prints wraps at a space as the PDP-1 program's does.
The banner, the `WORDS:` echo and the replies are no more than 79 characters a line, the line you type excepted (`OUT_WIDTH` in `term.c`).
The `/` commands' output is not checked against that width.

## What is in it

| File | What |
|---|---|
| `main.c` | The program's start, its big stack, and the stack's guard |
| `term.c` | The telnet side, the line reader, and the `/` commands |
| `top.c` | One sentence or one goal |
| `scene.c` | The world as seen, the projection, the hidden lines, the arm, and the point stream  |
| `rt.c`, `rt.h` | The Lisp run-time; values, cells and their collector, symbols, dynamic binding, non-local exits, an evaluator and the primitives |
| `code.c`, `image.c` | The original's Lisp as C and the Lisp world as loaded, with every amendment the PDP-1 program has |

The original's grammar, semantics, answers, and Micro-Planner run as compiled C.
The blocks world's theorems stay data, as they were, and the compiled Micro-Planner runs them through the evaluator.

## How fast

The 42 sentences of the dialogue take 16 ms of parsing and answering in all, and the slowest, 37,
1.3 ms on a fairly low performance x86.
The PDP-1 program takes 1,394 s of PDP-1 time for the same,
136 s for its slowest, 20.

The whole dialogue never garbage collects and the program's memory peaks at 14 MB.

## Where it differs from the PDP-1 program

The arm moves faster than the original's pace, I got tired of waiting for it.
It can be changed via a define, though.

The replies, the plans, and the placements are the same, and so is everything typed at the terminal.

 What differs:
- **`NO ROOM` is gone.** So are the drum, the code cache and the arenas, a sentence that ran out
  of room there works here and `/ROOM` gives this program's cells, collections and events.
- **A sentence that runs away is abandoned** after 10 s of the processor, or at 32M cells live, or
  at a stack of about 1 GiB and replies `I DON'T UNDERSTAND.`, or `I CAN'T`
  to `/DO` and `/TRY`.
- A `/DO` that is stopped leaves the world as it was.
- A sentence that is stopped may leave the planner's picture of the world part way through a plan,
  `/RESET` will reinitialize.
- A clarifying question that keeps being answered stops the same way after four answers.
- **`/TIMES`** has a second line, `MICROSECONDS PARSE p ANSWER a`, in decimal, since this program's
  sentences rarely take a whole millisecond.
  Neither line counts the arm's pace or the user's typing.
- **`/RESET`** puts everything back as at the start, learned words and dialogue memory included.
- **`/TRACE ON`** is accepted and does nothing.
- **`/FIND`** lists the same facts in a different order.
- **`/TREE`**'s unit lines have the same features, sometimes in another order.
