# **Sketchpad** technical design

This document describes how Sketchpad, the light pen drawing program for the PiDP-1 and its Type
340 display, is built and how it works, and ties each part back to Ivan Sutherland's 1963 thesis.
Where the program does something differently from the thesis, it says what and why.
How to use the program is *UsingSketchpad.md*.

This is version 1.0 and covers the program's first release.\
Edit date 18-Oct-2026

**NOTE** This document was written by Claude Opus because wje didn't have the patientce to do it.\
The writing is probably a bit, well, AI (duh). Sorry, but it is what it is.\
wje (Bill Ezell)

## The thesis and this program

The thesis is Ivan E. Sutherland, *Sketchpad, a Man-Machine Graphical Communication System*, MIT,
January 1963, in the University of Cambridge's 2003 electronic edition (technical report
UCAM-CL-TR-574).
References here are to its chapters, I to IX, its appendices, A to G, and its figures.

The thesis is the specification of what the user sees.
Where it states a behavior, the program does that, as far as a PDP-1 and a Type 340 allow.
How the program gets there is not bound by the thesis.
It is a new implementation of the behavior the user perceives, and it uses the thesis's
algorithms only where they suit this machine.
Where a different method is faster, smaller or simpler here, the program uses it.
Each such choice is listed under *Divergences from the thesis*, near the end.

The thesis itself has a PDP-1 connection.
Sutherland notes that before his first drawing program worked, Welden Clark of Bolt, Beranek and
Newman showed him a similar program on the PDP-1 (chapter II), and that BBN's Rolland Silvers
cut the cost of pen tracking with prediction (chapter IV).

## Two machines

| | TX-2 (the thesis) | PDP-1 with Type 340 (this program) |
|---|---|---|
| Word | 36 bits, split into 18-bit halves for x and y | 18 bits |
| Memory | 69,632 words | 65,536 words, sixteen banks of 4,096 |
| Display | Spot plotter, one point at a time from a table, about 20 us a spot | Vector display running a display list in core, lines, points and characters drawn by the display itself |
| Raster | 1024 by 1024, 7 inches square | 1024 by 1024 |
| Pen | Light pen reporting each spot it sees, by interrupt | Light pen pausing the display at each point it sees, by sequence break |
| Controls | 37 push buttons, 592 toggle switches, four knobs | None but the pen and the menu |
| Text entry | Lincoln Writer keyboard | A keyboard drawn on the screen, worked by the pen |
| Storage | Magnetic tape, library tape | Type 550 Microtape, sixteen pictures a tape |
| Hard copy | Plotter | *sketchplot*, a PNG drawn from the tape image |

Two differences shape everything else.

The TX-2's display was a spot plotter fed from a table the program computed, and about three
quarters of Sketchpad's computing time went into the fifteen instructions that generated line and
circle spots (chapter V).
The Type 340 draws vectors, points and characters from a display list in core by itself, stealing
one memory cycle a word, so the CPU never generates a spot.
The program's display work is writing and rewriting that list.

The TX-2 had the push button box, the toggle switches and the knobs.
The PDP-1 here has none of them, and the program takes every command from the light pen through a
menu on the screen.

## The program at a glance

The program is assembled by **am1** from *sketchpad.am1*, which holds the
variables and the tables, and includes *sketchpad.ah*, every constant but the intensities,
*intensities.ah*, the intensities, and thirty-one *sp\*.ac* files in memory order.
All of them are in *FunStuff/Sketchpad*, and the program is assembled there.
It starts at `boot`, at 100, which opens the extend window and enters `initVars` in bank 12, then
`initGates` in bank 6, then `init` in the main bank, bank 4.

Three things run at once.
- The **Type 340** runs the display list, over and over, a *frame* at a time.
- The **break routine**, `penISR` in bank 0, answers the display's flags, the light pen's hits,
  a frame's end and an edge violation, posts what happened for the main loop, and restarts the
  display.
  It runs no program logic.
- The **main loop**, `main` in bank 4, takes what was posted, runs the current mode, keeps the
  constraints, works out the truss, and rewrites the display list where the drawing changed.

The thesis warns of this split.
Its early versions updated the ring structure from another TX-2 sequence as each push button was
pressed, which went wrong when the display generation was working in the structure at that
instant.
Sutherland concluded that such sequences should only bring information in, the processing being a
job for the main computation stream (chapter III, "Booby Traps").
`penISR` follows that rule.

### The main loop

One pass of the main loop does, in order,
1. `snapshot` copies the break routine's mailboxes with the break masked, and reads the 1 ms clock.
2. `onFrames` counts the frames that ended.
3. `onTargetHit` moves the pseudo pen location and runs the mode's handler for a target hit.
   Target hits are taken before menu hits, so a pen carried from the target to the menu acts in
   the order things happened.
4. `onMenuHit` runs the menu.
5. `checkLost` declares a lift once no target hit has come for 100 ms, and runs the mode's lift
   handler.
6. `onClock` runs the timed work, a cycle's step, a label's flash, the status word's timeout,
   an idle list's closing and the display watchdog.
7. `kbTick` ends a key press on the pen keyboard while it is open.
8. `onRate` measures the frame rate and sets how often the target is drawn.
9. `solveStep` runs the constraint solver for up to 40 ms while it is awake.
10. `taTick` runs a slice of the truss analysis.
11. The text module writes the texts' list if a text changed.
12. `showStep` rewrites the constraints' marks while Show is on.
13. `rebuildDisplay` rewrites the line slots that changed.

The current mode chooses each handler through a jump table indexed by the mode, never through a
chain of skips.
An inverted skip is the easiest mistake to make in PDP-1 code and among the hardest to see, and
the jump tables remove that class of bug from the dispatch.

### Modules and gates

The main bank, bank 4, holds the main loop, the menus, the modes and Undo.
Everything else is in modules, each in a separate bank, each entered through one gate.

| Gate | Module | Bank |
|---|---|---|
| `arcCall` | Arcs, the removal and merging of points and lines, Define's and Copy's geometry | 2 |
| `tapeCall` | Saving and loading pictures | 6 |
| direct calls | Scale, its slider and its Undo | 6 |
| `instCall` | Instances, masters, Copy, Change, cycles, Dismember | 7 |
| `conCall` | Constraints and the solver | 8 |
| `txCall` | Texts and numbers | 10 |
| `taInit`, `taTick` | The truss analysis, its floating point, and what a lift will take | 11 |
| direct calls | The view, the pen keyboard, the target's clipping, routines moved for room | 12 |
| direct calls | The constraints' marks, the line search, arcs through the view | 13 |

A module is entered by an entry number.
`instCall`, `conCall` and `txCall` are called with `jda`, the entry number in IO and the argument
in AC, and jump through the module's table of entries, each entry the routine's `jsp` or `jda`, so
each routine keeps its calling form.
`arcCall` and `tapeCall` take the entry number alone.
A module reaches the main bank's words and the shared tables by full 16-bit address, through the
extend window, and calls into another bank only at that bank's entries.
A gate costs about twelve memory cycles a call.

## The display

### The frame

The display list is a frame that ends in `halt interrupt`.
When the display reaches the halt, it raises a break, and `penISR` restarts it with `dla`.
The frame end is the program's frame clock.
A frame lasts from about 1.4 ms with nothing drawn to about a third of a second with 192 lines
across the screen.

A frame holds, in order,
1. the menu bar, the status word under it when one is shown, and the open list if any, the bar
   and the list pen-sensitive,
2. a call of the tracking target,
3. the drawing, the line slots in bank 1, then the arc slots, then the instance slots, not
   pen-sensitive, with target calls spread among them,
4. the constraints' marks, while Show is on or a mode shows some,
5. the arcs' point marks, in the modes that take a point,
6. INK, pen-sensitive, from the start until the target first shows,
7. the marks on points Dist, Change or Gap has picked,
8. the rubber band, or the arc band and its radius,
9. the dot, the pseudo pen location, and the lock mark,
10. the halt.

At that halt `penISR` may start three more lists, each by `dla`, each ending in a halt.
These are the constraints' arms in bank 5, the texts in bank 3, and the truss's readouts in bank
13.
The pen keyboard and the tape directory replace the whole frame while they show, through the word
`frameGo`, which names the list each restart begins with.

The 340's `jump` and `save` reach only 13 bits of address, so every list they enter is in banks 0
and 1.
`dla` takes a full address, so a list started by it may be in any bank, provided it holds no jump
or save and ends in a halt.

### Rewriting while the display runs

The display reads the list while the program writes it.
A word half old and half new can draw a stroke across the screen, or run the display off the edge,
which stops the frame.
Every structure the program rewrites is laid out so that a frame sees the old item, nothing, or the
new item, never a mix, with three exceptions, each lasting one frame at most.
The rubber band (`writeBand`) and the crosses on the points Dim has picked (`pmAgain`) are rewritten
while they show, so one frame can draw a band or a cross of old and new words, near the pen.
An arc slot at x1 is rewritten as soon as its gate shuts, without waiting for a frame inside it to
leave, so that frame could draw old and new chords together.
Each slot starts at an absolute position, so the mix misplaces only that arc's chords, and only
for that frame.
- A **line slot** is a parameter word, a y word, an x word that enters subroutine mode, a gate
  word, and 11 vector words of which only the last carries `end`.
  The gate jumps into the last K vectors, K being the line's.
  A rewrite points the gate past the slot, writes the vectors, then points the gate at them.
- The **end of the line list** moves as lines come and go.
  No order of its two words' writes is safe while the display runs, so the pair is posted to
  `penISR`, which writes it at the frame's halt, while the display is stopped.
- An **arc slot** has the same shape, with 64 vector words.
- An **instance slot** is a parameter word, y and x, a gate that is a `save` of the master or a
  jump past it, and a target call.
  A move rewrites only the y and x words, each word a whole position.
- The **texts' list** is two buffers.
  The program writes the one not shown, and switches only after two frames have ended since the
  last switch.
- The **constraints' marks** are written from the last to the first, each slot's gate last, the
  slot after the last shut first.

A change rewrites only the slots it changes.
A commit or a deletion marks its slots stale, and the rebuild writes those alone, so at 192 lines
across the screen a change stops the main loop for 3 to 6 ms.
The whole list is written only after a load, a change of view, or a drag or a Scale step that
moves a point with more than 32 lines, about 0.35 s at 64 short lines and about 1 s at 192 lines
across the screen.

### Lines

A line is the whole straight line the user drew, one table entry between two points, as in the
thesis.
Only its display slot splits it, into the fewest vectors of at most 100 units on either axis, up
to 11, the remainders of the division spread so that every step ends within a unit of the true
line.

### Arcs

The thesis generates circles with a difference equation (chapter V, equations 5-2 to 5-5), and
discusses at length why circles drawn that way fail to close.
Here an arc is up to 64 chords, a chord to a 64th of a turn, each vertex computed directly from the
arc's start, rotated about its center with a quarter wave sine table and the PDP-1D's `mul`, in
32nds of a unit.
No error builds up from vertex to vertex, so every circle closes on its start, and the worst vertex
lies within 0.68 of a unit of the true circle.
The thesis's equation 5-2 at chord-sized steps was modeled at 0.79 to 1.29 units, and most of its
circles failed to close.

The slot runs the full circle backward from its last vertex to the start, and the gate enters the
last k words for an arc of k whole steps, a part step at the end in the word before them.
Under the view (*The view*, below) an arc is drawn in finer chords from a finer sine table, so that
it stays smooth when magnified.

### Masters and instances

A master is a Type 340 subroutine of relative vectors from its origin, the center of its box,
beam-off moves to each line and arc, its last word carrying `end`.
An instance is a slot that sets the scale and the origin's position and calls the master with
`save`.
The 340 scales relative vectors by 1, 2, 4 or 8, so an instance's size is one of those, at no cost
in drawing time.
A master is at most 128 words and 5 ms of drawing, one period of the target's presentation, since
the target cannot be called from inside a master.
The four masters' words live in bank 0, because `save` reaches only banks 0 and 1.

The thesis's instance is a four-component variable, its center and its size times the cosine and
sine of its rotation (chapter VI).
Here an instance is a translation and a scale.
The 340 cannot rotate a subroutine or a character, so rotation, and the constraints and functions
that depend on it, are left out.

### Text and characters

A text is drawn in the 340's character mode, character set 2 and its shift to set 1.
Text cannot live inside a master's words, because the 340 returns from a `save` at a text's end
code, and its one scale register is not put back by the return.
So an instance's texts are copies in the texts' list, written at the instance's place, at the
text's size times the instance's, up to x8.

A character takes about 27 us to draw at any size, and a text's other words about 39 us.
The program limits what the screen shows to 180 characters, each text counting two more, about
4.9 ms of drawing, one period of the target's presentation, so that text cannot slow the target.

## The light pen and tracking

### What the pen reports

The Type 340's light pen sees only points drawn with the pen enabled, within its aperture.
At a hit the display pauses and raises its pen flag.
`penISR` reads the hit's position with `drc`, and the display resumes with `drs`.
The historical PDP-1 has no pen-up signal, and the program does not use the emulator's.
So the program knows the pen has been lifted only when the hits stop.
The thesis uses the same signal, the loss of tracking, as its termination signal for every pen
operation (chapter IV, "Pen Tracking"), though there the user flicked the pen away rather than
lifting it.
Here a lift is declared 100 ms after the last target hit, and only once the target has been shown
at least four times since, so that a display that stops showing the target is not taken for a lift.

### The target

The thesis tracks the pen with a cross redisplayed about 100 times a second, at 1 ms a display, and
reports a maximum pen speed of about 20 inches a second (chapter IV).
Here the target is a 340 subroutine, a cross of dots, called from sites spread through the frame,
one call for every four line slots and one after each arc and each instance.
A frame grows with the drawing, so a target drawn once a frame would be drawn too seldom under a
large drawing.
What loses the pen is the longest gap between presentations, not their rate.

Under a held pen every presentation is a hit, and each hit pauses the display about 0.67 ms.
With every call live, a held pen would cost a third of the frames, which shows as flicker.
So the main loop measures the frame rate and makes only enough calls live for about 200
presentations a second, spread evenly through the frame, changing them only when the rate leaves
160 to 250.
A held pen then costs about 12% of the frames.

At each target hit, `penISR` reads `drp`, the emulator's predicted pen position, and moves the
target there.
`drp` is an addition of this emulator's Type 340, a predictor that makes up for the lag between
the mouse and the display.
The thesis tried prediction and set it aside for stability (chapter IV), while noting that BBN had
it working at about 3% of machine time.
With prediction the pen has been measured tracking at up to 40 inches a second.

The thesis "inks up" by having the user touch something displayed, the letters INK when nothing
else is shown, and puts the cross there (chapter IV).
Here the target is hidden at the start, the drawing is not pen-sensitive, and INK stands at the
middle of the screen, behind a gate in the frame that start-up opens and the first live target
call shuts.
A hit there, which `penISR` tells from a list's or the keyboard's by the display's address (`dra`),
is posted as a hit on Locate's label, so the target comes to the middle, under the pen.
Tracking, once on, is turned off only while a list or the keyboard holds the screen, so INK never
returns.

The target is drawn at scale 2, arms of 20 units with dots 4 apart.
A Type 340 edge violation halts the display, so the screen cannot clip the target's arms, and
`clipTarget` shortens them near the edges and the menu.

### Pointing

In the thesis every displayed spot carries the address of what it belongs to, and the light pen's
hits, gathered over a frame, say what the pen is aimed at, within about an eighth of an inch of
its center (chapter IV, "Demonstrative Use of Pen").
Here the drawing is drawn with the pen disabled, and the program finds what the pen is aimed at by
search, from the target's center.
The pen's hits are used only for the target and the menu.

This keeps a held pen from pausing the display at every point of the drawing it covers, and gives
the nearest-object rule directly, the same in every frame, without a table of hits.

### The pseudo pen location

The thesis's pseudo pen location (chapter IV) is a bright dot at the tracking cross's center,
which jumps onto the point, line, circle or intersection aimed at, and is where every operation
acts.
The program's dot does the same.
- It locks onto the nearest point within 20 units, nearest by the larger of the x and y distances.
  Twenty units stands for the thesis's reach of about an eighth of an inch.
- In the modes that take a line, it locks onto the nearest line, measured across the line's minor
  axis, within the line's span.
  A point wins over a line unless the line is nearer by more than 8 units, so that a short line
  can still be taken away from its ends.
- In Delete it also locks onto arcs, and in Line, Circle and a Move's drag onto arcs and onto two
  lines' crossing.
- An instance or a text is weighed by its middle against the nearest part.
- In Scale it locks onto lines, arcs, instances, texts and numbers, never a point alone, so that
  at a line's end the line locks.

The line search is the costly part, about 0.7 ms a line, so it is cut into slices of 30 ms, runs
only once the target has paused for 60 ms, and always runs at a lift.
A bright cross around the dot shows the point a lift will take, and the line or arc a lift takes
whole is drawn brighter, so the user sees what an action will do before doing it.

## The menu instead of the button box

Every thesis control is a row of the menu, and the toggle registers that chose a picture or a type
are the rows' sublists.

| Thesis control (appendix B) | Here |
|---|---|
| Draw | Draw > Line |
| Circle center, then Draw | Draw > Circle |
| Move | Edit > Move |
| Delete | Edit > Delete |
| Stop, or the pen's flick | A lift, or Stop |
| Horv | Edit > Constr > Horv |
| Constraint, with the type in toggle register 25 | Edit > Constr > a type |
| Copy, with the definition in toggle registers 20 to 23 | Picture > Copy > M1 to M4 |
| Instance, with the picture in toggle register 25 | Picture > Inst > M1 to M4 |
| Change instance | Edit > Change > M1 to M4 |
| Dismember | Edit > Dismem |
| Tie | Picture > Tie |
| Designate | Picture > Desig |
| Fix | Edit > Constr > Fix |
| Unfix | Misc > Unfix |
| Delete constraints, meta-guarded | Misc > Del cons, guarded by its OK |
| Delete picture, meta-guarded | Picture > Clear, guarded by its OK |
| Text | Draw > Text |
| Number | Draw > Dim > Number |
| Library, Library write | Misc > Load, Misc > Save |
| Garbage, recentering on the pen | View > Center |
| The scale and position knobs | View > x1 to x8, and Pan; Edit > Scale for one part's size |
| Toggle switches showing the constraints | Edit > Constr > Show |
| Sheet of paper, the toggle choosing a picture | Define > M1 to M4, and Clear |

Without the button box, a command needs a place on the screen and a way to be taken without being
taken by accident.
The bar across the top has eight items, a column each, 128 units apart, so that a column is
found by a shift, without division.
A list hangs below its item, its rows 36 units apart, with the pen's aperture 22 units across, so
that no frame can hit two rows.
The aperture is one define while a list is open and another elsewhere, both 22 units, so that
the two can differ again.
A row acts only through a small OK box beside it, and the box answers only a touch that begins a
press, so that a pen slid or carried onto it does nothing.
That box is the guard the thesis's meta button gave its destructive commands.
A row with nothing to act on is drawn dim and never lit, so the menu shows what is possible.
Nothing a row acts on changes while a list is open, so `rowCensus` counts the picture once, as
the list opens, into one word of tests, and `rowCan` answers each row from that word and a word
per item naming the tests it needs; a hit reads one word, never a scan.

While a list is open the drawing is hidden, by a jump past it, so that a list reaching into the
canvas crosses nothing and the frame runs fast.

## Data

### Tables instead of rings

The thesis keeps the drawing in a ring structure, n-component blocks linked by doubly linked rings
of hens and chickens, with a generic block for every type (chapter III, appendix C).
The rings let any element find everything that refers to it, and let new types be added without
changing the rest.

Here the drawing is a set of dense tables, one per kind of element, each entry a row across
parallel arrays.
- Points, `pointX` and `pointY`, 256.
- Lines, `lineFrom` and `lineTo`, point indices, 264.
- Arcs, their center, start and end points, direction, whole steps and radius, 19.
- Instances, their origin, scale, master, tie points and cycling flag, 32.
- Constraints, six words an entry, the type and up to four points and a parameter, 64.
- Texts, their origin, size, characters and 340 code run, 32, and the masters' texts, 32.
- Scalars, their value and whether it is set, 32.

A removal moves the last entry into the hole and renames every reference to the entry moved, so no
table has a free list or a dead entry.
A program of fixed tables in a known order is far smaller than the ring code, and lets a picture be
saved as a few runs of words.
The cost is that finding what refers to an element is a search, which at these sizes is cheap.

### The constraint table

Every constraint, and every designation and truss mark, is a six-word entry, the type word, the
points A, B, C and D, and a parameter.
The type words and the unused fields are negative, so no field but a point's index can be taken for
a point.
The thesis's rule is kept, that a constraint refers only to its variables, its points, not to the
line it was made on (chapter II, its "first basic error").

| Type | Mark | Points | Meaning |
|---|---|---|---|
| `CTHL`, `CTHP` | H | 2 | Level or plumb |
| `CTF` | F | 1 | Fixed |
| `CTEQ` | E | 4 | Two lengths equal |
| `CTLEN` | L | 2 | A length, kept in the parameter |
| `CTPAR` | P | 4 | Two lines parallel |
| `CTPERP` | R | 4 | Two lines perpendicular |
| `CTMID` | X | 3 | A point at a line's middle |
| `CTON` | N | 3 | A point on a line |
| `CTONC` | C | 3 | A point on a circle |
| `CTATT` | A | 1 | An attacher, the parameter its order |
| `CTBEAM` | B | 2 | A truss member, the parameter its stiffness |
| `CTLOAD` | W | 2 | A truss load |
| `CTROLL` | O | 3 | A truss roller, B and C its axis's line |
| `CTDES` | 1 to 6 | 1 | A designated point, its order its place among the designations |
| `CTDIST` | D | 3 | A scalar equal to a distance |
| `CTCHG` | Y | 3 | A scalar equal to a change in distance |
| `CTGAP` | O | 2 | A point kept beside a number's digits |

The thesis's letters (appendix A) are kept where they are free, H, C, X, P, D, Y and O.
The thesis uses P for both parallel and perpendicular, M for equal length at a ratio, L for a point
on a line and W for a fixed distance.
Here each row has a letter, named for the menu, so E is equal, R is perpendicular, L is a
length and N a point on a line.
B and W here are the truss's member and load.
The thesis's B makes a scalar equal to a size, and its W keeps a distance, which with Y built its
bridge beams.

### Memory

Each structure that grows has a bank, and each module a bank, so that one can grow without moving
the others.

| Bank | Holds |
|---|---|
| 0 | The break vector, `boot`, `penISR`, and every display word the 340 reaches by `jump` or `save`, the frame, the menus, the arc and instance slots, the marks and their glyphs, the master store |
| 1 | The line slots alone |
| 2 | The arc module, merging and removal, Define's and Copy's geometry, the arc table |
| 3 | The point, line and instance tables, the dispatch tables, the texts' list buffers, the scalars, some of the truss's tables |
| 4 | The main bank, the main loop, the menus, the modes, Undo, the gates |
| 5 | Copy's store, each master's points, lines and arcs, the constraints' arms list, and a saved copy of the arc module's working words |
| 6 | The tape module, start-up's display gates, the menu rows' census, the scale module |
| 7 | The instance module, which rows can act, the offsets of an arc's first quarter |
| 8 | The constraint module |
| 9 | The constraint table, the solver's working tables, the masters' constraints |
| 10 | The text and number module, the texts' tables, the instance count and each instance's master |
| 11 | The truss analysis, the floating point, and what a lift will take |
| 12 | The view, the pen keyboard, the target's clipping, start-up's first part |
| 13 | The truss's snapshot and readouts, the constraints' marks, the line search, arcs through the view |
| 14, 15 | The truss's stiffness matrix |

Every bank is close to full.
When a bank runs short, more is moved out of it than the change needs, so that the next change
finds room.
The main bank runs with the extend window open and reaches bank 0 and the tables by full address.
Bank 0's words 4 to 077, unused by the single-channel break, hold display words too.

## Editing

### Drawing and moving

The thesis observes that drawing is moving, a new line being a line whose far end is left moving
with the pen (chapter VII).
Here a Line commit adds at most two points and one line, and the rubber band is drawn from the
last vertex to the dot.
A stroke along an existing line adds nothing, and a stroke under 4 units, the distance the target
settles past a pen that stops, adds nothing.

Move picks up a point, a line, an instance or a text.
A dragged point's lines swing whole.
A point dropped on a point merges into it.
The thesis's merging (chapter VI) is kept, everything that referred to either point now referring
to the one left, a line left of no length or repeating another removed, and a constraint repeated
or left on one point dropped.
A line dropped on a line merges end into nearer end, the thesis's merging of like with like.

While a point is dragged, only its lines' slots are rewritten, from a list made at the pick-up, up
to 32 lines.

### Points on lines and circles

The thesis adds a point-on-line or point-on-circle constraint when a point is made or dropped with
the pen aimed at a line or circle, and two at an intersection, only where one is not already there
(chapter VII).
The program does the same, as N and C entries.
The thesis also gives every arc a constraint keeping its start and end on its circle.
Here an arc's end is placed where the circle crosses the ray to the pen, so it lies on the circle
by construction, and the solver keeps start and end at the radius only for arcs touching a point
the solver may move.

### Deletion

The thesis's recursive deletion (chapter VI) is kept.
A deleted point takes its lines and arcs and its constraints, and a deleted line or arc takes the
N's and C's that keep points on it.
A point left with nothing on it is removed at once.
The thesis left such points for a separate, meta-guarded Delete points, and removing them at once
spares the user a step and leaves nothing invisible behind.

### Scale

The thesis scales with a knob, exponentially (chapter V).
Here Edit > Scale picks up one part, and a slider drawn in the band's words stands for the
knob.
It is a zigzag of one vector a doubling, 128 screen units long and 32 high, its corners the
ticks, at most five doublings each way inside the target's reach.
It stands 32 units below the pick, so as not to lie on a level line, or 32 above where it would
fall under the target's reach; the dot rides over it at the pick's height as its knob.
The factor at the pen's offset d is 2^k (1 + r/128), k whole doublings and r units more, kept in
1024ths and worked by `mul` and `div`, so it is continuous, monotone and needs no table.
Each moved coordinate is the center plus its offset at the pick times the factor, rounded, so
rounding never builds up.
A line goes about its middle or its fixed end, rewritten live as Move's drag of a whole line
rewrites it; an arc's start goes radially about its center, as Move's drag of a start changes a
radius.
A text, digits or an instance steps through Size's four sizes.
A step that would leave the target's reach or the arc module's radii is not made, and the knob
stays at the last factor applied.
The digits beside the knob are the status word's block, moved.
The scale module is in bank 6; it reaches the main bank's routines through a trampoline,
`mainCall`, since their `dap` returns cannot leave bank 4.

### Undo

The thesis has no undo.
Here one level of Undo takes back the last addition.
Every addition appends to the tables, so the record is the tables' counts before it, its kind, and
the points the solver moved for it.
Undo removes everything past the counts and puts the moved points back.
Anything that is not an addition clears the record, but a scale.
Just before a scale's first step every point's and origin's place is kept (the constraint
module's snapshot), and Undo puts them back, with the part's size and a Length's value.

## Masters, instances and copies

### Define and instances

The thesis's subpictures (chapters I, VI and VII) are kept, masters in four slots, instances that
show the master, and copies that become ordinary drawing.
Define writes the drawing's lines, arcs and attachers' crosses as the master's display words, and
keeps the master's points, lines and arcs in Copy's store in bank 5, its texts in the text module
and its constraints in bank 9.

The thesis nests instances to any depth, expanding them recursively at display time (chapter V and
VI).
Here a master's words never call another master.
Define takes every instance on the screen apart first and copies its parts into the new master as
plain words.
The 340 keeps one return address for its subroutines, so a master that called another master
could not return to the instance that called it.
A consequence is that redefining an inner master later leaves the outer one as it was.

### Attachers and tie points

The thesis's attachers (chapter VI) are kept.
A point designated in a master with Tie gives every instance a real point there, held by an
instance-point constraint.
Here that constraint is implicit, each instance keeping a list of its tie points, and the solver
reads the lists as it reads the arcs.
An instance's origin is a solver variable, laid out so that `pointX + 256 + i` is instance i's
origin.

Instances sharing a tie point are fastened.
In the thesis a dragged instance moved alone and its fastened neighbors were brought along by the
constraint satisfaction.
Here a fastened group is walked at the pick-up and carried rigidly under the pen, because the
solver does not run while the pen holds anything, and a group that followed only after the drop
would be left behind during the drag.

### Copy and designation

The thesis's Copy merges the last-designated attacher with what the pen aims at and the earlier
attachers with the earlier designated parts (chapter VII).
Here the copy is carried by its last attacher, which merges at the dot, and each other attacher
merges into the point designated in the same order, the designation made with Desig.
In the thesis Copy was a button pressed with the pen aimed, so one more attacher could be paired
with what the pen pointed at when the button went down.
Here Copy is chosen on the menu, so the pairing is by designation or, failing that, by the nearest
point.
The thesis has no way to undesignate, which it calls an oversight, and here a designated point can
be undesignated.

Copying a master that holds constraints copies them onto the new parts, so a constraint can be
defined by drawing it and applied by Copy, as the thesis's equal-length definition is.

### Change, cycling and the winking girl

The thesis's girl winks by Change instance, which swaps an eye's master for a closed eye (chapter
IX, figure 9.8).
Here Change gives an instance another master, lined up on the masters' attachers so that the
first tie point stays put.
The thesis shares one page frame between a master and its instances (chapter VI), so swapping
masters keeps the figure in place.
Here masters are kept about their centers, and the attachers line them up instead.
Define clears the drawing, so frames sharing one page frame would each be drawn without the others
in view, and nothing would say which masters belong to one animation.
Lined up by their attachers, the frames of an animation can be drawn anywhere.

Cycle goes past the thesis.
It steps an instance through the masters on the 1 ms clock, every cycling instance stepping
together on one sequence and one set of dwells, so both eyes of the girl blink together.
*SketchpadGirlWink.img* holds the sample.

### Dismember

The thesis's Dismember reduces an instance by one level (appendix B).
Here instances are one level deep, so Dismember turns an instance into its master's parts at its
place and size, with the master's constraints, each attacher merged into the tie point it had.

## Constraints

### What the thesis does

Each constraint type in the thesis is a generic block with a routine that computes an error, zero
when the constraint is met, scaled so that every type's error moves about one for one with a
displacement (chapter VIII).
The variables are satisfied by three methods.
SOLVE, a least squares routine, finds a variable's value that best meets all its constraints
together.
Relaxation applies SOLVE to one variable after another until the error is small.
The one pass method orders the variables so that each is solved once, exactly, and works for
most constructions and linkages but not for the bridge trusses.

### What the program does

The solver keeps every constraint by relaxation, one constraint at a time.
A sweep takes each constraint in turn and moves its points the least distance that meets it, a
projection, in 32nds of a unit, a fixed point never moving.
- H brings both ends to their mean y, or x.
- E brings both lines to their mean length, each about its middle, and L its line to the length
  kept.
- P turns both lines toward the direction between them, and R toward a right angle, each about
  its middle or its fixed end, with directions normalized by a square root and no angles.
- X shares the error by least squares, the point moving two thirds and the ends a third each.
- N moves the point onto the line through the ends, and the ends toward it, by least squares.
- C brings the point and the rim to one radius about the center, which it never moves.
- D, Y and O are described under *Numbers*, below.
- A tie shares the error between the tie point and its place on the instance.

Sweeps run about 40 ms of each pass of the main loop (the clock is read every fourth
constraint, so a pass may run up to three projections over), until a sweep moves no point more
than an eighth of a unit, or 8 s after the solver woke, when the figure is left where it got to.
The points that moved are redrawn at most every 50 ms.

While the pen holds anything, nothing is solved.
At the drop the figure settles first with the dropped point held where it was put, then with it
free.

### Why it differs

Projection replaces SOLVE because it needs no matrix and no linearization, and each type's
projection is a few multiplies, divides and square roots on a machine whose `mul` takes about 20
us and `div` about 35 us.
It meets the thesis's condition for stable relaxation, that a constraint's error be the same
whichever of its variables moves, since each projection moves all its points toward the same
answer.

The thesis's one pass method is not built.
With nothing solved during a drag, relaxation from the drop settles the thesis's hexagon in about
2 s and a four-bar linkage to within a unit in about a second, fast enough to follow.
The thesis itself reports relaxation as fast enough for the hexagon.

Solving nothing while the pen holds a point keeps the target and the dragged lines at full speed
during the drag, which on this machine would otherwise share the CPU with the sweeps.

### Show

The thesis shows a constraint as a small circle with its letter at the average of its variables,
with arms to each variable, and notes that the symbols overlap and one letter is not always
enough (chapter V, figures 5.3 and 5.4).
Here every mark is a dim circled letter drawn by the 340 from shared glyphs, its circle a 16-sided
ring of radius 10.
Its place is the average of its points, but a letter whose average lies on one line, H, L, X, N,
B and W, stands beside the line, a second on the other side and a third a ring further out, which
answers the thesis's overlap.
Equal, Paral and Perp, the marks that relate two lines, have dotted arms from one line's middle to
the other's, through the circle.
The thesis's mark for a scalar, a `#` on its digits, is left out, since the digits show the
scalar.

## The truss

### What the thesis does

The thesis's bridges (chapter IX, figures 9.6 and 9.7) are made of bridge beams, each a definition
of two constraints and a number copied onto a line.
The constraints keep the beam's length, and the number shows the change in its length since the
loads were applied, which is proportional to its force when the beams' cross sections are taken
proportional to their lengths.
The forces come from relaxing the whole truss, and new forces took about 30 seconds.

### What the program does

Here a truss is marked, not built of constraints.
Beam makes a line a member, Load a line a load of ten times its length, Roller lets a joint slide
along an axis, and a fixed point is a pin.
The analysis is a linear one, beside the solver, on the drawing as drawn, and never moves a point.

Each member is a spring of stiffness k, 1 unless Stiff set it, and the program solves K u = f for
the joints' displacements u, then reads each member's force from the stretch.
The unknowns are each joint's horizontal and vertical displacement, or a roller's displacement
along its axis.
They are numbered in the reverse of a breadth-first order from a joint with the fewest neighbors,
which keeps the matrix's profile narrow.
The stiffness matrix is assembled into its profile and factored as L D L^T a row at a time, then
solved by forward and back substitution, in the floating point of *spfloat.ac*, three words
a number, a 33-bit magnitude.
A pivot that is not positive, or not over 2^-23 of its diagonal, means the truss can fold, and
the readouts show dashes and UNSTABLE.

The matrix lives in banks 14 and 15.
The analysis runs a slice of each pass of the main loop, only while Forces is on, the solver sleeps
and nothing is held, and runs again when a snapshot of the truss's entries and points changes.

### Why it differs

Built as the thesis builds it, a truss settles by relaxation only as far as the coordinates show
the stretch.
The thesis's 36-bit coordinates hid that stretch.
On a 1024-unit screen, in 32nds of a unit, a force large enough to read needs a sag large enough to
see, and relaxation on a loaded truss cycles in an order-dependent way.
So the forces are worked out directly, as an engineer would, and the drawing is left as drawn.

Relaxing the linear system by sweeps, as the drawing's constraints are, fails on many drawn
trusses, since the stiffness matrix is often badly conditioned.
Of 279 drawn trusses, sweeps left 42 unanswered and 71 more inexact, whatever their settings.
The direct solve answers them, gives the sample bridge's forces exactly, finds a truss that can
fold in about 2 s, and solves the sample bridge in about half a second, against the thesis's 30.

Rollers, roller axes and member stiffnesses go past the thesis.
The thesis fixes both ends of its bridge and remarks that a real bridge is fixed at one end and
free to slide at the other, which Roller provides.
A stiffness counts only in a truss with more members than it needs to stand.

## Text and numbers

### Text

The thesis's text is a line of up to 36 characters, typed on the Lincoln Writer (chapter V).
Here a text is up to 24 characters, typed on a keyboard drawn on the screen, and kept as 340 codes,
three to a word.
A text has no points, so nothing joins it or constrains it, and it is moved and sized whole.
Rotated text is left out, since the 340 cannot rotate a character.

### The pen keyboard

The keyboard replaces the frame while it is open, through `frameGo`, and every pen hit goes to it.
Its keys are 80 units apart at scale 2, so that no frame can hit two.
A key acts at the first hit of a press, and a press ends after 250 ms with no hit, which is the
only sign of a lift the program has.
The pen sees nothing over the blank between keys either, so the gap is long enough for a drag to
cross it without typing again.

### Numbers and dimension lines

The thesis separates a scalar, a value, from its digits, which show it, and builds dimension lines
from constraints on scalars, D a scalar equal to a distance, Y a scalar equal to a change in a
distance, and O a point kept beside the digits (chapter V, chapter IX, figure 9.5, appendix A).
The program keeps that separation.
A scalar is a value in thousandths of an inch, at 100 screen units to the inch, fixed once a value
is typed.
Its digits are a text placed by an anchor, an ordinary solver point at their middle that the pen
never locks onto, so only a constraint is ever on it.
- D drives its points' distance from a fixed scalar, reads the distance into a free one it alone
  binds, and brings several to their mean.
- Y sets its scalar to the distance less the distance when it was made, and moves nothing, a
  one-way constraint as in the thesis.
- O keeps its point at a fixed distance from the anchor, room for five digits and a sign.

The scalar is not a solver variable.
D and Y write it inside their projections.
An instance shows its master's numbers doubled for each step of size, as the thesis has an
instance's magnification scale the value shown (chapter V).
*SketchpadDimension.img* holds the thesis's 3-4-5 triangle dimensioned.

## The view

The thesis's drawing lives on a page about a quarter mile on a side, seen through a window that the
knobs move and scale over a range of about 2000 (chapter V).
Here the page is the screen's size, 1024 units, and the view magnifies it by 1, 2, 4 or 8 about a
center, with Pan dragging the drawing as the knobs did and Center recentering on the dot, as the
thesis's Garbage button did with the pen tracking.
The view has no rotation, as the thesis's window has none.

A larger page in finer units would change every table, the solver's units and the tape, so the
smaller version was chosen.
Points are placed to the page's unit at every view, so magnifying shows more but does not place
finer.

Every writer of a canvas word passes it through the view, `screen = ((page - center) << s) + 512`,
and clips it to the screen, since an edge violation would halt the display.
Lines are clipped by Cohen-Sutherland and split as before.
An arc is drawn over its longest run on the screen, in finer chords.
An instance shows only while its whole figure is on the screen and its scale times the view's is
x8 or less, since the 340 cannot clip a subroutine or draw past x8.
Marks keep their size and are placed through the view.

## Saving pictures

The thesis saves to magnetic tape, compacting storage first, and keeps a library tape of
definitions (chapter III, appendix B).
Here a picture is saved to the Type 550 Microtape, sixteen pictures a tape.
A picture is the tables as they stand, a few runs of words, since every display word can be
written again from them.
- Picture p is one stream of words over 32 blocks, in a slot of 35, the last 3 spare.
  P1 to P8 have the slots from block 1, the directory is block 281, and P9 to P16 have the slots
  from block 282, so every picture is within eight slots of the directory.
- The stream is a header, the constraint table, the tie lists, the name, the cycle's settings,
  the masters' constraints, the texts, the numbers' digits and the tables, then zeros, and the
  header again as its last words.
  The parts follow one another, not starting on a block.
- The header holds a magic word, the format, the stream's size and a generation word new at each
  save, so a save cut short, whose two headers differ, reads as empty.
- The directory holds each picture's format word, zero for none, and its name.
  A save writes it after the picture, and Dir, Save and Load read it to show the sixteen names,
  Save and Load picking a picture by a touch on its line.
  Only the pictures' headers say whether a picture is there.
  A directory block that fails its checksum reads as sixteen empty entries, and the next save
  rewrites it.

A load reads the picture forward once to check every block's checksum and both headers, and
changes nothing unless all is well.
Then it searches back to the picture's last block and reads the stream in reverse straight into
the tables, a block read backward giving its words last first, so the tape does not run back to
the start of the picture.
If that second pass fails, which a tape that has not changed between the passes cannot do, the
tables are emptied.
Then the display is rewritten.

The Microtape gives or wants a word every 200 us, and the program moves every word itself, so a
transfer runs with the break system off.
Each run of words is a pointer, a count and a step, and the next run is taken inside the word
loop, between two words.
Meanwhile the display runs a small list that loops on itself, showing WAIT and needing no CPU.

The format is 14.
It is the only format a load accepts, and any other format reads as EMPTY rather than loading
wrongly.
An instance's scale word keeps its low two bits for the Type 340's scale and the rest for a
multiplier, which this program always saves as zero.
A word with a multiplier loads at the power of two, and *sketchplot* draws it at its size, so
sizes between the powers of two can be added without a new format.

*sketchplot* reads a picture from a tape image and draws it as a PNG, the reading half of the
thesis's Plot.

## Timing

Measured on the emulator.

| What | Time |
|---|---|
| A frame, empty drawing | About 1.4 ms, over 700 frames a second |
| A frame, 64 short lines | About 15 ms |
| A frame, 192 short lines | About 44 ms |
| A frame, 192 lines across the screen | About 0.3 s |
| A held pen's cost | About 12% of the frames |
| A change at 192 lines across the screen | 3 to 6 ms |
| A whole rebuild | 0.35 s at 64 short lines to about 1 s at 192 across the screen |
| A character drawn | About 27 us |
| The hexagon in a circle settling | About 2 s |
| The sample bridge's forces | About 0.5 s |
| A truss that can fold found UNSTABLE | About 2 s |
| A load or save of P8 or P9, beside the directory | 4 to 5 s |
| A load of P1 or P16, from the directory | 16 to 19 s |
| A save of P1 or P16, from the directory | About 30 s |

## Divergences from the thesis

| The thesis | Here | Why |
|---|---|---|
| Push buttons, toggle switches and knobs | A menu bar worked by the pen | The PDP-1 here has no button box |
| A pen flick ends an operation | A lift ends it | A mouse lifts, and a flick was hard to use and hard to tell from a fast stroke |
| Pointing by the pen's hits on tagged spots | Pointing by search from the target's center, the drawing not pen-sensitive | A hit pauses the 340, and a search gives the nearest object directly |
| Tracking about 100 times a second, no prediction | About 200 presentations a second with the emulator's predictor | A frame grows with the drawing, and the gap between presentations is what loses the pen |
| A ring structure with generic blocks | Dense tables, the last entry moved into a hole | Far less code, and a picture saved as a few runs of words |
| Circles by a difference equation | Each vertex rotated directly from the start | No error builds up, and every circle closes |
| Instances rotate and scale continuously | Instances translate and scale x1, x2, x4 or x8 | The 340 scales by those four and cannot rotate a subroutine |
| A scale knob, exponential | Edit > Scale, a slider whose doublings are equal steps, for one part at a time | The PDP-1 here has no knobs; the slider keeps the thesis's exponential feel |
| Instances nest to any depth | Define flattens instances into the master | The 340 keeps one return address |
| Fastened instances follow by satisfaction | A fastened group moves rigidly under the pen | Nothing is solved during a drag |
| Masters and instances share a page frame | Change lines masters up by their attachers | Define clears the drawing, so frames could not be drawn over one another |
| SOLVE, relaxation and the one pass method | Relaxation by projection, one constraint at a time, nothing solved during a drag | No matrices, cheap on this machine, and fast enough for the thesis's examples |
| One constraint letter per type, P for two | A letter per menu row | Each mark says which row made it |
| A beam of constraints, forces by relaxation | A linear analysis by direct solve beside the solver | Small coordinates hide the stretch, and the direct solve is exact and fast |
| Bridges fixed at both ends | Rollers, roller axes and stiffnesses | The thesis notes a real bridge slides at one end |
| Text of 36 characters, typed on a keyboard | Text of 24 characters, typed with the pen | 24 is the pen keyboard's limit, and the PDP-1 here has no keyboard at the screen |
| A page a quarter mile on a side | A page the screen's size, magnified up to x8 | A larger page would change every table, the solver's units and the tape |
| Unattached points deleted separately | A point left alone is removed at once | Nothing invisible is left behind |
| No undo | One level of Undo for additions | Cheap, since every addition appends |
| No undesignate | Desig undesignates | The thesis calls its lack an oversight |
| A scalar shown as `#` | Not shown | The digits show the scalar |

## Not implemented

Ruled out, since the Type 340 cannot rotate a subroutine or a character.
- Rotated and mirrored instances, rotated text and digits, and the thesis's I and E constraints

Not built.
- Equal at ratios other than 1, and instance sizes other than x1 to x8 with the constraints on size
- A larger page, the full zoom
- Whole canvases held in memory for a fast load
- A library of masters, and members inside instances
- Plotter tape and punched output, the thesis's Plot and Punch, and saving as text
- Order and Disorder, which sort lines for the plotter

## The source files

| File | Bank | Holds |
|---|---|---|
| *sketchpad.am1* | | The variables, the tables, the bank directives, the includes |
| *sketchpad.ah* | | Every constant but the intensities, and `BOXAT`, the one macro two files use |
| *intensities.ah* | | The intensity of each kind of thing drawn, the tracking target's among them |
| *spbreak.ac* | 0 | The break vector, `boot`, `penISR` |
| *spframe.ac* | 0 | The frame, the menus, the slots, the marks and glyphs, the master store, WAIT |
| *spdispatch.ac* | 3 | The mode and menu dispatch tables, the lists' descriptors |
| *spmain.ac* | 4 | Init, the main loop, the clock, the frame rate, the solver's pass |
| *spmenus.ac* | 4 | The menu, lists, sublists, the OK box, the tape's rows |
| *spundo.ac* | 4 | Undo |
| *spmodes.ac* | 4 | Each mode's handlers, the gates |
| *sppen.ac* | 4 | The pseudo pen location, the band, a stroke's commit |
| *spdisplay.ac* | 4 | The target calls' spread, the line list's rebuild |
| *sparcs.ac* | 2 | The arc module |
| *spedit.ac* | 2 | Removal, deletion, merging |
| *spdrag.ac* | 2 | A drag's arcs |
| *spdefine.ac* | 2 | Define's master words, Copy's arcs and merge |
| *sparcdata.ac* | 2 | The arc tables, the masters' directory, the sine table |
| *sptape.ac* | 6 | Saving and loading pictures, the tape directory and its screen |
| *spinit.ac* | 6 | Start-up's display gates |
| *spcensus.ac* | 6 | `rowCensus`: what the menu's rows act on, counted as a list opens |
| *spscale.ac* | 6 | The scale module: Scale's pick, slider, steps, drop and Undo |
| *spinst.ac* | 7 | The instance module |
| *sprows.ac* | 7 | `rowCan`: whether a row can act, by item |
| *spcons.ac* | 8 | The constraint module and solver |
| *sptext.ac* | 10 | Texts and numbers |
| *sptruss.ac* | 11 | The truss analysis and readouts |
| *spfloat.ac* | 11 | The floating point |
| *splock.ac* | 11 | The dot, the lock mark, the lit line |
| *spaux.ac* | 12 | Routines kept out of banks 0 and 4 for room, start-up's first part |
| *spkbd.ac* | 12 | The pen keyboard, and the directory's screen's touches |
| *spview.ac* | 12 | The view, clipping, the constraints' arms |
| *spmarks.ac* | 13 | The constraints' marks |
| *spsearch.ac* | 13 | The line search |
| *spvarc.ac* | 13 | Arcs through the view |
| *Render/sketchplot.c*, *Render/Makefile* | | The picture renderer |
