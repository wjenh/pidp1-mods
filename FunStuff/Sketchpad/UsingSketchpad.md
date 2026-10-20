# Using **Sketchpad**

This document describes Sketchpad, a reproduction of Sutherland's Sketchpad for the PiDP-1 and its Type 340
display, and how to use it.

This is version 1.0 and covers the program's first release.\
Edit date 18-Oct-2026\
Initial version

## What is Sketchpad?

In 1963 Ivan Sutherland wrote Sketchpad on MIT Lincoln Laboratory's TX-2 for his doctoral thesis,
*Sketchpad, a Man-Machine Graphical Communication System*.\
It was the first program to implement several historically greatly significant items:
- The first of what became modern computer-aided-design programs, CAD
- The beginnings of object-oriented programming
- The first truly interactive human-conputer interface

It allowed creating complex drawings with numerous constraints, such as fixed points, equality between
lines, orientation, connectivity and more, as well as a force modeling system.

A drawing could be saved as a master and placed many times as instances with their defined constraints
and used to build up complex designs.

This Sketchpad is a new program in the spirit of Sutherland's, written for the PDP-1 with the
Type 340 display.
It does what the thesis describes, as far as a PDP-1 and a Type 340 can, and the design notes in
*SketchpadTechnicalDesign.md* say where and why it differs.
It is a *massive* program for a PDP-1, filling completely all 16 memory banks and is in excess of 40K lines of
assembler.
Additiionally, there is a 1300 line user guide and the technical document is almost 1000 lines.

Sutherland controlled the original with a lightpen together with a large push button box, toggle switches,
and rotary encoders exposed as knobs.

The pidp-1 has no large button box, no encoders, and not nearly as many switches.
Instead, everything is done with the pen, on the screen, using drop-down menus.

Wait, how is that historically accurate? There were no drop-down menus then.
But, there certainly could have been if someone had thought of them, this program uses only
devices that were actually available and would run on a real PDP-1 with sufficent memory and a Type 340 display.

What it can do, in brief:

- Draw lines and circle arcs, joined at their points, and place lines of text
- Move, delete and resize what is drawn, merging points dropped on each other
- Keep constraints, horizontal or vertical lines, equal lengths, parallel and perpendicular lines,
  fixed lengths, midpoints and fixed points, enforcing the constraints as the drawing is changed
- Define masters and place instances of them which follow any change to the master
- Place copies of masters, which become ordinary drawings
- Join instances to drawings and to each other by attachers, as Sutherland's supported
- Animate an instance by cycling it through the masters, as Sutherland's famous winking girl did
- Work out the forces in a truss, as Sutherland's also famous truss bridge did
- Place numbers and dimension lines that read or set distances
- Magnify the drawing up to eight times
- Save sixteen pictures per tape on the Microtape drive and load them back
- Undo the last item added
- Scale features and instances

There are only a few restrictions, one being object rotation as the original did it, the Type 340 doesn't
support native rotation.

There are also some major improvements in the internal structures.
While the external behavior tries to closely follow the original functionality described in the thesis,
the use of the Type 340 to offload the graphics processing along with some major changes in how data is represented
means the PDP-1 version is up to 60 times faster that the TX-2 version. That is not a typo.
The truss bridge example took about 30 seconds to recompute the forces after every change on the TX-2,
this version does it in under 3 seconds while still preserving the iterative convergence of the original.

## What you need

- The PiDP-1 emulator with its Type 340 display IOT and a display client that can support the high data rate
  the Type 340 can generate.
  The much improved lightpen tracking is also very desirable.
  *t30dpy* is the only good choice, see *UsingT30dpy.md* and *UsingTheLightpen.md*.
- All sixteen banks of memory, 65,536 words. It's a huge program.
- The BBN timesharing clock, IOT 32, for its 1 ms counter, see *UsingTimesharingClock.md*.
- The Type 550 Microtape, only to save and load pictures, see *UsingType550Microtape.md*.
- **am1** to assemble the program, see *UsingAM1.md*.

All of the peripherals are supported by the simh version of the PDP-1 and this program should be able to
run under it with few changes, the lightpen motion prediction done on the pidp-1 would not be available.

## The files

Every file is in *FunStuff/Sketchpad*, this guide, a technical design document.
The *Render* subdirectory contains a C program that replaces the plotter the TX-2 version had, allowing
the creation of a standard png file of what would have been plotted.

| File | What it is |
|---|---|
| *sketchpad.am1* | The program's main source file; it includes the others |
| *sp\*.ac* | The rest of the source, thirty-one files included by *sketchpad.am1* |
| *sketchpad.ah* | The program's constants, sizes, times and limits, included by *sketchpad.am1* |
| *intensities.ah* | The brightness of each kind of object drawn, included by *sketchpad.am1* |
| *SketchpadGirl.img* | A Microtape image holding a drawing of the famous girl, as picture P1 |
| *SketchpadGirlWink.img* | The true classic, the winkin girl, as P1 |
| *SketchpadBridge.img* | The truss bridge of the thesis's figure 9.6, as P1 |
| *SketchpadDimension.img* | A 3-4-5 triangle with dimension lines, the thesis's figure 9.5, as P1 |
| *Render/sketchplot.c*, *Render/Makefile* | *sketchplot*, the plotter replacement |
| *SketchpadTechnicalDesign.md* | How the program is implemented and how it compares with the thesis |

## Building and starting it

Assemble it in the directory that holds the source files.
```
cd /opt/pidp1-mods/FunStuff/Sketchpad
make
```
This creates *sketchpad.rim*, a read-in tape of all sixteen banks.
It is strongly suggested to use *pdp1central* or *fastload* to load it, otherwise be prepared
to wait a while, about 5 minutes, it is the equivalent of 16 very long tapes!

To load the tape with *fastload*:
```
fastload sketchpad.rim
```
The program starts at address 100.

Use the *pdp1central* load button when using it.

The screen is divided into two parts, the menu bar at the top and the drawing canvas below that.
When started, the screen displays the menu bars the top and the word INK in the middle of the canvas.
Touch INK or **Locate** to bring the tracking target to the middle of the canvas and start
having fun.

To make an item on the screen brighter or dimmer, change its level in *intensities.ah*, 0 being the
dimmest to 7 the brightest, and assemble again.

## The lightpen

The lightpen is the mouse.
Holding the left button down is holding the pen to the screen, and releasing it lifts the pen.
Remember, it's a lightpen, it only detects dots that are showing, blank areas do nothing at all.

Lifting the pen or moving it off the target faster than the target can follow look the same, a *lift*.
A lift is what completes most actions, so a lift is part of every gesture.

## The target and the dot

The *target* is a cross on the canvas.
Put the pen down on it and move, and the target follows the pen.
This is *tracking*.
Lift the pen and the target stays where it was left.
Move smoothly.
A pen that stops dead or reverses sharply at fast speed can leave the target behind, which counts as
a lift.
However, the target can be tracked at considerable speed, this should not usually be a problem.

At the target's center is a small dot, which Sutherland called the *pseudo pen location*.
The dot is where things happen, where a line starts and ends, what is picked up, what is deleted.
Near a point the dot jumps onto that point, it *locks*.
Otherwise the pen would have to be manipulated with more precision than is useful.

In the modes that can take a line, an arc, an instance or a text, the dot locks onto those too,
whichever is nearest.
If there are multiple potential locks, there is a selection priority.

A point is preferred over a line that is nearly as close.\
An instance or a text is identified by its middle, so aim at the middle to select it.

In Line, Circle and Move, the dot also locks onto them and onto the crossing of two lines.

Moving with nothing being dragged locks onto lines but not arcs.
In these modes it finds lines and arcs once the target pauses, however briefly, and always at the
moment the pen lifts.
It does the same while Copy is carrying a copy by its attacher.

What a lift will do:
- A bright cross around the dot marks the selection point, the same in a Line, Circle or Move's drag.
  It also marks a line, an arc, or a crossing.
- A line or an arc that a lift selects is also drawn brighter.
  This is true in Delete, in Move with nothing dragged, in Horv, and in the constraint rows that take
  lines.
- An instance or a text the dot is locked onto is also drawn brighter.

When the program starts the target is hidden and **INK** shows in the middle of the canvas.
Touch INK with the pen or select **Locate** to bring the target to the middle of the canvas.
This is Sutherland's "inking up, the target appears where INK was.
Choosing a mode that uses the target also brings it to the middle if it is not already displayed.
INK is shown only until the target first appears.
From then on the target stays, hidden only while a list or the pen keyboard is open.

Again, remember, unless the pen is down and on some visible point that the program treats as an active point,
the pen does nothing.

## The menu bar

The bar reads, left to right, **Draw**, **Edit**, **Picture**, **Misc**, **Locate**, **Undo**,
**Stop** and **View**.
The bar always responds to the pen being put on an item whatever the program is doing.

**Locate**, **Undo** and **Stop** act the moment the pen touches them.

**Draw**, **Edit**, **Picture**, **Misc** and **View** open a list below them.
While a list is open the drawing is hidden and the target does not track.
- Touch an item to select it.
  An item ending in `>` opens a further list beside it.
- A small **OK** box appears beside the active row.
  Touch it to confirm the action, the the list closes.
- OK responds only after the pen has been lifted and put back down.
  A pen slid onto it from a row, or brought up from the drawing still held down does nothing until it
  is lifted for a quarter second and put back on OK.
- To close a list without doing anything, touch its bar item again, touch another bar item,
  or leave the menu alone for a few seconds.

A row with nothing to act on is drawn dim and the pen won't select it.
A row ending in `>` whose rows are all dim is dim itself, and opens nothing.
Most selections are modal, that is, they stay in effect until another selection or a *Stop* is done,
or the selection is no longer applicable.

| Row | Dim until there is |
|---|---|
| Truss > Beam | a line that is neither a member nor a load |
| Truss > Load | a member, and a line that is neither a member nor a load |
| Truss > Roller | a point |
| Truss > Axis | a roller and a line |
| Truss > Stiff | a member |
| Dim > Dist, Change | two points and a number |
| Dim > Gap | a point and a number |
| Edit > Move, Delete, Scale | a point, a text, or an instance |
| Constr > Horv, Length | a line |
| Constr > Equal, Paral, Perp | two lines |
| Constr > Mid | a line and a third point |
| Constr > Fix | a point |
| Edit > Size > | an instance or a text |
| Edit > Change > | a master defined and an instance placed |
| Edit > Dismem | an instance |
| Picture > Define > | a line, an arc, or an instance |
| Picture > Inst >, Copy > | a master defined |
| Picture > Tie, Desig | a point |
| Misc > Del cons | a constraint other than a truss's member, load, or roller |
| Misc > Unfix | a fix, an attacher, or a designation |
| View > Pan, Center | a view larger than x1 |

Change's Cycle, Dwell and Seq, and Undo have rules of their own, see their sections.
Define's row of an empty slot is drawn dim but still acts, dim there means empty.
Clear always acts, even on an empty canvas.

The entire menu with each row's section in this document.

| Bar item | Rows |
|---|---|
| Draw | Line, Circle, Truss > (Beam, Load, Roller, Axis, Forces, Stiff), Text, Dim > (Number, Dist, Change, Gap) |
| Edit | Move, Delete, Constr > (Horv, Equal, Paral, Perp, Length, Mid, Fix, Show), Size > (x1, x2, x4, x8), Scale, Change > (M1 to M4, Cycle, Dwell, Seq), Dismem |
| Picture | Define > (M1 to M4), Inst > (M1 to M4), Copy > (M1 to M4), Clear, Tie, Desig |
| Misc | Save, Load, Drive n > (D1 to D8), Dir, Del cons, Unfix |
| Locate | Acts at once |
| Undo | Acts at once |
| Stop | Acts at once |
| View | x1, x2, x4, x8, Pan, Center |

## Modes

The program is always in a *mode*.
**Locate** is the default mode.
In it the target follows the pen across any number of lifts and nothing is drawn or changed.
The other modes are chosen from the lists.

| List | Row | Mode |
|---|---|---|
| Draw | Line | Draws connected lines |
| Draw | Circle | Draws circles and arcs |
| Draw | Truss > Beam | Makes a line a member of a truss |
| Draw | Truss > Load | Makes a line a load on a truss |
| Draw | Truss > Roller | Lets a point of a truss roll, level at first, or holds it again |
| Draw | Truss > Axis | Sets the direction a roller rolls in |
| Draw | Truss > Stiff | Sets how stiff a member of a truss is |
| Draw | Text | Places a line of text, or edits one |
| Draw | Dim > Number | Places a number, or types its value |
| Draw | Dim > Dist | Makes a number equal the distance between two points |
| Draw | Dim > Change | Makes a number equal the change in a distance |
| Draw | Dim > Gap | Keeps a point beside a number |
| Edit | Move | Moves points, lines, instances and texts |
| Edit | Delete | Deletes points, lines, arcs, instances and texts |
| Edit | Constr > Horv | Makes a line horizontal or vertical, and keeps it so |
| Edit | Constr > Equal | Keeps two lines the same length |
| Edit | Constr > Paral | Keeps two lines parallel |
| Edit | Constr > Perp | Keeps two lines at right angles |
| Edit | Constr > Length | Keeps a line the length it has |
| Edit | Constr > Mid | Keeps a point at the middle of a line |
| Edit | Constr > Fix | Fixes a point in place, or frees it |
| Edit | Size > x1, x2, x4, x8 | Sets an instance's or a text's size |
| Edit | Scale | Resizes a line, a circle or an arc, a text, a number or an instance along a slider |
| Edit | Change > M1 to M4 | Gives an instance another master |
| Edit | Change > Cycle | Starts or stops an instance cycling through the masters |
| Edit | Change > Dwell | Sets how long a cycle shows each master |
| Edit | Change > Seq | Sets the order a cycle shows the masters in |
| Edit | Dismem | Takes an instance apart into ordinary points, lines and arcs |
| Picture | Inst > M1 to M4 | Places one instance of a master |
| Picture | Copy > M1 to M4 | Places one copy of a master |
| Picture | Tie | Makes a point an attacher, or an ordinary point again |
| Picture | Desig | Designates the points a copy merges onto, in order |
| View | Pan | Drags the drawing as far as the target moves |
| View | Center | Brings the point the dot is on to the middle of the canvas |

The current mode's bar item stays lit.
A mode lasts until **Stop** is pressed or another mode is chosen.
**Inst** and **Copy** are active for only a single placement.
Constr's **Show**, Truss's **Forces** and View's x1 to x8 are not modes.
Instead, they act within the menu leaving the mode as it was.

### Aiming and gestures

Within a mode the program is either *aiming* or *in the middle of a gesture*.
While aiming, the target moves freely and the dot shows by that mode's rules what a lift would
select.
Lifting the pen takes the mode's first step if the dot is on something the mode can operate on
and does nothing if it cannot, so a lift in empty space is always safe.

There are two exceptions.
In Text a lift anywhere opens the keyboard.
In Number a lift anywhere places a number which Undo can cancel.

**Choosing a mode**

Line, Circle, and Move take effect at once, at the dot.
Line starts its first line there, Circle puts its center there.
Move picks up what the dot is on, if anything.
To start somewhere else, press Locate, move the target, and lift.

Every other mode starts out aiming and acts at the next lift.
Taping on the target without moving it acts on what the dot is on.

If the target is not tracking when a mode is chosen, it moves to the middle of the canvas and the
mode starts out aiming.

**Locate** inside a mode ends any gesture under way and goes back to aiming, the mode stays active.
A line in progress is abandoned, a circle's center or start is canceled, a constraint's first line
or point is canceled, and anything Move is carrying is put down where it is.

Pressed in Locate mode itself it brings the target to the middle of the canvas.

**Stop** does what Locate does and then leaves the current mode for Locate mode.
Choosing another mode also stops the current one.

Dragging the target straight to the menu without lifting first does not count as a lift.
It never deletes, fixes, or sizes on the way.

## Drawing

### Line

Each lift adds a line from the last vertex to the dot, and the next line starts there, lines
are drawn as a chain.
A *rubber band* shows the line in progress.
Lines start and end on existing points when the dot locks onto them, joining parts of the drawing.
A very short stroke adds nothing, and a stroke along a line already drawn between the same two
points adds no second line.
A vertex placed with the dot on a line or an arc remains on it, see
*Points on lines and circles*, below.
Press Locate to end the chain and start a new one.

### Circle

A circle or an arc takes three lifts, the center, then the start of the arc, which sets the
radius, then its end.
The arc runs the way the pen first moves away from the start and is drawn as the pen is swept.
The radius shows as a dotted line.
Ending on the start after going at least halfway around makes a full circle.
The mode then waits for the next center.

A center, start or end placed with the dot on a line or an arc is kept on it.
An arc's end joins where the two cross.

Each arc's center, start, and end carry small dim crosses in Line, Circle, Move and Delete modes
so they can be aimed at.

### Text

Lift where a text should start.
The pen keyboard opens with the prompt `Text`, see *The pen keyboard*, below.
OK puts the text typed at that location, its first character's lower left at the dot at size x2, 14 units
tall, the size of the menu's labels.

Lift with the dot on a text already placed to edit it.
The keyboard opens with its text, and OK puts the new text in its place, at its current size.
A text emptied and OK'd is deleted.
Cancel leaves the text unchanged.
The mode stays in text mode.

A text holds up to 24 characters, upper and lower case.
In Text mode the dot locks onto texts only, never a number's digits.
A text has no points so lines do not start or end on it and no constraint applies to it.

A text that would run off the screen is refused at OK, OFF EDGE showing under the bar at its
right for a few seconds.
More characters can be displayed (see *Limits*) is refused with NO ROOM.
With the drawing's texts all used a lift opens no keyboard and shows NO ROOM.

## Editing

### Move

Lift with the dot on a point to pick it up, then drag and lift to put it down.
Its lines follow it.
Put down on another point, it merges into that point.
Put down on a line or an arc, it is kept on it.
Lines and arcs on the point being dragged are never locked onto since they move with it.
Dragging an arc's center carries the arc, dragging its start or end changes its radius.

A line is picked up by aiming at it away from its ends.
Near an end, the dot locks onto the end's point instead.
Both ends move together.
The line keeps its length and direction and the place it was taken to stays under the dot.
Any lines connected to its ends follow as they do for a point.
At an edge of the canvas, the line stops on that axis but still slides along the other.

An arc centered on one of the line's ends or running from one end to the other moves with the line.
An arc starting or ending on one end takes its radius from it, as when that point is dragged, and
a drag an arc cannot follow is refused, the line stopping at the last valid location.

Put down with the dot on another line, the dragged line merges into that line, each end into the
nearer end.
The line dropped on stays where it is and the dragged line is gone.
Every line, arc and constraint on the dragged line's ends goes to the ends it merged into.
Put down anywhere else, a point included, the line stays where it was put and nothing merges.
It also doesn't merge when an arc runs from one of its ends to the other or where an arc on its ends
could not follow the merge.

An instance is moved by its origin, carrying its tie points, what is drawn on them, and
every instance fastened to it.
A tie point dragged brings its instance along, see *Attachers and tie points* below.

A text is moved whole, the place it was taken staying under the dot.
At an edge of the screen it stops.
A number's digits move the same way and the constraints on them follow.

Move stays in force until another mode is selected or a Stop is done.

### Delete

Lift with the dot on something to delete it.
- A deleted point takes every line, arc, and every constraint on it.
  A point left with nothing on it is automatically deleted.
- A deleted line or arc also deletes the constraints that kept points on it and a line also deletes its Beam
  and Load.
- An instance is completely deleted.
  Each of its tie points stays while a line, an arc, or another instance is on it and is deleted otherwise.
- A tie point deletes its lines and its instance stays.
- A text is deleted completely.
- A number's digits deleted delete the number with them and every Dist, Change and Gap assigned it.
- With Show on, a lift with the dot on a constraint's mark, within its circle, removes that
  constraint alone and nothing else.
  The mark wins over anything else in reach.

**Misc > Del cons** removes every constraint at once, see *Misc*, below.

### Size

Edit > Size > x1, x2, x4 or x8.
Lift with the dot on an instance or a text to set it to the chosen size.
Size works on instances and texts only.
Ordinary lines and arcs are not resized here; Scale resizes them, see below.
A size that would take the instance, its texts included, or the text off the screen is refused.

A text grows from its first character's lower left, which remains fixed.
A number's digits grow about its middle and a Gap on them widens with them.
An instance's tie points move to the new size, the lines on them following.
A circle on a tie point follows also and if it cannot the size is refused.

### Scale

Edit > Scale.
Lift with the dot on a line, a circle or an arc, a text, a number, or an instance to select it.
The dot is bright on something Scale applies to, dim otherwise.
A slider appears just below the dot, or above it near the bottom of the canvas.
It is a horizontal zigzag whose corners mark doublings and beside the dot is the factor in digits, `x1.00`.
Put the pen down and move it along the slider.
To the right the part grows, to the left it shrinks, twice or half as large at each corner.
The pen's up and down motion is ignored.
Lift to select that size.
Scale stays in force until another mode is selected or a Stop is done.

- A line is scaled about its middle or about its fixed end, which stays fixed.
  Its unfixed ends move and the lines and circles on them follow.
  A Length on it takes the new length.
- A circle or an arc is scaled about its center.
- A text, a number or an instance steps through the same sizes Size sets, x1, x2, x4 and x8, the
  slider snapping to the nearest and grows as it does under Size.
- A step is not made if it would take the part past the target's reach, make a line shorter
  than 4 units, or take a circle's radius below 15 or past 1294.
  The item and the knob stay at the last factor that was valid.

The slider spans five doublings each way, x1/32 to x32, as far as the target can follow, and
for a text, a number, or an instance only the sizes x1 to x8.

These are refused, nothing picked up and the dot staying dim:
- a line with both ends fixed
- a line with a circle or an arc on both its ends, a radius or a chord
- a circle or an arc whose start or end is fixed

Locate selects the current resizing and stays in Scale.
Stop puts it down and return to Locate mode.
Choosing another menu item resizes it first.
Undo reverts a scale.

### Dismem

Edit > Dismem.
Lift with the dot on an instance to take it apart, as the thesis's Dismember does.
It becomes its master's points, lines, arcs and texts at its location and its size with the
constraints the master was defined with just as a copy becomes when it is placed.
The instance is gone.

Each of its tie points becomes the point of the master's attacher there so whatever was drawn on
a tie point and any instance fastened at it stays joined to the parts.
A cycling instance is taken apart as the master it currently shows.
Dismem stays in force for the next instance.

It is refused, nothing changing, if the drawing or the constraint table has no room for the
parts, its texts included.
It is also refused if a part would land where the target cannot reach, under the menu or at the
screen's edge, or if an arc, if the instance's size would be too large to draw.

## Constraints

Edit > Constr opens Horv, Equal, Paral, Perp, Length, Mid, Fix and Show.

A constraint is kept until it is deleted or the object it constrains is deleted.
When a point is moved, the program moves the other points it constrains.

While a point is dragged nothing else moves.
At the drop the figure's constrants are applied,  redrawn as it converges on the constraint solution.
A constraint that cannot be satisfied or takes too long to solve stops the constraint solver and leaves
the object in the state at that point.
A fixed point is never moved to keep a constraint.

Each constraint mode locks the dot onto lines alone so that at a corner the nearer line is selected.
Mid's first lift and Fix lock onto points.
Each mode stays in force.

A constraint already held or one that makes no sense, such as Mid on one of its line's ends, is refused,
as is one when the constraint table is full and othing changes.

**Horv.**
Lift with the dot on a line.
It becomes horizontal or vertical, whichever it was closer to, and stays that way.

**Equal, Paral, Perp.**
Two lifts, on two lines.
The first line picked is drawn bright until the second lift.
A second lift on the same line does nothing but the pick is kept.
The second lift adds the constraint.
Equal brings both lines to their mean length, each about its middle.
Paral turns both toward the direction between them, and Perp toward a right angle, each about
its middle, or about its fixed end if it has one.

**Length.**
Lift with the dot on a line.
It then keeps the length it has.

**Mid.**
Lift with the dot on a point, then on a line.
A dotted band, like Circle's radius, runs from the point to the dot in between.
The point is kept at the line's middle, the point and the line's ends sharing moves.

**Fix.**
Lift with the dot on a point to fix it, then nothing the program adjusts to keep a constraint
can move it.
Lift on a fixed point again to free it.
Dragging a fixed point with Move, or a line with it at an end, frees it.
In Fix mode each fixed point carries a small circled F, with Show on or off.
**Misc > Unfix** frees every fixed point at once.

### Points on lines and circles

A point placed or put down with the dot on a line is kept on that line, extended past its ends if
necessary.
One put down on an arc is kept at that arc's radius about its center and one on two line
crossings is kept on both.
These constraints have no row.
Line, Circle and Move make them as part of the placement.
Undo of a Line or Circle placement deletes them with the point.
A Move's drop, which Undo does not take back, keeps them.
If the constraint tabl ise full, the point is placed without one.
A point already kept on that line or circle gets no new constraint.

### Show

Edit > Constr > Show, its OK touched, turns the constraint marks on,or off.
Its label is bright while they are on.
It acts within the menu, the mode and any gesture under way stay as they are.

While Show is on every constraint carries a small dim circled letter at the middle of its points,
in every mode, following them as they move.

| Mark | Constraint |
|---|---|
| H | Horv |
| E | Equal |
| P | Paral |
| R | Perp |
| L | Length |
| X | Mid |
| N | A point kept on a line |
| C | A point kept on a circle |
| F | A fixed point |
| A | An attacher |
| 1 to 6 | A designated point, its place in the order |
| B | A truss member |
| W | A truss load |
| O | A truss roller with two dim ticks along its axis or a number's Gap |
| D | A number's Dist |
| Y | A number's Change |

With Forces on, a member's B and a load's W change to the force symbols.

A letter whose place falls on a line, H, L, X, N, B and W, are placed beside that line instead, its
circle just clear of it, above a shallow angled line and right of a steep one.
A second letter on the same line is placed on the other side, a third beyond the first, a ring
further out.
A number's letters are placed at its digits' upper left rather than on the lines,
a second letter on the same digits further left.

An E, a P, or an R, shown at the middle of its two lines, has arms, a dim dotted run from the
first line's middle to the second's, through its circle, so that it shows which lines it is associated with.
The arms follow the lines as they move and are cut off at the screen's edge.
A mark too close to both middles to display shows nothing.

Show is off when the program starts and after a load and is not saved with a picture.

## Undo

**Undo** takes back the last thing added.
That can be a line, an arc, a text, a number, an instance with its tie points, or a copy, with its
texts, its merges, and its constraints, the drawing is put back to the way it was before the addition.
It can be a constraint with whatever the program moved to satisfy it, a fix, an attacher, a
designation, a member, a load, or a roller.
It can be an instance taken apart, its parts with their merges and constraints restored and the
instance put back as it was, cycling again if it cycled.
It can be the start of a chain, or a circle's center or start.
It can be a scale, the object put back as it was with the lines and arcs that followed it and
any Length's value.

Undo never leaves the mode it is pressed in.
In Line it removes the last line and the chain continues from the vertex before it.
In Circle it goes back one lift.
In Inst and Copy it removes what was being placed.

There is one level of Undo and it only takes back additions or a scale.
An Undo cannot be undone

after any of these Undo has no effect:
- A deletion, a move, a size, or a change of master
- A text edited, or a number's value typed
- A cycle's start or stop
- A point freed, an attacher made ordinary again, or a point undesignated
- A roller's axis set or a member's stiffness set
- Clear, Define, Del cons, Unfix and Load
- Setting a dwell or a sequence

A cycle's steps do not affect an udo, so a change made while cycling can be undone.

The Undo label is dim whenever there is nothing to undo.

## Masters, instances and copies

A *master* is a drawing kept for reuse, stored in one of four slots, M1 to M4.
An *instance* is a display of a master, placed anywhere and at any of four sizes.
Every instance of a master displays that master.
A *copy* is a master's drawing placed as ordinary points, lines, arcs and texts, which can then be
edited one by one.

### Define

Picture > Define > M1-4 makes the present drawing, its lines, arcs, texts,
attachers, and constraints that master and clears it from the canvas.
Defining into a slot already used replaces that master.

Every instance on the canvas also goes into the new master.
It is taken apart as Dismem would and its parts become part of the drawing.
Its tie points become the master's points and a designated one an attacher.
The master keeps them as its lines and arcs, not as an instance, so redefining the inner master
later leaves it unchanged.
A master may be redefined with instances of it on the canvas.

An attacher with no line or arc on it is not kept and a fix is not kept.

While nothing is drawn and no instance is placed, Define and its rows are dim.
Texts alone cannot be masters, there must be drawn objects.

Define is refused leaving the drawing and the instances as they were if:
- the drawing is too large for a master,
- it has more than 16 constraints among its points,
- all masters together would hold more than 32 texts,
- while any instance cycles,
- an instance cannot be taken apart because the tables lack room for its parts, its texts
  included, a part would lie off the screen, or an arc would be too large at its size.

Define takes effect as soon as its OK is touched, clears the canvas, and leaves the program in Locate mode.

### Inst

Picture > Inst > M1-4 places one instance of the selected master.
The new instance follows the tracking target.
A lift, or any menu action, puts it down and the program returns to Locate mode.
It is refused if the drawing has no room for its tie points.
It is refused as it is chosen, NO ROOM showing, if the master's texts would exceed the characters
the screen can show.

An instance's texts are its master's, shown at its origninal place and size.
A text's size and the instance's multiply, up to x8, so a text placed at x2 shows at x2 in an
instance at x1 and at x8 in one at x4 or x8.
The pen does not select an instance's texts, only the instance.
it.

### Copy

Picture > Copy > M1-4 places one copy of the selected master onto the canvas.
It is placed as Inst places an instance, but when placed it becomes ordinary points,
lines, arcs, and texts with the same constraints the master was defined with.

A copy of a master with attachers when being placed is dragged by the last attacher that was marked
and locks onto points, lines, and arcs as in Move's drag, though no bright cross is drawn.
At the drop attacher merges into the point the tracking dot is on or is attached to the line or arc it is
on.

Each other attacher merges into its designated point (see *Desig*, below), however far away.
The first attacher marked merges into the point designated first, the second into the second, etc.
One with no designated point merges into the nearest point of the drawing within about 20 units,
if there is one.
The targeted attacher always merges at the drop location, so with n attachers at most n - 1 designations are
used.
The drop clears every designation, used or not, and Undo of the copy does not bring them back.
A copy refused for room leaves them.

A line of the copy that lands on a line already drawn between the same two points becomes that line.
Where the edge of the screen stops the drag and selected attacher moves away from the dot, it does not merge.
A master with no attachers is dragged by its middle and nothing merges.

Copy is refused if the drawing or the constraint table has no room for all its parts.

### Clear

Picture > Clear erases the whole drawing, its texts, and every instance.
The masters and their texts are kept.
Clear takes effect as soon as its OK is touched and leaves the program in Locate mode.

### Tie

Picture > Tie.
Lift with the dot on a point to make it an attacher, or on an attacher to make it an ordinary point
again.
In Tie mode each attacher carries a small circled A with Show on or off.
There can be up to six, and a seventh is refused.
The order they in which they are made matters to Copy since the last one made is the one a copy is moved by.
**Misc > Unfix** makes every attacher an ordinary point immediately.

### Desig

Picture > Desig.
Lift with the dot on a point to designate it, next in order, or on a designated point to
undesignate it, the later ones moving up one.
In Desig mode each designated point carries a small circled digit, 1 to 6, its place in the order,
with Show on or off.
There can be up to six, and a seventh is refused.
A point may be an attacher and designated too.
Copy's next drop merges its attachers onto the designated points in order, and clears designators.

Designation lets a constraint be defined by drawing it, as in the thesis.
Draw two lines, put an Equal between them, make their four ends attachers, and define them as a
master.
Copy that master onto two lines with three of their ends designated and the fourth under the dot.
The copy's lines are deleted as repeats of the lines they land on and an Equal is left on the two lines.

Define keeps no designation in a master, and **Misc > Unfix** and **Misc > Del cons** clear them
all.

### Attachers and tie points

An attacher is a point by which a master's instances are joined to the rest of a drawing, as
Sutherland's were.
Each instance shows a small cross at each of its master's attachers and has a real point there, a
*tie point*, which the program keeps at that place on the instance.
Otherwise a tie point is an ordinary point.
Lines and circles start and end on it, and Move, Delete and the constraints lock to it.
- Moving an instance by its body carries its tie points and what is drawn on them with it.
- Dragging a tie point drags its instance with it, as if a handle.
  Dropped on another point, the tie point merges into it as any dropped point does.
  Dropped on a line or an arc, it is attached to it.
- A line dragged with a tie point at an end moves the tie point alone, the instance staying.
  At the drop the instance and whatever is fastened to it settles to the tie point.
- Two instances that share a tie point are *fastened*.
  Drop a tie point of one on a tie point of the other to fasten them.
  Fastened instances move together, rigidly, when either is dragged.
  Where something else holds a tie point, a fix, a constraint, or another fastening, they settle at
  the drop, as any constrained figure does.
- An instance taken apart by Dismem leaves its tie points where they are, each now a point of its
  parts.
  One it shared with a fastened instance stays that instance's tie point so the parts move with
  it as anything drawn on a tie point does.

### Change

Edit > Change > M1-4.
Lift with the dot on an instance to make it an instance of the selected master.
This is how Sutherland's girl winks,
one eye an instance whose master is swapped for a closed eye.
The instance keeps its size and its first tie point stays where it is.
The new master's first attacher lands on it.

Where the new master's other attachers lie as the old one's did, no tie point moves.
So, for the frames of an animation, mark the same features as attachers in each, an eye's corners
for instance, in the same order, before Define.
The frames can be drawn anywhere.
Where the other attachers lie elsewhere, the other tie points move to them taking the lines and
circles on them along and the constraints settle the figure as after a Size.
If a circle on a moved tie point cannot follow, the change is refused.
Two masters without attachers are lined up by their middles.

Change is refused, nothing changing:
- for an empty master,
- for a master with a different number of attachers, or attachers where the old one has none,
- where the new figure would leave the screen,
- or where the new master's texts would exceed the characters the screen can show (NO ROOM).

### Cycle

Edit > Change > Cycle.
Lift with the dot on an instance to start it cycling.
The row is dim until an instance is placed and at least two masters are defined.
At each step it changes to the next master of the sequence, in M1 to M4 order and back to M1 unless
Seq has set one specifically.
Each step lasts that master's dwell, a quarter second if Dwell has not set it.
Lift on a cycling instance to stop it where it currently is.

Every cycling instance takes the same step at the same moment.
A master that none of them can change to is passed over, and one that only some can take holds
the others where they are.
For the winking girl, both eyes blink together, and an eye started later falls in step at the next step.
A step never moves a tie point, a master whose attachers lie elsewhere is passed over.

A cycling instance carried by Move keeps changing, its tie points staying where they are.
Delete and Clear stop a cycle with its instance and a load stops every cycle.
Which instances cycle is not saved with a picture but the dwells and the sequence are.
Define is refused while any instance cycles.

Since a cycling instance may show any master next, its texts are counted as those of the master
with the most characters.
A start that would exceed the characters the screen can show is refused (NO ROOM).

### Dwell

Edit > Change > Dwell.
The number keyboard opens for each defined master in turn with the prompt `M1 dwell` etc.,
showing how long a cycle displays that master, in milliseconds.
Type a whole number from 50 to 9999 and OK to accept it, and the next master's keyboard opens.
Anything else is refused, that dwell left as it was and the next opens.
Cancel stops the propmpts, the dwells already set kept.
Every dwell is 250 when the program starts.
A picture saves its dwells and a load sets them.
The menu row is dim until a master is defined.

### Seq

Edit > Change > Seq.
The number keyboard opens with the prompt `Sequence`, one digit, 1 to 4, for each step, up to 24 steps.
`1232` shows M1, M2, M3, M2 and loops.
A sequence left empty is M1 to M4 in turn.
OK keeps it, and the next step takes its first entry.
A digit other than 1 to 4 refuses the whole sequence and the old one kept.
Cancel keeps the old one.
The row is dim until a master is defined.

## The truss

Draw > Truss opens Beam, Load, Roller, Axis, Forces and Stiff.
Its rows mark a truss, a bridge's frame of members, for the force in each member to be computed
and shown, as Sutherland's bridges show.
The truss's marks are kept like constraints but the program moves nothing to keep them.
Beam, Load, and Stiff lock the dot onto lines, Roller onto points, and Axis onto a point and then a
line.
Each stays in effect until a new selection is made.

**Beam.**
Lift with the dot on a line and it becomes a member of the beam being defined.
A line already a member or a load's line is refused.

**Load.**
Lift with the dot on a line and it becomes a load.
It pushes on the end that is a member's point toward its other end with a force of ten times its length.
Exactly one of its ends must be a member's point, otherwise it is refused.

**Roller.**
Lift with the dot on a point and it rolls, free to slide along its axis but held to it as a
bridge's end on rollers is.
A new roller's axis is level so it slides sideways and Axis turns it.
Lift on a roller again to fix it in place
A fixed point cannot be made a roller, nor a roller fixed, and either is refused.
In Roller and Axis modes each roller carries a small circled O, with Show on or off, and two short
dim ticks outside the circle along its axis drawn in the nearest of eight directions, every 22.5
degrees.

**Axis.**
Lift with the dot on a roller, then with the dot on a line.
The roller's axis follows the line so that it slides parallel to the line as a roller on a slope
or a wall does.
A dotted band runs from the roller to the dot between the two lifts.
The axis stays with the line.
Move or turn the line later, the ticks turn with it and with Forces on the forces are calculated again.
Delete the line and the roller stays, level again.
The roller need not lie on the line since only its direction counts.
A lift on a point that is not a roller does nothing.
To make a roller level again, give it a level line, or make it fixed and then make it a roller again in
Roller mode.

**Stiff.**
Lift with the dot on a member and the number keyboard opens with the prompt `Stiffness`.
Every member starts at 1.
Type a number from 0.1 to 99.9, with at most one digit after the point, and OK to keep it.
Anything else, or Cancel, leaves it as it was.
A lift on a line that is not a member does nothing.
While Stiff mode is on, each member shows its stiffness beside its middle where its force would
be, e.g., `2.5`, or `4` for 4.0 whether Forces is on or off and no load shows.
With Show on, a member's B changes to its stiffness,and a load's W stays.
When the mode ends, the forces return.

A stiffness counts only where the truss has more members than it needs to stand and there a
stiffer member takes more of the load.
In a truss with just enough, such as the bridge on a pin and a roller, the forces are the same
whatever the stiffnesses.

**Forces.**
Draw > Truss > Forces, its OK touched, turns the members' forces on, or off again.
Its label is bright while they are on.
It acts within the menu, as Show does.
Forces is off when the program starts and after a load and is not saved with a picture.

While Forces is on, each member shows its force beside its middle, in the load's units, positive
for a member pulled (in tension), with a minus sign for one pushed (in compression).
Each load shows its size, ten times its length, beside its middle.
A number shows just above a member that runs more across than up, and just to the right of one
that runs more up than across, clear of its line though a neighboring line may still cross it.

The forces are recalculated whenever a member, a load, a support, or a member's point changes,
and shown again.
They are hidden while anything moves, the drawing is settling during recalculation,
a point or an instance is being held, or while a menu list is open.

When the truss has no answer that can be calculated, every number shows as three dashes
and UNSTABLE appears under the menu, at its right.
A truss has no answer when it can fold, as a frame missing a diagonal can, when a joint's members
all lie on one line, or when the loads are too large.
While a tape message is being shown under the menu, it takes UNSTABLE's place.

The truss needs supports to stand.
A fixed point is a pin, holding its joint still, and a roller holds its joint across its axis.
A bridge stands on a pin at one end and a roller at the other.

## Numbers and dimension lines

Draw > Dim opens Number, Dist, Change, and Gap, the thesis's numbers and dimension lines.

A *number* is a value, shown by its *digits*, up to five digits and a sign, leading zeros suppressed.
Its unit is a thousandth of an inch at 100 screen units to the inch, the screen being 10.24
inches across, so a line 300 units long reads 3000.
A number is *free* until a value is typed for it, and then it is *set*.
Its digits are like a text, placed and moved by their middle.
The dot locks onto them by their middle as for text but never onto a point, so no line
starts or ends on them.
Move, Delete, Size and Undo work as they do for text.
Each Dim mode stays in effect for the next lift.

**Number.**
Lift with the dot away from any number and a new number is placed there, free, its digits
reading 0, at size x2.
Lift with the dot on a number's digits and the number keyboard opens with its value, with the
prompt `Number`.
Type an optional minus sign then one to five digits and OK; the number is set to that value.
Clear and OK mmakes it free it again.
Anything else, or Cancel, leaves it as it was.
In Number mode the dot locks onto numbers only.
With 32 numbers or the drawing's texts or points all used, a lift places nothing and shows NO
ROOM.

**Dist.**
Lift with the dot on a point, then on a second point, then on a number's digits.
The number and the distance between the two points are kept equal.
A free number on one Dist reads the distance and follows as the points move.
A set number moves the points to its distance, just as Length keeps a line's length constant.
A free number on several Dists brings them and itself to their mean, as Equal does.
A value that cannot be met, below 0 or past the screen's diagonal (14480), moves nothing.

**Change.**
Lift with the dot on a point, then on a second point, then on a number's digits.
The number reads how much the distance between the points has changed, the distance now less
the distance when Change was last made, with a minus sign when it is shorter.
It reads 0 at first, and never moves the points.
A number Change sets drives a Dist on it as a typed value does.

**Gap.**
Lift with the dot on a point, then on a number's digits.
The point is kept at a fixed distance from the digits' middle, room for five digits and a sign at
their size and a margin, so that the digits maintain  a gap.
Sizing the digits widens it.
Two Gaps, one each side of the digits, and a Paral leave the gap in a dimension line, as in the
thesis.
*SketchpadDimension.img* shows one.

Each point picked carries a small cross until the constraint is made or the mode is left by Stop,
Locate or another row.
The dot passes over a point already picked without it being selectable.
Dist's or Change's second point must be another point.

Each of the three is a constraint.
It is refused as the Constr modes refuse one and reverted by Undo leaving the number as it was.
It is removed by Del cons, the numbers staying, and marked by Show.
Deleting a number's digits removes the Dists, Changes and Gaps on it.
A typed value cannot be undone.

In a master, a number keeps its value and whether it is set, with the Dists, Changes, and Gaps
in the master's constraints.
An instance shows the master's numbers at its size, each value doubled for each step above x1, x2
doubling it and x4 making it four times, five digits at most.
Copy and Dismem give each of the master's numbers a number in the drawing, its value at the
instance's size.
Copy and Define over an instance are refused when the drawing has no room for the numbers.

## Misc

Misc's list holds the tape's rows, **Save**, **Load**, **Drive n >** and **Dir**, then
**Del cons** and **Unfix**.

**Del cons**, its OK touched, removes every constraint in the drawing.
That is every Horv, Equal, Paral, Perp, Length, and Mid, every N and C that holds a point on a line
or a circle, every fix, every attacher and every designation, and every Dist, Change, and Gap.
The truss's Beam, Load and Roller marks stay since they describe the truss rather than hold the
shape, and so do the instances' tie points.
Nothing moves.
Each point stays where it is, now free to move.
Any gesture under way ends and the program is left idle as after Clear.
Undo cannot bring the constraints back.

**Unfix**, its OK touched, frees every fixed point, makes every attacher an ordinary point again,
and clears every designation, as Sutherland's Unfix did.
Every other constraint stays.
The figure then settles as the remaining constraints are recalculated now that the freed points can move.
The instances' tie points stay and so do the attachers kept in the masters.
A truss's pin is a fixed point so after Unfix a truss with Forces on shows UNSTABLE until a point
is fixed again.
Any gesture under way ends and the program is left idle.
Undo cannot bring the fixes, the attachers, or the designations back.

## Saving and loading pictures

A *picture* is everything the program keeps.
That is the drawing with its texts, numbers, constraints, fixed points, attachers, designations
and tie points, the masters with the constraints, texts and numbers each was defined with, and the
cycle's dwells and sequence.
It is saved to and loaded from a Microtape.
Each tape holds sixteen pictures, P1 to P16, and each picture can have a name.

Mount the tapes on the emulator's drives before saving or loading, as *UsingType550Microtape.md*
describes.
The program never mounts a tape itself.
To try a sample picture, mount its image on a drive, choose that drive, and load P1.
```
bin/mtp 1 /opt/pidp1-mods/FunStuff/Sketchpad/SketchpadBridge.img,locked
```
*,locked* write-protects the drive, so a Save cannot overwrite the sample.

- **Drive n > D1 to D8** chooses the drive.
  The row shows the drive chosen, and it starts at D1.
- **Dir** reads the tape's directory and shows, in place of the drawing, the drive and the
  sixteen pictures, P1 to P8 down the left and P9 to P16 down the right, each its name, `-`
  where no picture was saved.
  **OK** closes it.
- **Save** reads the directory and shows the same canvas, with **Cancel** in place of OK.
  Touching a picture's line picks it.
  The keyboard opens with the name if already save or empty with the prompt `P1-16 name`, up
  to 12 characters.
  **OK** writes the present picture to tape as the selected picture with the name shown, which can be
  empty, replacing whatever was there.
  **Cancel**, on the keyboard or on the list, saves nothing.
- **Load** shows the same list and touching a picture's line replaces everything on the canvas
  with that picture from the tape.
  **Cancel** loads nothing.
  Undo is cleared and the program is left idle, the target as it was.
  The view goes back to x1, and Show and Forces are off.

While the tape runs, the canvas shows only WAIT at its center.
Afterward a word under the menu at its right, shows the completion status for a few seconds.
After SAVED or LOADED the picture's name also shows under the status.
If there is no tape or it cannot be read when Save, Load or Dir reads the directory, nothing opens.
A tape that is write-protected is detected at the save itself, after the name is typed.

The directory is one block in the middle of the tape which each save rewrites after its picture.
P8 and P9 are beside it, so they save and load in about 5 seconds.
The further a picture is from the middle, the longer the tape runs,
up to about 19 seconds for a load of P1 or P16 and 30 for a save.

**Be patient!** It's a tape drive running at the actual speed the real one ran. Welcome to the '60s.

The directory only lists the picture name.
A save cut short can leave its line showing the old name, or `-` if the directory's block
itself was damaged

## View

The drawing is a page the size of the visible canvas.

**View** magnifies part of it, as Sutherland's scale knobs did.
Its list holds **x1**, **x2**, **x4**, **x8**, **Pan** and **Center**.\
At x1 the canvas shows the whole page.\
At x4 it shows a quarter of the page's width, four times the size, around the view's *center*.\
When magnified, the canvas is effectively larger than the screen although it is stil the same absolue size.

- **x1 to x8**, its OK touched, sets the size.
  The center stays, moved only as far as needed to keep the view on the page.
  The mode and any gesture under way stay in effect, so a line can be started at x1 and ended at x4.
  At x1 the center is always the middle, coming back to x1 forgets where the view was.
  Every change of view redraws the whole picture which takes up to a second with a large drawing.
- **Pan** is a mode that drags the drawing, as if moving it across and off the canvas.
  Move the target and lift, and the drawing moves as far as the target did, in the same direction,
  so the point that was under the target at the start ends under it at the lift unless the
  page's edge stops it first.
  The move is counted from where the target rested when Pan was chosen, at the last lift, after
  Locate, or after a size was chosen, so the same move always gives the same pan.
  At x1 its row is dim.
- **Center** is a mode.
  Stop leaves it and returns to Locate.
  The dot locks onto points, as in Fix, and each lift moves the view's center to the dot.
  A lift on a point brings that point to the middle of the canvas or as near it as the page allows.
  At x1 its row is dim.
  Stop returns to Locate.
- Choosing a size in Pan or Center keeps the mode but forgets the move under way.
- The view is not saved.
  It is x1 at the start and after a Load, Clear keeps it as set.

What the view shows.
- **The canvas's top**, magnified, is the menu's lower edge.
  Nothing is drawn over the menu and "edge" in this section means that edge of the canvas.
  At x1 the whole page shows as always.
- **Lines and arcs** are cut at the canvas's edge.
  An arc that leaves the canvas and curves back shows only the part remaining on the canvas.
  While a circle is drawn, the band shows the part nearest the pen.
  Arcs are drawn smooth at every size.
- **An instance** shows only while its entire figure is on the canvas and its size times the view's
  is x8 or less.
  Otherwise it is hidden, with its texts, and the pen cannot select it.
  One moved past the canvas's edge is hidden as long it is past.
- **Texts and numbers** grow with the view, up to x8, so a text of size x4 shows at x8 at a view of
  x2 or more.
  A text that would cross the canvas's edge is not shown and the pen cannot select it.
  A number shows the same value at every size.
- **Marks**, Show's letters, fixes, attachers, the truss's marks and the circles' crosses, and the
  truss's readouts keep their size.
  Each is hidden while what it marks is off the canvas, a mark's place, a cross completely, or a
  readout's line's middle.
  A mark's arms go with it, hidden with it, and otherwise drawn magnified and cut off at the
  canvas's edge.

**The pen at a larger view.**
The target and the dot stay on the canvas, and the dot reaches as far on the canvas as at x1 so at
x4 it locks only onto what is a quarter as far away.
A point is placed to the page's unit, the same at every view.
The view shows more, it does not place finer, so at x4 new points land four screen units apart.
What is off the canvas cannot be reached, so Pan to it.
The constraints and the truss still act on the whole drawing, on the screen or not.

## The pen keyboard

Where the program needs typed text or a number, it shows a keyboard in place of the drawing
A prompt is at the top, then the text typed so far with a cursor, `_`, after it, then the keys.

```
  1  2  3  4  5  6  7  8  9  0
  Q  W  E  R  T  Y  U  I  O  P
  A  S  D  F  G  H  J  K  L  -
  Z  X  C  V  B  N  M  .  ,  /
  (  )  :  =

  Shift  Space  Back  Clear    OK  Cancel
```

- A key types the moment the pen touches it and flashes.
  Nothing more types until the pen is lifted.
  A pen held on a key types it once and a pen dragged on over other keys types nothing more.
  Lift and touch again to type again.
- The program knows the pen is lifted only after it has seen no hit for a quarter of a second, so
  lift for that long between keys.
  A drag that lingers that long between keys counts as a lift too, and will cause the next charater to be selected.
- **Shift** turns lower case on and off for the letters, and stays bright while on.
  The digits and marks are the same in either case.
- **Back** deletes the last character, **Clear** all of them.
  A key does nothing once the text is as long as allowed.
- **OK** keeps the text, and **Cancel** leaves things as they were before the keyboard opened.
  Either closes the keyboard and brings the drawing, the menu, and the target back.
- A keyboard for a number shows only the digits, `-`, Back, Clear, OK, and Cancel; Stiff's shows
  `.` too, for its tenths.

While the keyboard is open, only its keys respond the pen.
Draw's Text opens it for a text, and Misc's Save for a picture's name.
Change's Dwell and Seq, Truss's Stiff, and Dim's Number open the number keyboard.
A pen still resting where it touched when a keyboard opens, on Save's OK, or on OK when Dwell's
next master's keyboard opens presses nothing, lift it first.

## Messages

Each message shows under the menu, at its right, for a few seconds, except for WAIT which displays
in the center of the screen while the tape runs, and UNSTABLE, which stays visible while the truss has no solution.

| Message | Meaning |
|---|---|
| WAIT | The tape is running |
| SAVED | The picture was saved, its name shown under the word |
| LOADED | The picture was loaded, its name shown under the word |
| NO TAPE | No tape is mounted on the drive chosen |
| LOCKED | The tape is write-protected |
| EMPTY | No picture was saved in the slot or its save was cut short |
| BAD BLOCK | A block of the picture could not be read correctly |
| NO FLAG | A tape fault, shouldn't happen |
| NO ROOM | A text, a number, or an instance's texts would exceed the program's limits |
| OFF EDGE | A text would run off the screen |
| UNSTABLE | The truss has no solution, see *The truss* |

## Limits

- Up to 256 points, 264 lines, 19 arcs and 32 instances.
- Four masters, with up to 16 constraints kept with each master.
- Up to 64 constraints, counting fixed points, attachers and designations, the truss's members,
  loads and rollers, and the distances Dist, Change and Gap keep.
- Six attachers in a drawing, so six tie points an instance, and six designations.
- Up to 32 texts in the drawing, and 32 across the masters together, each of up to 24 characters.
- The screen can show 180 characters of text in all, the drawing's texts and the instances'
  together, each text counting two more than its characters, a space counting and Shift not.
  That is about as much text as the display can draw without making the target slow to follow the
  pen.
- Up to 32 numbers in the drawing.
  A number's digits are one of its texts and count against the texts and the characters as any
  text does.
  An instance's digits count as six characters, the most they can show.
- The view magnifies up to x8.
  An instance whose size times the view's exceeds x8 is hidden and a text is drawn at x8 at most.
- A circle's radius is from 15 to 1294 units.
- A picture's name is up to 12 characters.

## sketchplot, a picture as a PNG

*sketchplot* draws a picture saved on a Microtape as a PNG file without the emulator needing to be running.
It replaces the plotter that was on the TX-2, since the pidp-1 doesn't have one. Yet.
It is a plain C program.
It isnin the *Render* directory and is built when sketchpad is built.

Usage.
```
sketchplot [-d n] [-p n] [-t image] [-s size] [-i] out.png
sketchplot [-d n] [-p n] [-t image] --dump
```
- *-d n* reads the tape on drive n, 1 to 8, the default is 1.
  The drive's image file is its line in */opt/pidp1-mods/microtapes.txt*, as
  *UsingType550Microtape.md* describes, set by *mtp*.
- *-p n* draws picture Pn, 1 to 16, the default 1.
- *-t image* reads that image file instead, ignoring *-d* and the drive list.
- *-s size* makes the PNG *size* pixels square, 16 to 8192.
  The default is 1024, one pixel a screen unit, at othe sizes the drawing is scaled to fit.
- *-i* draws black on white for printing.
  The default is light lines on black as the Type 340 shows them.
- *--dump* prints the picture's tables, its header, name, points, lines, arcs, instances,
  masters, texts, and digits, and writes no PNG.

An unknown option prints the usage and a value out of range says which range it wants;
both exit with status 2.

It checks the picture as Sketchpad's Load does.
A picture that fails is refused with the word Sketchpad would show, EMPTY or BAD BLOCK, exits with
status 1, and nothing is written.
A file that is not a whole number of 1032-byte blocks, or is longer than 576 of them is refused
since it cannot be a valid tape.

It draws what Sketchpad's screen shows of the picture just after a load at x1 with Show and
Forces off.
The menu, the target, the marks, and the truss's readouts are not drawn.

For instance, printing the bridge:
```
sketchplot -t SketchpadBridge.img -i bridge.png
```

## Finally, the end

This was designed and written by the coding team of Bill and Claude Opus using Sutherland's original thesis as
the requirements.
Not everything is here yet, but almost all of it is and the rest is coming.

This is free to use in any way, just credit me.

Bill Ezell
October 2026
