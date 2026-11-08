# **Sketchpad** technical design

This document describes how Sketchpad, the light pen drawing program for the PiDP-1 and its Type
340 display, is built and how it works, and ties each part back to Ivan Sutherland's 1963 thesis.
Where the program does something differently from the thesis, it says what and why.
How to use the program is *UsingSketchpad.md*.

This is version 2.0\
Edit date 4-Nov-2026\
Updated to include the remaining thesis functionality

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
| Storage | Magnetic tape, library tape | Type 550 Microtape, twelve pictures a tape, or a library tape of up to 256 masters |
| Hard copy | Plotter | Misc > Plot on the CalComp plotter, device 73, and *sketchplot*, a PNG drawn from the tape image |

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
*intensities.ah*, the intensities, and thirty-nine *sp\*.ac* files in memory order.
All of them are in *FunStuff/Sketchpad*, and the program is assembled there.
It starts at `boot`, at 100, which opens the extend window and enters `initVars` in bank 12, then
`initGates` in bank 11, then `init` in the main bank, bank 4.

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
5. `checkLost` declares a lift once no target hit has come for 100 ms and two frames have ended
   since the last, and runs the mode's lift handler.
6. `onClock` runs the timed work, a cycle's step, a label's flash, an idle list's closing and
   the display watchdog, which restarts the display when no frame has ended for 500 ms and its
   address (`dra`) stands still over two clock ticks, so a frame drawing for longer than that is
   left to end, not started over. The status word is not timed: a fresh press in `onMenuHit`
   or a hit in `onTargetHit` hides it (`msgHide`).
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
| `arcCall` | Arcs, the removal and merging of points and lines, Copy's geometry | 2 |
| `tapeCall` | Saving and loading pictures, a drum overlay; its kept words and stubs in bank 11 | 6 |
| direct calls | Scale, its slider and its Undo, a drum overlay; Figure and All a second one, the group scale | 6 |
| direct calls | The pen keyboard, a drum overlay | 6 |
| `instCall` | Instances, masters, Copy, Change, cycles; Define and Dismember a drum overlay | 7 |
| `conCall` | Constraints and the solver | 8 |
| `txCall` | Texts and numbers | 10 |
| `taInit`, `taTick`, `taRedo` | The truss analysis and its floating point, a drum overlay; its stubs in bank 11 | 6 |
| direct calls | What a lift will take | 11 |
| direct calls | The view, the target's clipping, routines moved for room | 12 |
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
These are the constraints' arms in bank 5, the texts in bank 12, and the truss's readouts in bank
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
Only its display slot splits it, into the fewest vectors of at most 127 units on either axis, up
to 9, the remainders of the division spread so that every step ends within a unit of the true
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
Define takes the lines greedily, each next one the nearest not yet drawn, from either end, so a
line that starts where the last one ended needs no move; the arcs follow, then the words of the
instances the master holds, composed in place (see *Define and instances*), then the attachers'
crosses.
An instance is a slot that sets the scale and the origin's position and calls the master with
`save`.
The 340 scales relative vectors by 1, 2, 4 or 8, so an instance's size is one of those, at no cost
in drawing time.
A master is at most 128 lines, 530 words and 20 ms of drawing, about four periods of the
target's presentation, since the target cannot be called from inside a master: 20 ms is the most
that still holds a pen moving at 1000 units/s, measured.
Every master's words are kept in its record on the drum (see *Drum overlays*), and those the
drawing calls are also in a store of two regions, 398 words of bank 0 and the last 530 of bank 1,
because `save` reaches only banks 0 and 1.
A master lies whole in one region: the 340 returns from a `save` at its first escape from vector
mode, so a master cannot `jump` on from one bank's part to the other's.
Define counts the drawing's words first, refusing it as too many for a region or too slow, writes
the master's record, and empties the store, since the canvas it clears leaves no slot calling a
master.
A master is called while it is an instance's, while Undo would put back a dismembered instance of
it, and, while any instance cycles, every master of the cycle's sequence.
The first call brings its words in from its record, into the free run that fits it most tightly,
taking out first any master nothing calls whose room is wanted; with no room even then, the Inst,
the Change or the cycle's start is refused.
Each master's start is kept in the masters' directory, and every instance's slot is written from
it.

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
The program limits what the screen shows to 360 characters, each text counting two more, about
9.7 ms of drawing, half of the 20 ms a master may take, so that text cannot lose the pen.

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
at least four times since, counted from the second frame end after the hit, so that a display
that stops showing the target is not taken for a lift.
The second frame end matters for a pen taken to the menu bar, which the pen sees only when the
bar is drawn, first in each frame: a frame drawn whole after the hit has shown it the bar, so a
trip to the bar is not taken for a lift however long a frame takes (about a third of a second
with 192 lines across the screen, when a lift is declared up to two frames after it).

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

At each target hit, `penISR` reads `drc`, which gives the emulator's predicted pen position, and
moves the target there.
The prediction is an addition of this emulator's Type 340, a predictor that makes up for the lag
between the mouse and the display.
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
  at a line's end the line locks; in Scale > All nothing need be locked.

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
| Copy, with the definition in toggle registers 20 to 23 | Picture > Copy, a master picked |
| Instance, with the picture in toggle register 25 | Picture > Inst, a master picked |
| Change instance | Edit > Change > Master, a master picked |
| Dismember | Edit > Dismem |
| Tie | Picture > Tie |
| Designate | Picture > Desig |
| Fix | Edit > Constr > Fix |
| Unfix | Misc > Unfix |
| Delete constraints, meta-guarded | Misc > Del cons, guarded by its OK |
| Delete picture, meta-guarded | Picture > Clear, guarded by its OK |
| Text | Draw > Text |
| Number | Draw > Dim > Number |
| Library, Library write | Misc > Library > Load, Misc > Library > Save; a whole picture by Misc > Load and Misc > Save |
| Garbage, recentering on the pen | View > Center |
| The scale and position knobs | View > x1 to x8, and Pan; Edit > Scale > Part for one part's size, Figure and All for many parts' |
| Toggle switches showing the constraints | Edit > Constr > Show |
| Sheet of paper, the toggle choosing a picture | Define, a master picked and named, and Clear; Picture > Name renames a master |

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
- Texts, their origin, size, characters and 340 code run, 64, and the masters' texts, 64.
- Scalars, their value and whether it is set, 64.

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
| `CTEQ` | E, M | 4 | Two lengths equal (E), or A-B k times C-D (M), k 2 or 3 in the parameter's low bits, 0 for E; x1/3 and x1/2 are stored as 3 and 2 with the lines exchanged |
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
| `CTSAME` | S | 2 | Instance A at a ratio of instance B's overall size, the parameter the ratio's index |
| `CTFULL` | U | 1 | An instance at x1 |
| `CTINCH` | I | 1 | An instance's overall size, the parameter in thousandths of an inch |
| `CTSZNUM` | Z | 3 | A scalar equal to an instance's size in inches, A twice, C the digits' anchor |

The last four name instances, as `MAXPTS` plus the instance's index, not points.
The solver numbers an instance's origin the same way, so a mark stands on such an index as it
stands on a point.
Size is not a solver variable: a size change walks a variant from the drum at 0.8 ms a word, so
nothing can happen inside a sweep.
These entries are records the sweeps pass over, and a resolver acts on them at a commit.
The resolver, in the overlay `scale`, works each instance's overall size out in 16ths of a unit:
the absolutes first, then the instance changed, then, while a group has no known member, its first
entry's present size, each followed by passes over the S entries until none changes.
An overlay cannot bring a master's record, the window holding one record and only root code reading
the drum, so a root runner, `szRun` in *spsize.ac*, calls the overlay and brings each record it asks
for.
The main loop calls the runner, not the solver, since the overlay calls the constraint module,
which is not reentrant.
A refusal sends the instance changed back, and takes a new entry off.

The thesis's letters (appendix A) are kept where they are free, H, C, X, P, D, Y, O and M, M for
a length at a ratio of another's other than 1.
The thesis uses P for both parallel and perpendicular, M for equal length too, L for a point on a
line and W for a fixed distance.
Here each row has a letter, named for the menu, so E is equal, R is perpendicular, L is a
length and N a point on a line.
B and W here are the truss's member and load.
The thesis's B makes a scalar equal to a size, here Z, and its W keeps a distance, which with Y
built its bridge beams.
The thesis's S keeps its letter; its letter for full size is lost in the scan, so U here, and its
inch sizes are I, a letter free since its I constraint needs rotation.

### Memory

Each structure that grows has a bank, and each module a bank, so that one can grow without moving
the others.

| Bank | Holds |
|---|---|
| 0 | The break vector, `boot`, `penISR`, and every display word the 340 reaches by `jump` or `save`, the frame, the menus, the arc and instance slots, the marks and their glyphs, the masters' first region |
| 1 | The line slots, then the masters' second region |
| 2 | The arc module, merging and removal, Copy's geometry, Define's working words and the held instances' blocks, the arc table |
| 3 | The point, line and instance tables, the dispatch tables, the scalars, some of the truss's tables |
| 4 | The main bank, the main loop, the menus, the modes, Undo, the gates |
| 5 | The window MW, holding one master's record, its points, lines, arcs, constraints, display words and the instances it holds; after it, the masters' attachers, directory and nesting, the constraints' arms list, a saved copy of the arc module's working words, the library's screen and its directory's header and page, the masters' picker's list and lines, each line holding its master's name, what a master's remaking keeps, the offsets of an arc's first quarter, Scale's Undo record and the variants' tables |
| 6 | The overlay window, holding the scale module, the pen keyboard, the group scale, Define and Dismember, the tape module, the library, Plot or the truss analysis; after it, Scale's kept words and stubs, the pen keyboard's entries and kept words |
| 7 | The instance module but its Define and Dismember parts, their stubs, which masters the drawing calls, the masters' name keyboards' blocks and the name typed |
| 8 | The constraint module |
| 9 | The constraint table, the solver's working tables, the masters' constraints' counts, the variants' lookup and sweep, which rows can act |
| 10 | The text and number module, the canvas's texts' tables, the instance count and each instance's master |
| 11 | The overlay manager, the tape module's kept words, directory, screen and stubs, the tape's mount, seek, read and write, which the tape module and the library both call, the tape rows' steps, the library's kept words, stub and steps, the masters' picker's blocks, keys and hits, start-up's display gates, the menu rows' census, the truss's stubs, what a lift will take, the masters' texts and their numbers' digits, the overlay table |
| 12 | The view, the target's clipping, start-up's first part, the texts' list buffers |
| 13 | The truss's snapshot and readouts, the constraints' marks, the line search, arcs through the view |
| 14, 15 | The truss's stiffness matrix, and the library's entry buffer from bank 14's start, which a library save or load takes over, starting the analysis again |

Every bank but 2, 3 and 5 is close to full.
When a bank runs short, more is moved out of it than the change needs, so that the next change
finds room.
The main bank runs with the extend window open and reaches bank 0 and the tables by full address.
Bank 0's words 4 to 077, unused by the single-channel break, hold display words too.

### Drum overlays

The scale module, the pen keyboard, the group scale, the instance module's Define and Dismember
parts, the tape module (Save, Load, Dir), the library (its Load, Save, Dir and Delete), Plot, the
marks' ranks and the truss analysis are drum overlays, kept on the Type 23 drum's tracks 11 to 21
octal, one a track, and
read into one window, W, at bank 6's start when called, by am1's overlay manager
(*UsingOverlays.md*).
The window is 3,877 words, 8 more than the largest, the truss's 3,869, the header included, so
that an overlay growing by a few words still assembles; Scale's takes 3,116, the keyboard's
1,226, the group scale's 2,751, Define and Dismember's 3,857, the tape's 3,455, a master's entry's buffer among them,
the library's 2,246, its entry's buffer being in banks 14 and 15, Plot's 2,743, its
characters' strokes' table of 1,087 among them, and the ranks' 216.
The ranks (*spranks.ac*), which set apart the constraints' letters that would stand at one place,
run only when a constraint is added or removed, so their load waits on a command, never on a
drag; called while another overlay holds the window, they are counted at Show's next pass.
The tape's mount, seek, read and write are in bank 11's root, since an overlay cannot call
another and both the tape module and the library need them.
The sixteen masters' records are data overlays, packed two a track at their own length, one each
after the keyboard's, the library's and the ranks' overlays on tracks 12, 16 and 21 octal, and
the rest on 22 to 27, read into a second window, MW, at bank 5's start, 1,636 words: a master's display words and Copy's store, its
points, lines, arcs and constraints, and the instances it holds, at the same addresses whichever
record is resident.
They are persistent, so Define writes the record in the window and the overlay manager writes it
back to the drum when another record takes its place.
MW is a two-pass window: the manager writes the outgoing record, then reads the incoming one, so
a stop during the write (an ad1 stop, the STOP switch, a single step) is retried and loses no part
of the drawing, at a cost of up to a drum revolution, about 35 ms, more an eviction than a
single transfer swapping the two.
A master's attachers, its directory entry and its nesting (how many instances it holds, its own
words' count, which masters it holds, and how many of its texts are its own) stay in the root, so the solver, a carry, Change and
the pickers read them with any record, or none, resident.
The lowest tracks, 0 to 10 octal, are left to other programs (the owner, 21-Oct-2026), and am1
places the overlays above them.
W holds the truss and the group scale, persistent, and swaps them in one transfer, so am1 gives
each of W's overlays a track of its own at the same drum address, first; it packs the records, MW being two-pass, at
their own length after them, which leaves tracks 30 to 37 octal free.
Define and Dismember are one overlay, since Define takes the instances of the master being
defined apart as Dismember does; Define's words, and their composing, are in it too.
Their stubs are in bank 7, and the words they keep between calls are bank 7's and bank 2's.
Only Define's first call, a Dismember's first lift and Undo of a Dismember can read them.
All but the truss and the group scale are read-only, so a call that finds another in the window
reads its own over it, the display dark for one frame gap of about 13 ms (Scale's, measured).
What each keeps between calls stays outside the window: the tape's directory, the picture's
name and the pen keyboard's blocks, Scale's knob, counts and Undo's record, so Undo of a Scale
reads the module back after a Save, and the keyboard's press, lit key and pick.
The truss keeps its tables, over a thousand words, in the window: it is persistent, written back
to its track in the same transfer that reads the overlay taking its place, so an analysis goes
on where it was after a Save or a Dir, and is not started again.
The group scale is persistent for its Undo record, kept in the window, so a group's scale can
be undone after the pen keyboard or a Save has taken W in between.
Each entry its callers call is a stub under the entry's own name, in bank 6 for Scale and the
keyboard, bank 7 for Define and Dismember, bank 11 for the tape, the library and the truss, which makes the
manager's call, so the callers' code is the same as without overlays.
The truss's stub `taTick`, which the main loop calls each pass, calls the truss only when it has
work: Forces on, readouts shown or Stiff mode, and neither Scale the mode nor the keyboard open,
whose overlays hold the window meanwhile, the readouts hidden; `taInit` at start reads nothing,
so a session that never turns Forces on never reads the truss, and `taRedo`, from the view,
notes the change for the pass's call.
Scale's `scDone`, which every mode change calls, returns at once when no part is held, so a mode
change reads nothing.
The keyboard's per-pass work stays outside the window too: `kbTick`, which the main loop calls
each pass, and `kbKey`'s test for a press under way run in bank 6's root, so the manager is
called only when the keyboard opens, at a press's first hit and when a key's flash ends.
A Dir or a Save reads the tape module and then the keyboard, which shows the directory and the
names, so the keyboard is in the window after either.
A load that fails, the drum holding another build's overlays or none, goes to the program's
hook, not a halt: DRUM shows under the bar, the mode is left, and the main loop goes on.
When it was the truss's, at a pass's end, the mode stays, and the stub tries again after 10 s.
When it was the keyboard's that failed, the hook also turns tracking back on if the keyboard had
turned it off.
The drum asks for a break at the end of each transfer; `penISR` counts it apart from the
display's while a stub's call is under way.

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
It is a square wave read as a ruler: a level run of one vector a doubling, 128 screen units
long, on its two lines 32 apart by turns, joined by upright ticks at the doublings, at most five
doublings each way inside the target's reach. Its up to 13 words are the band's 9 and 4 more
before them (`slideVec`).
It stands 32 units below the pick, so as not to lie on a level line, or 32 above where it would
fall under the target's reach; the dot rides over it at the pick's height as its knob.
The factor at the pen's offset d is 2^k (1 + r/128), k whole doublings and r units more, kept in
1024ths and worked by `mul` and `div`, so it is continuous, monotone and needs no table.
Each moved coordinate is the center plus its offset at the pick times the factor, rounded, so
rounding never builds up.
A line goes about its middle or its fixed end, rewritten live as Move's drag of a whole line
rewrites it; an arc's start goes radially about its center, as Move's drag of a start changes a
radius.
A text or digits step through Size's four sizes.
A step that would leave the target's reach or the arc module's radii is not made, and the knob
stays at the last factor applied.
An instance takes any size from x1/4 to x8, though the 340 scales a subroutine only by powers of
two.
A size is a power of two times a multiplier in (1/2, 1], the 340 doing the first and a *variant*
the second: a copy of the master's display list, in the master store, each place in it the
master's times the multiplier, rounded, so that a figure still closes and a tie point stays on its
cross, and the move from the last place the difference.
So a place on the screen is always that rounded place shifted by the 340's scale, and nothing else
a variant needs is new: it is as long as its master, and a variant is found by its master and
multiplier, which the instance's own words hold.
There are 16 entries, the multipliers lying on 32 sizes to a doubling so that instances showing
the same digits share one, and x1.5 and x3 sharing one since only the multiplier keys it.
While the pen slides nothing changes but a box drawn dim and pen-blind, 9 words past the band at
the smallest 340 scale that reaches its longer side, its sides rounded to that scale; a size whose
box would leave the screen is held.
The walk takes about 0.8 ms a word, at the lift, once: 85 ms for a master of 108 words.
A variant no instance has stays until room is wanted, and a master all of whose instances are at
a multiplier leaves the store the same way, since nothing draws it.
A cycling instance stays on the powers of two, so that a sequence does not need a variant at each
step.
Define keeps an instance at a multiplier by reading its variant, so its master need not be in the
store, and a holder's size is kept whole, multiplier and all, with its place in the holder.
A redefinition recomposes such a holder from the held master's words, each place by the same rule
as the walk, about 0.8 ms a word on each pass.
Its trial keeps every variant, since the trial's instances are emptied and a refusal gives them
back: so a redefinition with instances at a multiplier on the canvas can be refused for room an
empty canvas would have had.
The digits beside the knob are the status word's block, moved.
The scale module is in bank 6; it reaches the main bank's routines through a trampoline,
`mainCall`, since their `dap` returns cannot leave bank 4.

Edit > Scale is a sublist: Part is the above, Figure and All the group scale, a second overlay
(*spgscale.ac*), the root's stubs choosing between the two by a word that Scale's sublist sets.
All takes every part. Figure takes the parts joined to the one the pen locks onto: at the pick a
union-find over the points and the instances' origins, with path halving, unites each line's
ends, each arc's center with its start and end, each instance's origin with its tie points, and
each number's anchor with the point or origin it reads, one pass of each table; every word is
then made its root, and the seed's root marks the selection. A plain text is tied to nothing,
so a Figure never takes one and All always does. About 0.1 s at 192 lines, once at the pick.
A group is never rewritten live: a whole list's rebuild at each step would take 0.35 to 1.0 s,
the target not following meanwhile. So while the pen slides only a box round the selection
follows the factor, the instance's box's 9 words, and the lift applies the scale at once, about
the box's middle or the selection's one fixed point (two at different places refuse the pick,
CONFLICT). Every point and origin goes to its place at the factor exactly, texts and digits to
the nearest of Size's sizes, instances to the slider's nearest (a cycling one the nearest power
of two), the Lengths and Changes on lines in the selection scaled, then one rebuild and one
settling of the solver. Everything that can refuse is checked before anything moves: a part off
the screen (OFF EDGE), or more than the 16 variants' sizes in use after the scale, the instances
outside the selection counted (NO ROOM). The instances' sizes are set in two passes, every
selected instance's variant released first, so that the old and the new sizes never need the
16 entries at once; the new variants are built from the root (bank 5's `gsRun`), since a
variant is written into the master store, which an overlay in W cannot reach while it runs.

### Undo

The thesis has no undo.
Here one level of Undo takes back the last addition.
Every addition appends to the tables, so the record is the tables' counts before it, its kind, and
the points the solver moved for it.
Undo removes everything past the counts and puts the moved points back.
Anything that is not an addition clears the record, but a scale.
Just before a scale's first step every point's and origin's place is kept (the constraint
module's snapshot), and Undo puts them back, with the part's size and a Length's value.
A group's scale keeps the same snapshot and the Lengths' and Changes' values as Part's does and,
in the group scale's own words, every text's place and size and every instance's size word, a
record that covers every table at its limit, so no scale is too large to undo; the overlay is
persistent, so the record survives another overlay taking W, and one Undo takes the whole scale
back.

## Masters, instances and copies

### Define and instances

The thesis's subpictures (chapters I, VI and VII) are kept, masters in sixteen slots, instances that
show the master, and copies that become ordinary drawing.
Define writes the drawing's lines, arcs and attachers' crosses as the master's display words, and
keeps them with the master's points, lines, arcs and constraints in its record on the drum, its
texts in the text module.

The thesis nests instances to any depth, expanding them recursively at display time (chapter V and
VI).
Here a master keeps the instances it holds in its record, up to eight, each one's master, place
relative to the origin, scale and tie points, so the hierarchy is in the data.
Its display words still never call another master.
The 340 keeps one return address for its subroutines, so a master that called another master
could not return to the instance that called it.
Define composes them instead: each held instance's master's words are written into the new
master's, a beam-off move to the instance's place, each vector doubled to its scale, one that
outgrows its field split in two.
Its texts are copied in at the same place and scale.
Define refuses a loop, an instance whose master holds the one being defined at any depth.
An instance of the master being defined is taken apart one level as Dismember does, and the
instances it holds become the new master's, counted against the eight with the others.
A master's own texts come first among its texts, the copied ones after them, and the master keeps
their count, so that Dismember and Copy place only its own as texts and leave the rest to the
instances they add.
A library save of a master that holds instances carries every master it calls with it, see *The
library tape*.

Composed words go stale when an inner master changes, so a Define or a library load of a held
master makes every master holding it again, at any depth, inner ones first.
The thesis gets this free, by expanding at display time; here it costs a pass over each holder, at
the one moment no holder can be on the screen: Define clears the canvas and refuses an instance
that calls the master being defined, and the library's picker dims a master the drawing calls.
Each holder keeps its own lines and arcs, the first `mOwnLen` words of its record; its held
masters' words and its crosses are composed again from its sub-instances and attachers, its box
worked out again, and its derived texts dropped and copied again.
Tie points on its sub-instances go to their masters' new attachers, which Dismember ties to.
An attacher of the holder's own on such a point keeps its place, since moving it would change the
crosses' words after they were counted, and a line of the holder's ending there keeps its words.

All of it is a trial first, the master store its scratch space: each new master, inner first, is
composed into the store and judged.
Define's trial has the whole 1,042 words, every master having left the store with the canvas's
clear; a library load's has what the drawing's masters leave.
A refusal leaves everything as it was, the instances the Define emptied put back from their
tables.
A holder is refused if it would pass 530 words or 20 ms, if the masters' texts' table or the store
runs out, or if the master it holds would carry digits, which a holder has no way to keep
following their anchor; the refusal names the holder, by number under the menu, by name under a
library load's status word.
Then the commit writes each holder's record, and its copy in the store is given up.
A library load of a held master reads the entry from the tape twice, once for the trial and
again for the commit, each a call of the library overlay complete in itself.

A master's name, up to 12 characters, is kept in its line on the masters' picker, bank 5, a slot
label and a name's run as the tape directory's screen keeps a picture's, so the picker shows the
names with no other table and no drum read.
Define's keyboard and Define itself are both overlays in bank 6's window, so the name typed waits
in bank 7 until Define runs, and is copied into the line only when Define succeeds, a refusal or
a Cancel leaving the old name.
Every writer of a master writes its name: Define and Name the name typed, a library load its
entry's, a library save the name saved under, and a picture's load each master's saved name,
blanking the rest.

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
Copying a master that holds instances places them as instances, as Dismember does at x1.

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
Here Dismember turns an instance into its master's parts at its place and size, with the
master's constraints, each attacher merged into the tie point it had.
Each instance its master holds becomes an instance on the canvas of its own master, as in the
thesis, so a second Dismember takes the next level.
Its origin is the instance's plus its offset at the instance's size, and its size the two
composed: the shifts added, the multipliers multiplied, and the product brought back into
(1/2, 1] by moving doublings out of the shift, so that x1/2 at x4 is x2 with no multiplier.
A size past x8 or under x1/4 is refused.
The composed size stays exact, not snapped to the slider's 32 sizes a doubling, so a dismember
shows what it showed, and two bays taken apart the same way still share a variant.
Its tie points are the placed points for its ties, written before the attachers merge, so the
merges rename them as they rename every tie.
The held instances' masters are brought into the store first, pinned so that bringing one does
not take out another nothing yet calls, since the store cannot be filled while the outer master's
record is resident.
Those at a multiplier are brought as variants, from one sweep and a hold over the rest, since a
variant has no instance yet for a sweep to spare, and the words to bring are read from the record
first, as each variant loads another's.
Undo removes the added instances before it puts the instance back, since putting it back first
would overwrite the slot of the first one added.

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
  kept. M is E with A-B's length divided by k first: both lines free go to the mean of that and
  C-D's length, A-B taking k times it; one free goes to the ratio of the other. A type number
  could not be spared, every value past `CTSAME` naming instances, so M is an E with k in its
  parameter, and `cBad` does not swap its lines into E's fixed order, which would invert it.
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
A letter for one point, F, A, a designation's digit and a roller's O, stands beside its point in
whichever of eight directions the point's lines leave clearest, a second on the same point in the
next clearest, a direction past the screen's edge passed over, so that a pin's or a roller's
letter is not crossed by the lines it joins.
Equal, its ratios, Paral and Perp, the marks that relate two lines, have dotted arms from one line's middle to
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
Here a picture is saved to the Type 550 Microtape, twelve pictures a tape.
A picture is the tables as they stand, a few runs of words, since every display word can be
written again from them.
- Picture p is one stream of words over 23 blocks, then its masters' entries, then a closing
  block, in a slot of 47, so the entries may take up to 23 blocks.
  P1 to P6 have the slots from block 1, the directory is block 283, and P7 to P12 have the slots
  from block 284, so every picture is within six slots of the directory.
- The stream is a header, the constraint table, the tie lists, the name, the cycle's settings,
  the masters' attachers, the texts, the numbers' digits, the tables and the masters' nesting,
  then zeros.
  The parts follow one another, not starting on a block.
- Each defined master's entry follows from a block's start: its record's tables as far as its
  counts reach, its display words first among them and the instances it holds last, then the
  master's name, padded to a whole block.
  The name is last so that every entry's display words stay where *sketchplot* reads them.
  The tables go through one buffer, gathered into it before a save's seek and scattered from it
  after a load's read, since the word loop has no time for a run a table, and the seek's flag
  leaves only 400 us to start the transfer.
- The closing block is zeros with the header again as its last words.
- The header holds a magic word, the format, the stream's size, a generation word new at each
  save, so a save cut short, whose two headers differ, reads as empty, and the entries' blocks.
  A save counts the entries first and refuses a picture whose masters need more than 23 blocks,
  TOO BIG, before it writes.
- The directory holds each picture's format word, zero for none, and its name.
  A save writes it after the picture, and Dir, Save and Load read it to show the twelve names,
  Save and Load picking a picture by a touch on its line.
  Only the pictures' headers say whether a picture is there.
  A directory block that fails its checksum is refused, BAD BLOCK, by Dir, Save and Load, since
  the pictures behind it may still be readable.
- A save writes only on Sketchpad's own tapes or on blank ones.
  The directory block tells them apart: a picture tape's magic, of any format, or a library's is
  Sketchpad's; a block of zeros, every one of its 256 words, is a blank tape; anything else is
  another program's, which Dir and Load show as twelve empty entries and a save refuses, NOT
  BLANK, writing nothing.
  Another program may leave block 283 alone and still use a slot's blocks (the Z-machine's saves
  start at block 1), so a save onto a blank directory first reads every block it will write, in
  a loop of its own that keeps nothing and only ORs the data words, and goes on only if all are
  zeros or the first holds a picture's magic, left by a first save cut short.
  That costs a few seconds, once per tape; a Sketchpad tape is trusted as its directory says.

A load reads the first block for the header, then the whole picture forward once to check every
block's checksum and both headers, and changes nothing unless all is well.
Then it searches back to the stream's last block and reads the stream in reverse straight into
the tables, a block read backward giving its words last first, so the tape does not run back to
the start of the picture, and then each entry forward into its master's record, the record
written back to the drum before the next is read.
A loaded master goes back into the store at the place it was saved from, if it was there.
If a later pass fails, which a tape that has not changed between the passes cannot do, the
tables are emptied.
Then the display is rewritten.

The Microtape gives or wants a word every 200 us, and the program moves every word itself, so a
transfer runs with the break system off.
Each run of words is a pointer, a count and a step, and the next run is taken inside the word
loop, between two words.
Meanwhile the display runs a small list that loops on itself, showing WAIT and needing no CPU.

The format is 19.
A load also accepts format 18, written before a master kept the count of its own texts, its
masters' texts then all counted as their own, and format 17, written before a master could hold
instances, its masters then holding none; any other format reads as EMPTY rather than loading
wrongly.
An instance's scale word keeps its low two bits for the Type 340's scale and the rest for a
multiplier, zero at a power of two, and needed no new format.
A load keeps the word, each variant made again from its master's record as the list is rebuilt,
and *sketchplot* draws it at its size, its lines by the factor, within a unit of the variant's
rounded places.

*sketchplot* reads a picture of any of the three formats from a tape image and draws it as a PNG, the
reading half of the thesis's Plot.

### The library tape

The thesis's library tape holds drawings to build others with (chapter IX's wedge, dimension
line, bridge bay, raster and alphabet), read by Library and written by Library write.
Here a library tape holds masters only, each an entry, so a tape holds up to 256 where a picture
tape holds twelve pictures.
- An entry is one master, starting at a block: a header (a magic word, the format, its size, a
  generation word, its name), the master's directory words, attachers, constraint count and
  texts with their digits, then its record's tables as a picture's entry has them.
  The cycle's settings are the picture's and are not kept.
- A master that holds instances is a nested entry, format 2: a header of its own (the magic,
  the format, its size, the generation, the name, the count of parts and each part's master as
  saved), then a part for the master and for every master it calls at any depth, through its
  `mUses`, the master first and the others in their order among the masters.
  Each part is laid out as a flat entry, followed by the master's nesting words and its
  sub-instances, each naming its master as saved.
  The masters it calls are carried whole, not flattened into it, so the hierarchy survives.
- An entry is at most 32 blocks, the library's buffer: banks 14 and 15 from 14's start, the
  truss's direct solve's scratch between its slices.
  A save or a load sets the truss's analysis to start again from its map before it writes there,
  so with Forces on the readouts go and come back within the solve's bound, about 10 s; Dir and
  Delete leave the banks alone.
  A larger entry is refused, TOO BIG, before anything is written.
- A master's record can be made resident only from outside an overlay, so a nested save or load
  is a round trip per part: the overlay returns with the next part's master named, bank 11's
  stub makes its record resident and calls again, the state across the calls kept in the
  buffer's header, which is written as zeros to the tape.
  Every refusal comes in the first call, before any record but the first part's is touched.
- The directory is at block 283, where a picture tape keeps its own, so that one block tells the
  two kinds apart, and a library command on a picture tape, or a picture command on a library
  tape, is refused before it writes.
  It is a header block (each page's count of entries and a map of the tape's blocks, a bit a
  block) and sixteen pages after it, each naming sixteen entries by a slot word (its first block,
  its length, up to 32 blocks, and a bit marking a nested entry, which Dir shows as a `+` before
  the name) and a name.
  A blank tape, its directory's block all zeros, is an empty library, written at its first save.
  A directory block neither the library's nor all zeros, any word in it, is another program's
  tape, refused, NOT BLANK, by every library command, Dir and Load too, since its pages would
  read as entries.
  The blocks a library save writes are not read first, as a picture tape's first save reads
  them: such a scan took about 35 s of a blank tape's first save, so a tape whose directory block was never
  written is taken as blank, and the user guide warns against using another program's tape.
  The library's first save writes from just past the directory, so a Z-machine save tape, whose
  saves start at the tape's beginning, is reached only at the wrap.
- A save takes the first free run long enough, looking from just past the directory to the
  tape's end and then from block 1, so its seeks to the directory stay short.
  It writes the entry, then the header with its blocks taken, then the page with its slot, so a
  save cut short leaves only blocks taken that no entry names, never an entry whose blocks are
  free; a delete writes the page, then the header.
  Save's keyboard starts from the master's name, or from the name last saved for a master with
  none, and a save that succeeds gives the master the name it was saved under, so the master and
  its entry agree.
- A load reads the entry whole, checks every block's checksum and judges the header (its counts
  within the tables, its size, room for its texts) before anything changes; then it replaces the
  master's texts, puts its words back, writes its tables into the master's record and gives the
  master the entry's name.
  A nested entry's every part is judged so, its sub-instances' masters among the entry's parts
  and the texts' room over all of them; its first part goes to the master chosen, each other to
  an empty master in order, never one matched by name, since two masters of one name may differ,
  or the load is refused, SLOTS, before anything changes.
  Each part's sub-instances are mapped to the masters the parts took, and its `mUses` made
  again from them.
  It may replace only a master the drawing does not call, directly or through a master holding
  it at any depth, so the canvas never changes; Undo does not cover it.

Misc's Save, Load and Dir and the library's four rows are one driver in the main bank, which
asks the tape module's root for each step (open a screen or the keyboard, run a transfer, or
stop) and runs each transfer through the same call, WAIT showing and the breaks off.

## Timing

Measured on the emulator.

| What | Time |
|---|---|
| A frame, empty drawing | About 1.4 ms, over 700 frames a second |
| A frame, 64 short lines | About 15 ms |
| A frame, 192 short lines | About 44 ms |
| A frame, 192 lines across the screen | About 0.3 s |
| A frame, 32 instances of a 20 ms master | About 0.63 s, 1.6 frames a second |
| A held pen's cost | About 12% of the frames |
| A change at 192 lines across the screen | 3 to 6 ms |
| A whole rebuild | 0.35 s at 64 short lines to about 1 s at 192 across the screen |
| A character drawn | About 27 us |
| The hexagon in a circle settling | About 2 s |
| The sample bridge's forces | About 0.5 s |
| A truss that can fold found UNSTABLE | About 2 s |
| A load or save of P6 or P7, beside the directory | 4 to 5 s |
| A load of P1 or P12, from the directory | 16 to 19 s |
| A save of P1 or P12, from the directory | About 30 s |

## Divergences from the thesis

| The thesis | Here | Why |
|---|---|---|
| Push buttons, toggle switches and knobs | A menu bar worked by the pen | The PDP-1 here has no button box |
| A pen flick ends an operation | A lift ends it | A mouse lifts, and a flick was hard to use and hard to tell from a fast stroke |
| Pointing by the pen's hits on tagged spots | Pointing by search from the target's center, the drawing not pen-sensitive | A hit pauses the 340, and a search gives the nearest object directly |
| Tracking about 100 times a second, no prediction | About 200 presentations a second with the emulator's predictor | A frame grows with the drawing, and the gap between presentations is what loses the pen |
| A ring structure with generic blocks | Dense tables, the last entry moved into a hole | Far less code, and a picture saved as a few runs of words |
| Circles by a difference equation | Each vertex rotated directly from the start | No error builds up, and every circle closes |
| Instances rotate and scale continuously | Instances translate and scale from x1/4 to x8, 32 sizes to a doubling | The 340 scales only by powers of two, so the sizes between are copies of the display list, and it cannot rotate a subroutine |
| A scale knob, exponential | Edit > Scale, a slider whose doublings are equal steps, for one part, a figure or the whole drawing | The PDP-1 here has no knobs; the slider keeps the thesis's exponential feel |
| A definition chosen by its number in toggle registers | A master picked on a screen of sixteen, each shown with its name | The PDP-1 here has no toggle registers to spare, and a name says what a slot holds |
| Instances nest to any depth, expanded at display time | A master holds up to eight instances, its words composed from theirs; changing an inner master makes its holders again or is refused whole, at Define or a library load; Dismember and Copy place them as instances again | The 340 keeps one return address |
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
| Constraints on the size of any thing | On instances only, each at its overall size, the inch sizes any value from 0.001 to 10 | A text or digits steps through four sizes, so 1/3, 3 and most inch sizes could never be met |

## Not implemented

Every constraint type of the thesis's appendix A is built but I and E, and every push button of
its appendix B is built or replaced by a menu row but the four below.

Ruled out, since the Type 340 cannot rotate a subroutine or a character.
- Rotated and mirrored instances, rotated text and digits, and the thesis's I and E constraints

Ruled out, as covered another way or not needed here.
- A larger page, the full zoom: a larger page would change every table, the solver's units and
  the tape
- The Constraint button, a constraint of any type on dummy points, left moving and merged onto
  points: each type is a row applied to what is picked, and a master's constraints, which Copy
  adds on the copy's points, define new ones, as the bridge beam is defined
- Members inside instances: an instance is rigid, so a member in one could carry no force; the
  thesis's bridge is built of copies, as it can be here
- Order and Disorder: Plot orders the lines itself, each from its end nearer the pen, and Define
  chains a master's lines the same way
- Punch, the plotter tape: the tape was for a plotter offline, and Plot draws on the plotter
  directly

Not built.
- Whole canvases held in memory for a fast load
- Numbers inside a held master
- Saving as text

## The source files

| File | Bank | Holds |
|---|---|---|
| *sketchpad.am1* | | The variables, the tables, the bank directives, the includes |
| *sketchpad.ah* | | Every constant but the intensities, and `BOXAT`, the one macro two files use |
| *intensities.ah* | | The intensity of each kind of thing drawn, the tracking target's among them |
| *spbreak.ac* | 0 | The break vector, `boot`, `penISR` |
| *spframe.ac* | 0 | The frame, the menus, the slots, the marks and glyphs, the masters' first region, WAIT |
| *spdispatch.ac* | 3 | The mode and menu dispatch tables, the lists' descriptors |
| *splibdir.ac* | 5 | The library's screen, and its directory's header and page as read |
| *spmain.ac* | 4 | Init, the main loop, the clock, the frame rate, the solver's pass |
| *spmenus.ac* | 4 | The menu, lists, sublists, the OK box, the tape's rows |
| *spundo.ac* | 4 | Undo |
| *spmodes.ac* | 4 | Each mode's handlers, the gates |
| *sppen.ac* | 4 | The pseudo pen location, the band, a stroke's commit |
| *spdisplay.ac* | 4 | The target calls' spread, the line list's rebuild |
| *sparcs.ac* | 2 | The arc module |
| *spedit.ac* | 2 | Removal, deletion, merging |
| *spdrag.ac* | 2 | A drag's arcs |
| *spdefine.ac* | 2 | Copy's arcs and merge, the gates through which Define's words reach the arcs |
| *sparcdata.ac* | 2 | The arc tables, the sine table, Define's working words and the held instances' blocks |
| *spscale.ac* | 6 | The scale module: Scale's pick, slider, steps, drop and Undo, and the constraints on size's rows and resolver; the stubs choosing it or the group scale |
| *spgscale.ac* | 6 | The group scale, Scale > Figure and All: the selection, the box, the apply, the refusals and Undo's record, a persistent drum overlay |
| *spkbd.ac* | 6 | The pen keyboard, and the directory's screen's touches: its entries, a drum overlay's stubs, and the overlay |
| *spinst.ac* | 7, 6 | The instance module: its stubs, and Define and Dismember's overlay |
| *spdefword.ac* | 6 | Define's words, the held instances' composed among them, in Define and Dismember's overlay |
| *spvar.ac* | 9 | The variants: an instance's words at a size between the powers of two, their lookup, sweep and root path |
| *spvar2.ac* | 2 | Two sizes composed, and the variants' root calls with no room in bank 9 |
| *spsub.ac* | 12 | The words a dismember's or a copy's held instances will call brought into the master store |
| *sprows.ac* | 9 | `rowCan`: whether a row can act, by item |
| *spcons.ac* | 8 | The constraint module and solver |
| *sptext.ac* | 10 | Texts and numbers |
| *sptape.ac* | 11, 6 | Saving and loading pictures, the tape directory and its screen, the masters' picker, the tape's mount, seek, read and write, the tape rows' steps: its kept words and stubs, and the overlay |
| *splib.ac* | 11, 6 | The library of masters: its kept words, stub and rows' steps, and the overlay |
| *spplot.ac* | 11, 6 | Plot: its first step and stub, and the overlay, the page on the plotter with its characters' strokes |
| *spinit.ac* | 11 | Start-up's display gates, the overlays' failure hook |
| *spcensus.ac* | 11 | `rowCensus`: what the menu's rows act on, counted as a list opens |
| *sptruss.ac* | 11, 6 | The truss analysis and readouts: its stubs, and the overlay |
| *spsize.ac* | 11 | The constraints on size: the root runner that brings each master record the resolver wants |
| *spfloat.ac* | 6 | The floating point, the end of the truss's overlay |
| *splock.ac* | 11 | The dot, the lock mark, the lit line |
| *spaux.ac* | 12 | Routines kept out of banks 0 and 4 for room, start-up's first part |
| *spview.ac* | 12 | The view, clipping, the constraints' arms |
| *spmarks.ac* | 13 | The constraints' marks |
| *spsearch.ac* | 13 | The line search |
| *spvarc.ac* | 13 | Arcs through the view |
| *Render/sketchplot.c*, *Render/Makefile* | | The picture renderer |
