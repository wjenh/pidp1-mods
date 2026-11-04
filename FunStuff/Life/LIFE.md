# Running Life

John Conway's Game of Life, written in am1 for the PDP-1D, on the Type 340 display with the light pen.

This is version 1.1\
Edit date 04-Nov-2026

## What is the Game of Life?

Conway, a mathematician at Cambridge, devised Life in 1970, and Martin Gardner made it famous
in his *Mathematical Games* column in the October 1970 *Scientific American*.
It is not a game anyone plays, it is a universe with one rule that plays itself.

The universe is a grid of square cells, each alive or dead.
Each cell has eight neighbors, the cells touching it across a side or a corner.
At every tick, a generation, every cell changes at once:
- A dead cell with exactly three live neighbors is born.
- A live cell with two or three live neighbors survives.
- Every other cell dies, or stays dead, of loneliness or of overcrowding.

That is all of it, yet from it come patterns that sit still, patterns that blink, patterns that
crawl across the grid, the gliders and spaceships, and small patterns that churn for thousands of
generations before they settle.
Conway offered a prize for a pattern that grows without limit, and in November 1970 Bill Gosper's
group at MIT won it with the glider gun, a pattern that fires a new glider every 30 generations.
Computers of the day soon ran it, for hours on end, to see what came out.

This version runs Life on a field of up to 126 by 120 cells, one dot a cell.
A real computer's field has to end somewhere, and what happens at its edges is a choice;
Life starts with dead edges, and the `FIELD` menu offers three more, see *The edges* below.
Everything is done with the light pen and menus at the top of the screen, there are no switches
to set.

## Building and starting it

Assemble it in the directory that holds the five source files:
```
am1 life.am1
```
*life.am1* includes *life.ah*, its settings, *lifemenu.ac*, the menus and the pen, *lifelib.ac*,
the pattern library, and *lifefield.ac*, the field's sizes and edges.
This makes *life.rim*.

Load *life.rim* like any other tape using *pdp1central*, *fastload*, or the reader and start it at 0100.
It needs:
- A PDP-1D, it uses the extended instructions.
- The Type 340 display on sequence break channel 0, with a display client that shows the light pen,
  *t30dpy*, see *UsingT30dpy.md* and *UsingTheLightpen.md*.
- The BBN timesharing clock, IOT 32, for its 1 ms counter, see *UsingTimesharingClock.md*.
- Six banks of memory, banks 0 to 5.

It starts running four glider guns, each firing at the center, on the largest field with dead edges
at the fastest speed.

Started again at 0100 without loading the tape again, it keeps the field size, the edges, the speed
and the whole field (`GUNS` or `ALGOL`) last chosen, and starts that field again from generation 0.
Everything else is as a fresh load leaves it.
No menu or library screen is open, no pattern is held, the pen paints, and there is nothing for
`UNDO` to take back.

## The screen

The menu bar is across the top, the field below it.
The generation counter, seven digits, is at the top right on the line of the bar.
It counts to 9,999,999 and starts again at 0.
When a generation has more live cells than the display list can hold, the rows that fit are drawn,
the rest are not, and `FULL` is lit beside the counter.
The generation itself is always computed in full, only the picture is cut short.
A field smaller than the largest is centered, inside a dim border.

While Life is stopped, with no menu open, a dim lattice of dots marks every cell's place, lit or not,
graph paper for the pen.
While it runs the lattice is not drawn and only the menu answers the pen.

## The light pen

The light pen is the mouse.
Holding the left button down is holding the pen to the screen, and releasing it lifts the pen.
It is a light pen, it only sees dots that are lit, blank space does nothing at all.
That is what the lattice is for.
A dead cell has nothing to see without it.

A *press* is putting the pen down; a lift of a quarter second or more ends it.
Some things act only on a fresh press, so that a pen slid onto them from somewhere else does nothing.

## The menu bar

| Item | What it does |
|---|---|
| `RUN` / `STOP` | Starts the generations, then reads `STOP`; touched again, stops them and reads `RUN` |
| `STEP` | One generation, while stopped; dim and inert while running |
| `EDIT` | Opens the editing list: `PAINT`, `ERASE`, `CLEAR`, `UNDO` |
| `PATTERN` | Stops the run and opens the pattern library's screen |
| `SPEED` | Opens the list of speeds |
| `FIELD` | Opens the list of field sizes and edges |

A bar item acts once when touched.
To use it again, lift the pen and touch it again, holding the pen on it does not repeat it.
Touching any bar item closes a list open under another item, and ends placing a pattern.

## The lists

`EDIT`, `SPEED` and `FIELD` each open a list hung below the item, and the item is lit.
`SPEED` opens with the speed in use lit, `FIELD` with the size and the edge in use lit.

- Touch a row to light it. A small box, a square with its diagonals, appears to its right.
- Lift the pen, then touch the box to choose the lit row. It acts and the list closes.
- Touching the item on the bar again closes the list with nothing chosen.
- A list left alone for four seconds closes by itself.

The pen can slide down a list lighting rows, but it has to be lifted before touching the box,
so that sliding over it chooses nothing.

### EDIT

Each of these stops a run.

| Row | What it does |
|---|---|
| `PAINT` | The pen sets cells, the mode Life starts in |
| `ERASE` | The pen clears cells |
| `CLEAR` | Kills every cell and sets the counter to 0 |
| `UNDO` | Takes back the last edit; dim when there is nothing to take back |

### SPEED

The rate is the most generations a second, the speed takes effect at once, running or not.

| Row | Rate |
|---|---|
| `FAST` | As fast as the machine can go, one generation right after the other |
| `8/S` | 8 a second |
| `4/S` | 4 a second |
| `2/S` | 2 a second |
| `1/S` | 1 a second |
| `1/2 S` | One every 2 seconds |

A pattern can't go faster than the machine.
How fast that is depends on how many cells are alive and where.
It is about 3 generations a second on the four guns at the start, slowing toward 2 as their gliders
spread, about 3 a second on `ALGOL`, and down to one every 3 seconds on a field packed with cells.
A dense field is mostly slow because of drawing it, not computing it.

### FIELD

The first four rows are the sizes, the last four the edges.

| Row | Field |
|---|---|
| `126X120` | The whole screen, the size Life starts in |
| `90X90` | |
| `60X60` | |
| `32X32` | |
| `DEAD` | Dead edges, the edge Life starts with |
| `MIRROR` | Mirrored edges |
| `TORUS` | The edges wrap around |
| `OPEN` | Open edges, only at the three smaller sizes |

The cells stay the same distance apart, a smaller size is a smaller area in the middle of the screen.
Any cell outside the new size is cleared, the field becomes what is inside its bounds.
A smaller field is faster, so a small pattern can run quicker than it does on the full screen.

Changing the size or the edge does not stop a run or reset the counter, but it can't be undone.

## The edges

Life's universe is infinite, a computer's field is not.
What a cell at the edge of the field sees beyond it is the edge's choice:

- With `DEAD`, nothing beyond the edge is ever alive.
  A glider that reaches an edge breaks up there, usually into a block or some debris,
  which can in time get in the way of what comes next.
- With `MIRROR`, the edge is a mirror, so a cell at the edge sees itself beyond it.
  Something that meets a mirror meets its reflection, and that rarely ends in a clean bounce.
  A glider usually turns into debris, but different debris than at a dead edge.
- With `TORUS`, the top and bottom edges wrap around to each other, and so do the left and right,
  the surface of a doughnut.
  A glider that leaves on the right comes back on the left, and nothing is ever lost;
  the four guns' gliders come round and hit the guns from behind.
- With `OPEN`, what leaves the field goes on as if into empty space, out of sight, and is not seen
  again.
  Life computes a hidden band around the field, the rest of the largest field, so a smaller size
  is needed.
  At 126 by 120 the `OPEN` row is dim and does nothing.
  The band is 15 to 47 cells wide, depending on the size, and it is not infinite.
  What reaches its far edge meets a dead edge there, out of sight, and debris left there can,
  rarely, grow back into view.
  While anything moves in the band it is computed like any other cell, so the field runs more
  slowly, toward the speed of the largest size.
  The band is cleared whenever the size or the edge is changed while `OPEN` is in use, so nothing hidden ever
  comes into view, and choosing 126 by 120 then sets `DEAD`.

All four follow Life's rule exactly, each on a different kind of universe.

## Painting and erasing

Stop Life first, with `STOP`, or by choosing `PAINT` or `ERASE` from `EDIT`, which stops it.
The lattice appears, and whichever of `PAINT` and `ERASE` was chosen last says what the pen does.

- Touch a cell's dot on the lattice to paint or erase it.
- Hold the pen down and drag to paint or erase a line of cells.
  The pen is read once a frame, so a quick stroke skips cells; the gaps are filled with a straight line.
- Painting the same cell twice, or erasing a dead one, does nothing, so going over a cell again is safe.
- A painted cell shows at once; an erased one goes out a moment later, a quarter second or more
  on a large, busy field.
- After any menu action, the lattice waits for the pen to be lifted and put down again,
  so a pen still held from the menu draws nothing.

While running, touching the field does nothing.

## Undo

`UNDO` puts back the field and the counter as they were before the last edit made while stopped.
An edit is one of:
- A stroke, from putting the pen down on the lattice to lifting it.
- One copy of a pattern dropped from the library.
- `CLEAR`.
- A whole field placed from the library.

`UNDO` again puts the edit back, so it flips between the two.
There is one level only.
A generation, by `RUN` or `STEP`, or a change of size or edge, ends it, and `UNDO` is dim until the next edit.

## The pattern library

`PATTERN` stops the run and replaces the screen with the library.
`PATTERNS` is across the top, below it a page of names in two columns, the preview on the right,
and six keys along the bottom.
The library has 29 entries on two pages.

Touch a name and it lights, and its preview shows to the right, with a line below it giving the
pattern's size in cells and what it does:
- `STILL` never changes.
- `P2`, `P15`, ... repeats every that many generations, an oscillator.
- `C/4`, `C/2` moves, a spaceship; *c*, the speed of light, is one cell a generation, the fastest anything can move.
- `GUN` fires gliders.
- `GENS` and `GONE AT` say how long a methuselah, a small pattern with a long life, runs before it
  settles or dies out.
- `WHOLE FIELD` replaces the field instead of being placed on it.

The keys act only on a fresh press:

| Key | What it does |
|---|---|
| `PREV` | The page before, round to the last |
| `NEXT` | The page after, round to the first |
| `TURN` | Turns the lit pattern a quarter turn clockwise; the preview shows it |
| `FLIP` | Flips the lit pattern left for right |
| `PLACE` | Takes the lit pattern into the pen, or replaces the field with a whole field, and closes the library |
| `CANCEL` | Closes the library, nothing changed |

`TURN` and `FLIP` do nothing on a whole field.
The turn and flip stay set when another pattern is lit, so a set of patterns can be placed the same way round.
`PLACE` with nothing lit does nothing.

### Page 1, still lifes and oscillators

| Name | What it is |
|---|---|
| `BLOCK`, `BEEHIVE`, `LOAF`, `BOAT`, `TUB`, `SHIP`, `POND` | Still lifes |
| `EATER` | A still life that destroys a glider that runs into it the right way |
| `BLINKER`, `TOAD`, `BEACON`, `CLOCK` | Period 2 oscillators |
| `PULSAR` | Period 3 |
| `PENTADEC` | Pentadecathlon, period 15 |
| `GALAXY` | Kok's galaxy, period 8 |
| `QUEENBEE` | The queen bee shuttle, period 30 |

### Page 2, spaceships, guns and methuselahs

| Name | What it is |
|---|---|
| `GLIDER` | The glider, the smallest spaceship |
| `LWSS`, `MWSS`, `HWSS` | The light, middle and heavy weight spaceships |
| `GUN` | Gosper's glider gun |
| `SIMKIN` | Simkin's glider gun, period 120 |
| `RPENT` | The R-pentomino, 5 cells that run for 1,103 generations |
| `ACORN` | 7 cells that run for 5,206 generations |
| `DIEHARD` | Dies out completely at generation 130 |
| `BHEPTO`, `PIHEPTO` | The B- and pi-heptominos |
| `GUNS` | The whole field Life starts with, four guns firing at the center |
| `ALGOL` | A whole field, the pattern of the ALGOL 68 Life program on Rosetta Code |

The generation counts of the methuselahs are on an unbounded grid.
Here the field is limited, so they can come out differently, especially on a small field;
`OPEN` edges come closest.

### Placing a pattern

After `PLACE` with a pattern lit, the field is back with its lattice, and the pattern is in the pen.

- Put the pen on the lattice.
  A dim copy of the pattern, the *ghost*, follows it, centered on the pen.
- Lift the pen and the pattern is dropped there, added to the cells already alive.
  Whatever falls outside the field is left off.
- The pattern stays in the pen, so it can be dropped again and again, each copy a separate edit for `UNDO`.
- Touch any item on the bar to put the pattern down without dropping it.

A whole field, `GUNS` or `ALGOL`, replaces the field at once when `PLACE` is touched,
starting at the top left of the size in use, and the counter goes to 0.
`UNDO` brings back the field it replaced.

## Credits

- The Game of Life is by John Horton Conway, 1970.
- The glider gun is by Bill Gosper, MIT, 1970.
- The `ALGOL` pattern is from the ALGOL 68 Life program on Rosetta Code.
