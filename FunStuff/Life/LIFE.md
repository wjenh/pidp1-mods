# Running Life

John Conway's Game of Life, written in am1 for the PDP-1D, on the Type 340 display with the light pen.

This is version 1.2\
Edit date 06-Nov-2026

## What is the Game of Life?

John Horton Conway, a mathematician at Cambridge, devised Life in 1970, and Martin Gardner made it famous
in his *Mathematical Games* column in the October 1970 *Scientific American*.
It is not a game one plays, it is a universe with one rule that plays itself.

The universe is a grid of square cells, each a sinble point and each alive or dead.\
Each cell has eight neighbors, touching it on the sides and at each corner.\
At every tick, a generation, every cell changes at once:
- A dead cell with exactly three live neighbors is born.
- A live cell with two or three live neighbors survives.
- Every other cell dies of loneliness or of overcrowding, or stays dead.

That's it, yet from it come patterns that sit still, patterns that blink, patterns that
crawl across the grid, the gliders and spaceships, and small patterns that churn for thousands of
generations.

Conway offered a prize for a pattern that grows without limit, and in November 1970 Bill Gosper's
group at MIT (Go Tech!) won it with the glider gun, a pattern that fires a new glider every 30 generations.

It quickly captivated countless people, and computers of the day soon ran it for hours on end to see what
new patterns culd be discovered.

This version runs Life on a field of up to 126 by 120 cells, one dot a cell.
A real computer's field has to end somewhere, and what happens at its edges is a choice.

Life starts with dead edges, and the `FIELD` menu offers three more modes, see *The edges* below.
Everything is managed with the light pen and on-screen menus, there are no switches to set.

A field can be saved to paper tape or to a library on the Microtape and read back, see
*Saving and loading* below.

## Building and starting it

```
make
-or-
make install
```

If intall is used, the predefined library tape, LifeLibrary.mtp is also copied to /opt/pidp1-mods/Microtapes.
If you don't want to install the tape, it's in Tapes here.

It also includes the pen keyboard library, PENKBD, from am1's include directory for the pop-up keyboard.

Load *life.rim* like any other tape using *pdp1central*, *fastload*, or the reader and start it at 0100.
It needs:
- A PDP-1D, it uses the extended instructions.
- The Type 340 display with a display client that supports the light pen, *t30dpy* is preferred.
- The BBN timesharing clock, IOT 32, for its 1 ms counter.
- Seven banks of memory, banks 0 to 6.
- For saving and loadingi libraries the Type 550 Microtape is neeed.
  Life runs without them, only the commands that use them need them.

It starts running four glider guns, each firing at the center, on the largest field with dead edges
at the fastest speed.

Started again at 0100 without reloading, it keeps the field size, the edges, the speed,
the Microtape drive and the whole field last chosen, and starts that field again from generation 0.
Everything else is as a fresh load leaves it.

## The screen

The menu bar is across the top, the field below it.
The generation counter, seven digits, is at the top right on the line of the menu.
It counts to 9,999,999 and starts again at 0.
When a generation has more live cells than the display list can hold, the rows that fit are drawn,
the rest are not, and `FULL` is highlighted beside the counter.
The generation itself is always computed in full, only the display is truncated.
After a tape command, its status, such as `PUNCHED` or `BAD TAPE`, shows instead until the next pen-down.
A field smaller than the largest is centered inside a dim border.

While Life is stopped with no menu open, a dim lattice of dots marks every cell's position, it's
graph paper for the pen.
While it runs the lattice is not drawn and only the menu responds to the pen.

## The light pen

The light pen is the mouse.
Holding the left button down is holding the pen to the screen, *pen-down* or a *touch*, and releasing it lifts the pen.
Remember, it's a light pen; it only sees dots that are on, blank space can't be detected.
That is what the lattice is for, a full-field cell target.

A button-up of a quarter second or more is a *lift*, or pen-up.
Some things act only on a new pen-down; a pen moved onto them does nothing until that happens.

## The menu bar

| Item | What it does |
|---|---|
| `RUN` / `STOP` | Starts the generations and shows `STOP`; touched again, stops them and shows `RUN` |
| `STEP` | Run one generation. Available while stopped, inactive while running |
| `EDIT` | Opens the editing list: `PAINT`, `ERASE`, `CLEAR`, `UNDO` |
| `PATTERN` | Stops the run and opens the pattern library menu |
| `SPEED` | Opens the list of speeds |
| `FIELD` | Opens the list of field sizes and edges |
| `TAPE` | Opens the paper tape list: `PUNCH`, `READ` |

A menu item acts once when touched.
To use it again, lift the pen and touch it again, it does not repeat automatically.
Touching any menu item closes a list open under another item and ends placing a pattern.

## The lists

`EDIT`, `SPEED`, `FIELD` and `TAPE` each open a list below the item, and the item is highlighted.
`SPEED` opens with the speed in use highlighted, `FIELD` with the size and the edge in use highlighted.

- Touch a row to highlight it. A small box, a square with its diagonals, appears to its right.
- Lift the pen, then touch the box to choose the selection.
  The list closes and the action is performed.
- Touching the item again closes the list with nothing chosen.
- If no selection is made within 4 seconds, the list will close.

The pen can slide down a list highlightin rows, but it has to be lifted before touching the box,
sliding over it chooses nothing.

### EDIT

Each of these stops a run.

| Row | What it does |
|---|---|
| `PAINT` | Pen-down sets cells, the mode Life starts in |
| `ERASE` | Pen-down clears cells |
| `CLEAR` | Clears every cell and sets the counter to 0 |
| `UNDO` | Takes back the last edit, dim when there is nothing to undo |

### SPEED

The rate is the most generations a second, the speed takes effect at once, running or not.

| Row | Rate |
|---|---|
| `FAST` | As fast as the program can run, one generation right after the other |
| `8/S` | 8 a second |
| `4/S` | 4 a second |
| `2/S` | 2 a second |
| `1/S` | 1 a second |
| `1/2 S` | One every 2 seconds |

For FAST, how fast depends on how many cells are alive and where and the size of the field.
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
| `32X32` | The size of the original computer version's field |
| `DEAD` | Dead edges, the initial mode |
| `MIRROR` | Mirrored edges |
| `TORUS` | The edges wrap around |
| `OPEN` | Open edges, only at the three smaller sizes |

The cells stay the same distance apart, a smaller size is a smaller area in the middle of the screen, fewer cells.
Any cell outside the new size is cleared, the field becomes what is inside its bounds.
A smaller field is faster, a small pattern can run quicker than it does on the full screen.

Changing the size or the edge does not stop a run or reset the counter, but it can't be undone.

## The edges

Conway's abstract Life's universe is infinite, a computer's field is not.
What a cell at the edge of the field sees beyond it is determined by the edge mode:
- With `DEAD`, nothing beyond the edge is ever alive.
  A glider that reaches an edge breaks up there, usually into a block or some debris
- With `MIRROR`, the edge is a mirror, so a cell at the edge sees itself beyond it.
  Something that meets a mirror meets its reflection, and that rarely ends in a clean bounce.
  A glider usually turns into debris, but different debris than at a dead edge.
- With `TORUS`, the top and bottom edges wrap around to each other, and so do the left and right,
  the surface of a doughnut.
  A glider that leaves on the right comes back on the left, and nothing is ever lost.
  The four guns' gliders come back around and hit the guns from behind.
- With `OPEN`, what leaves the field goes on as if into empty space, out of sight, and is not seen
  again.
  Life computes a hidden band around the field, the rest of the largest field, so a smaller size
  is needed.
  At 126 by 120 the `OPEN` row is dim and does nothing.
  The band is 15 to 47 cells wide, depending on the size, and it is not infinite.
  What reaches its far edge meets a dead edge there, out of sight, and debris left there can,
  rarely, grow back into view.
  While anything is in the hidden band it is computed like any other cell so the field runs more
  slowly, generally the speed of the largest size.
  The band is cleared whenever the size or the edge is changed while `OPEN` is in use so nothing hidden ever
  comes into view, and choosing 126 by 120 then sets `DEAD`.

All four follow Life's rule exactly, each in a different kind of universe.

## Painting and erasing

Stop Life first, with `STOP`, or by choosing `PAINT` or `ERASE` from `EDIT`, which also stops it.
The lattice appears faintly, and whichever of `PAINT` and `ERASE` was chosen last determine the current edit mode.

- Touch a cell's dot on the lattice to paint or erase it.
- Hold the pen down and drag to paint or erase a line of cells.
  The pen is read once a frame, a quick stroke skips cells; the gaps are filled with a straight line.
- Painting the same cell twice or erasing a dead one does nothing, so going over a cell again is safe.
- A painted cell shows at once; an erased one goes out a bit later, a quarter second or more
  on a large, busy field.
- After any menu action, the lattice waits for the pen to be lifted and put down again,
  a pen still held down and dragged from the menu draws nothing.

While running, touching the field does nothing.

## Undo

`UNDO` puts back the field and the counter as they were before the last edit made while stopped.
An edit is one of:
- A stroke, from putting the pen down on the lattice to lifting it.
- One copy of a pattern dropped from the library.
- `CLEAR`.
- A whole field placed from the library.
- A record read from a tape and placed or put in as the field, see *Saving and loading*.
  After `FIELD`, `UNDO` also puts back the size and the edges the field had.

`UNDO` again puts the edit back, so it flips between the two.
There is one level only.
A generation, by `RUN` or `STEP`, or a change of size or edge, ends it, and `UNDO` is dim until the next edit.

## The pattern library

`PATTERN` stops the run and replaces the screen with the library.
`PATTERNS` is across the top, below it a page of names in two columns, the preview on the right,
and two rows of keys along the bottom.
The library has 29 entries on two pages.
The lower row's keys are described here; the upper row has only `MICROTAPE`, which opens the Microtape management 
menus, see *The Microtape library* below.

Touch a name and it highlights and its preview shows to the right with a line below it giving the
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
| `PREV` | The page before, loops to the last |
| `NEXT` | The page after, loops to the first |
| `TURN` | Turns the highlighted pattern a quarter turn clockwise; the preview shows it |
| `FLIP` | Flips the highlighted pattern left for right |
| `PLACE` | Takes the pattern 'into' the pen or replaces the field with a whole field and closes the library |
| `CANCEL` | Closes the library, nothing changed |

`TURN` and `FLIP` do nothing on a whole field.
The turn and flip stay set when another pattern is selected,
so a set of patterns can be placed with the same orientation.
`PLACE` with nothing selected does nothing.

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
| `ALGOL` | A whole field, the pattern from the ALGOL 68 Roseatta Code |

The generation counts of the methuselahs are on an unbounded grid.
Here the field is limited, so they can come out differently, especially on a small field;
`OPEN` edges come closest.

### Placing a pattern

After `PLACE` with a pattern highlighted, the field is back with its lattice and the pattern is in the pen.

- Put the pen on the lattice.
  A dim copy of the pattern, the *ghost*, follows it, centered on the pen.
- Lift the pen and the pattern is dropped there, added to the cells already alive.
  Whatever falls outside the field is discarded.
- The pattern stays in the pen, so it can be dropped again and again, each copy a separate edit for `UNDO`.
- Touch any item on the menu to discard the pattern without dropping it.

A whole field, `GUNS` or `ALGOL`, replaces the field at once when `PLACE` is touched,
starting at the top left of the size in use, and the counter goes to 0.
`UNDO` brings back the field it replaced.

## Saving and loading

A field can be saved to paper tape with `TAPE` on the menu, or to a library on the Microtape,
from the library screen.
Both save the same *record*:
- A name of up to 8 characters.
- The live cells, trimmed to the smallest box that holds them.
- The field size, the edges and the generation counter.

To save one pattern rather than the whole field, clear the rest of the field first; `UNDO` restores it afterwards.
Under `OPEN` edges, only the cells on the screen are saved, not the hidden band around them.

While a tape operation is running, Life stops and the screen shows only `WAIT` in its center.
The field comes back when the tape stops.
Be patient, the tape punch is not speedy.

### The status word

Each tape command ends by showing a word beside the generation counter and at the top right of
the library screen:

| Word | Meaning |
|---|---|
| `PUNCHED` | The record was punched |
| `READ` | A record was read from paper tape |
| `SAVED` | The record was written to the Microtape |
| `LOADED` | A record replaced the field, by `FIELD` |
| `DELETED` | The selected entry was deleted from the Microtape |
| `BAD TAPE` | A record or a Microtape block failed its checks, or a paper tape ended early |
| `NO TAPE` | Nothing in the reader, or no tape on the Microtape drive |
| `NOT LIFE` | The Microtape is not a Life library |
| `LOCKED` | The Microtape drive is write-locked |
| `TAPE FULL` | The Microtape library has no free slot |

A command that is refused changes nothing, the field, the counter and the tape stay as they were.
The status stays until the pen is next put down.

### Names: the pen keyboard

`PUNCH` and `SAVE` first open the pen keyboard in place of the screen with the prompt `NAME`.
Its field starts with the name last used.

- Touch a key to type it. A key types once per press, however long the pen is held on it.
- The keys are the letters, the digits, and `-`, `.`, `,`, `/`, `(`, `)`, `:` and `=`.
- `Shift` turns lower case on or off, and is highlighted while it is on.
- `Space` types a space, `Back` erases the last character, `Clear` erases them all.
- `OK` accepts the input.
  An empty name is allowed, it shows as `-` in the library.
- `Cancel` closes the keyboard and nothing is punched or saved.

Nothing more is accepted once a name has 8 characters.

### Paper tape

Touch `TAPE` on the menu, then a row and its box, as for any list.

- **`PUNCH`** opens the keyboard for the name, then punches the record.
  The punch is slow, about 18 ms a line.
  The four guns take about 7 s, with the blank leader and trailer, a glider about 2 s,
  and a field packed with cells at 126 by 120 about 40 s.
  Remember to save the tape using *pdp1central*, the desktop icon, or the *save_ptr* utility.
- **`READ`** reads a record from the reader.
  Load the tape in the reader first, as for any tape, with *pdp1central* or *load_ptr*.
  A reader with nothing in it gives `NO TAPE` after 2 seconds.

A tape starts with 60 blank lines, about 6 inches, originally to have enough blank tape to load it, and ends with 30.

A good `READ` opens the library screen, `PAPER TAPE`, with the record's
name highlighted, its preview, and a line giving its width and height in cells, the field size, the
edges and the generation it was saved at, for example `3X3  126X120 DEAD  0`.
From it:
- `PLACE` puts the record in the pen to drop like a library pattern when it has 64 live cells or fewer.
  A larger record replaces the field, as `FIELD` does.
- `FIELD`, on the upper row, replaces the field with the record, at the size, the edges, and the
  generation it was saved with, and shows `LOADED`.
- `CANCEL` closes the screen and leaves the field as it was.
- `UNDO` undoes either.

### The Microtape library

A Microtape holds a library of up to 128 records, eight pages of 16 names.
Mount a tape on a drive before using it, as *UsingType550Microtape.md* describes; Life never
mounts one itself.
A blank tape, or a new image file, is an empty library, ready to save to.

*LifeLibrary.mtp* is a library of 64 patterns, the 29 built-in ones and 35 more.
Mounting it locked write-protects the drive, so it won't be modified.

On the library screen, `MICROTAPE` open the Microtape's display and reads the drive's directory.
The page's title gives the drive and the page, `MICROTAPE D1  1/8`.
The upper row of selections is then:

| Selection | What it does |
|---|---|
| `BUILTIN` | Back to the built-in pages |
| `D1` to `D8` | The drive. Each press steps to the next, wrapping from 8 to 1, and reads that drive's directory.
  Life starts with drive 1 selected. |
| `SAVE` | Opens the keyboard for the name and writes the field's record to the first free slot nearest the directory |
| `DELETE` | Deletes the highlighted entry, its slot is then free for a later save |
| `FIELD` | Replaces the field with the highlighted record, at its saved size, edges and generation |

- Touch a name to highlight it.
  Its record is read from the tape and its preview and size line show as for a paper tape.
- A free slot is a blank line.
- `TURN`, `FLIP`, `PREV`, `NEXT`, `PLACE` and `CANCEL` act as on the built-in pages.
  `PLACE` holds a record of 64 live cells or fewer in the pen and puts a larger one in as the
  field, as on the paper tape's page.
- A slot that gives `BAD TAPE` is read again only when the pen is lifted and put down on it again.
- A tape that is not a Life library gives `NOT LIFE` and is never written.

The directory is read again at each `TAPE` and each press of the drive key so a tape changed on a drive is seen.
`PATTERN` on the menu always opens on the built-in pages.

The Microtape is reasonably quick near the directory, in the middle of the tape, where the first records go,
a save or a read takes a second or two.
The first action after a tape is mounted starts from the tape's beginning and takes about 20 seconds to
get to the directory.
Remember, it's a tape drive, it has to move that tape to the correct location, and it uses accurate timing.

## Credits

- The Game of Life is by John Horton Conway, 1970.
- The glider gun is by Bill Gosper, MIT, 1970.
- The `ALGOL` pattern is from the ALGOL 68 Life program on Rosetta Code.
- Fifteen of *LifeLibrary.mtp*'s patterns are from LifeWiki's pattern collection.
