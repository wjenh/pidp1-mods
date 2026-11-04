# Using **Sketchpad**

This document describes Sketchpad, a reproduction of Sutherland's Sketchpad for the PiDP-1 and its Type 340
display, and how to use it.

This is version 2.0\
Edit date 4-Nov-2026\
Updated for added functionality from the thesis.

## What is Sketchpad?

In 1963 Ivan Sutherland wrote Sketchpad on MIT Lincoln Laboratory's TX-2 for his doctoral thesis,
*Sketchpad, a Man-Machine Graphical Communication System*.\
It was the first program to implement several historically greatly significant items:
- The first of what became modern computer-aided-design programs, CAD
- The beginnings of object-oriented programming
- The first truly interactive human-conputer interface

It allowed creating complex drawings with numerous constraints such as fixed points, equality between
lines, orientation, connectivity, and more, as well as a force modeling system.

A drawing could be saved as a master and placed many times as instances with their defined constraints
and used to build up complex designs.

This Sketchpad is a new program in the spirit of Sutherland's, written for the PDP-1 using the
Type 340 display.
It does what the thesis describes, as far as a PDP-1 and a Type 340 can, and the design notes in
*SketchpadTechnicalDesign.md* list the few differences.
It is a *massive* program for a PDP-1, filling completely all 16 memory banks and is in excess of 57K lines of
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
- Save twelve pictures per tape on the Microtape drive and load them back
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
- The BBN timesharing clock for its 1 ms counter.
- The Type 550 Microtape, only to save and load pictures.
- The Type 23 drum which holds the code overlays.
- The am1 assembler environment.

All of the peripherals are supported by the simh version of the PDP-1 and this program should be able to
run under it with few changes, but the simh target tracking speed might not be suitable.

## The files

Every file is in *FunStuff/Sketchpad*, this guide, a technical design document.
The *Render* subdirectory contains a C program that replaces the plotter the TX-2 version had, allowing
the creation of a standard png file of what would have been plotted.

| File | What it is |
|---|---|
| *sketchpad.am1* | The program's main source file; it includes the others |
| *sp\*.ac* | The rest of the source, thirty-nine files included by *sketchpad.am1* |
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
This creates *sketchpad.rim* and *sketchpad.ovl*, the drum overlays.
It is strongly suggested to use *pdp1central* or *ovlload* to load it, otherwise be prepared
to wait a LONG while, about 5 minutes, it is the equivalent of 16 very long tapes!

*pdp1central* will automatically load the overlays onto the drum, see *UsingPdp1central.md*.

To load it with *ovlload*, which will also start the program:
```
ovlload sketchpad.ovl
```
The program starts at address 100.

*fastload* does not load the overlays, and if they aren't on the drum
the program still runs, but Save, Load, Dir, the library's rows, Plot,
Scale, Define, Dismember and Undo of a Dismember, and anything that opens the pen keyboard show
DRUM, as do Forces and
Stiff mode, every 10 seconds while they are active, leaving the mode as it is.

The screen is divided into two parts, the menu bar at the top and the drawing canvas below that.
When started, the screen displays the menu bar and the word INK in the middle of the canvas.
Touch INK or **Locate** to bring the tracking target to the middle of the canvas and start
having fun.

To make an item on the screen brighter or dimmer, change its level in *intensities.ah*, 0 being the
dimmest to 7 the brightest, and assemble again.

## The lightpen

The lightpen is the mouse.
Holding the left button down is holding the pen to the screen, releasing it lifts the pen.
Remember, it's a lightpen, it only detects dots that are showing, blank areas do nothing at all.

Lifting the pen or moving it off the target faster than the target can follow look the same, a *lift*.
A lift is what completes most actions so losing tracking can give unexpected results.
However, using t30dpy and the pidp1-mods pdp1 emulator, tracking speed is quite fast.

In this document, *pen-down* and *touch* mean the same thing, the left mouse button is pressed.

## The target and the dot

The *target* is a cross on the canvas.
Put the pen down on it and move, and the target follows the pen.
This is *tracking*.
Lift the pen and the target stays where it was left.
Move smoothly.
A pen that stops dead or reverses sharply at fast speed can leave the target behind, but it takes
some effort to make this happen.
This should not usually be a problem.

At the target's center is a small dot which Sutherland called the *pseudo pen location*.
The dot is where things happen, where a line starts and ends, what is picked up, what is deleted.
Near a point the dot jumps onto that point, it *locks*.
Otherwise, the pen would have to be manipulated with more precision than is useful.

In the modes that can take a line, an arc, an instance, or a text, the dot locks onto
whichever is nearest.
If there are multiple potential locks, there is a selection priority,
a point is preferred over a line that is nearly as close.

An instance or a text is identified by its middle, so aim at the middle to select it.\
The dot also locks onto the crossing of two lines.\
Moving with nothing being dragged locks onto lines but not arcs.

In Line, Circle, and Move modes it finds lines and arcs once the target pauses, however briefly, and always at the
moment the pen lifts.
It does the same while Copy is carrying a copy by its attacher.
However, again, the actual action only happens when either the pen is lifted or the target doesn't move for several
seconds.

What a lift will do:
- A bright cross around the dot marks the selection point, the same in a Line, Circle or Move's drag.
  It also marks a line, an arc, or a crossing.
- A line or an arc that a lift selects is also drawn brighter.
  This is true in Delete, in Move with nothing dragged, and in constraint actions that take lines.
- An instance or a text the dot is locked onto is also drawn brighter.

When the program starts the target is hidden and **INK** shows in the middle of the canvas.
Touch INK with a pen-down or select **Locate** to bring the target to the middle of the canvas.
This is Sutherland's "*inking up*, the target appears where INK was.
Choosing a mode that uses the target also brings it to the middle if it is not already displayed.
INK is shown only until the target first appears.
From then on the target remains on-screen, hidden only while a list or the pen keyboard is open.

Again, remember, unless the pen is down and on some visible point that the program treats as an active point,
the pen does nothing.

## The menu bar

The bar reads, left to right, **Draw**, **Edit**, **Picture**, **Misc**, **Locate**, **Undo**,
**Stop** and **View**.
The menu items always respond to the pen being put down on an item regardless of what the program is doing.

**Locate**, **Undo** and **Stop** act the moment a pen-down touches them.

**Draw**, **Edit**, **Picture**, **Misc** and **View** open a list below them, a nested submenu.
While a list is open the drawing is hidden and the target does not track.
- Touch an item to select it.
  An item ending in `>` opens a further list beside it.
- A small **OK** box appears beside the active row with a dim dotted line leading to it from the row.
- OK responds only after the pen has been lifted and put back down.
  A pen slid onto it from a row, or brought up from the drawing still held down does nothing until it
  is lifted for at least a quarter second and put back on OK.
  Slide along the dotted line, however slowly, and OK still waits for the lift.
- To close a list without doing anything, touch its item again, touch another bar item,
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
| Dim > Size | an instance and a number |
| Edit > Move, Delete, Scale | a point, a text, or an instance |
| Constr > Horv, Length | a line |
| Constr > Equal, Paral, Perp | two lines |
| Constr > Mid | a line and a third point |
| Constr > Fix | a point |
| Edit > Size > | an instance or a text |
| Edit > Change > | a master defined and an instance placed |
| Edit > Dismem | an instance |
| Edit > Keep >, and its x1/3, x1/2, x1, x2, x3 | two lines, or an instance |
| Keep > Full, Inch | an instance |
| Picture > Define > | a line, an arc, or an instance |
| Picture > Inst >, Copy > | a master defined |
| Picture > Tie, Desig | a point |
| Picture > Name | a master defined |
| Misc > Del cons | a constraint other than a truss's member, load, or roller |
| Misc > Unfix | a fix, an attacher, or a designation |
| Misc > Library > Save | a master defined |
| View > Pan, Center | a view larger than x1 |

Change's Cycle, Dwell and Seq, and Undo have rules of their own, see their sections.\
Define, Inst, Copy, Name, Change's Master and Dwell, and Library's Save and Load open the master
picker, see *Masters, instances and copies*.
Clear always acts, even on an empty canvas.

The entire menu structure is:
| Bar item | Rows |
|---|---|
| Draw | Line, Circle, Truss > (Beam, Load, Roller, Axis, Forces, Stiff), Text, Dim > (Number, Dist, Change, Gap, Size) |
| Edit | Move, Delete, Constr > (Horv, Equal, Paral, Perp, Length, Mid, Fix, Show), Size > (x1, x2, x4, x8), Scale, Change > (Master, Cycle, Dwell, Seq), Dismem, Keep > (x1/3, x1/2, x1, x2, x3, Full, Inch) |
| Picture | Define, Inst, Copy (each the masters' picker, M1 to M16), Clear, Tie, Desig, Name (the masters' picker) |
| Misc | Save, Load, Drive n > (D1 to D8), Dir, Del cons, Unfix, Library > (Load, Save, Dir, Delete) |
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
| Draw | Dim > Size | Keeps a number equal to an instance's size in inches |
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
| Edit | Change > Master | Gives an instance another master, picked on the masters' picker |
| Edit | Change > Cycle | Starts or stops an instance cycling through the masters |
| Edit | Change > Dwell | Sets how long a cycle shows a master, picked on the masters' picker |
| Edit | Change > Seq | Sets the order a cycle shows the masters in |
| Edit | Dismem | Takes an instance apart into ordinary points, lines and arcs |
| Edit | Keep > x1/3, x1/2, x1, x2, x3 | Keeps an instance that many times the size of another, or a line that many times the length of another |
| Edit | Keep > Full | Keeps an instance at its master's size |
| Edit | Keep > Inch | Keeps an instance a number of inches in size |
| Picture | Inst | Places one instance of a master, picked on the masters' picker |
| Picture | Copy | Places one copy of a master, picked on the masters' picker |
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
Tapping on the target without moving it acts on what the dot is on.

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
right until dismissed, see *Messages*.
If there are more characters can be displayed, see *Limits*, is refused with NO ROOM.
With the drawing's maximum number of texts reached, a lift opens no keyboard and shows NO ROOM.

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

A text is moved in its entirety, the place it was moved to stays under the dot.
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
- If a number's digits are deleted, that also deletes the number with them and every Dist, Change and Gap assigned it.
- With Show on, a lift with the dot on a constraint's mark, within its circle, removes that
  constraint alone and nothing else.
  The mark wins over anything else in reach.

Taking the pen from the dot to the menu, to Stop, or any other item, deletes nothing, only a lift deletes.

**Misc > Del cons** removes every constraint at once, see *Misc*, below.

### Size

Edit > Size > x1, x2, x4 or x8.
Lift with the dot on an instance or a text to set it to the chosen size.
Size works on instances and texts only.
Ordinary lines and arcs are not resized here, Scale resizes them, see below.
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
It is a square wave read as a ruler, level runs joined by upright ticks that mark doublings,
and beside the dot is the factor in digits, `x1.00`.
Put the pen down and move it along the slider.
To the right the part grows, to the left it shrinks, twice or half as large at each tick.
The pen's up and down motion is ignored.
Lift to select that size.
Scale stays in force until another mode is selected or a Stop is done.

- A line is scaled about its middle or about its fixed end, which stays fixed.
  Its unfixed ends move and the lines and circles on them follow.
  A Length on it takes the new length, and a Change on it its reference length is scaled the same
  way, so its reading scales with the line.
- A circle or an arc is scaled about its center.
- A text or a number steps through the same sizes Size sets, x1, x2, x4 and x8, the slider
  snapping to the nearest and grows as it does under Size.
- An instance takes any size from x1/4 to x8, 32 sizes to a doubling, so two showing the same
  digits are the same size.
  A dim box shows the size as the pen slides, nothing else changes until the lift, when the
  new size then takes effect , its tie points moving as they do under Size.
  A size whose box would leave the screen is refused, the box and the knob staying at the last
  that fit.
  A lift with no room for the size shows NO ROOM and the instance stays as it was, see *Limits*.
- A step is not made if it would take the part past the target's reach, make a line shorter
  than 4 units, or take a circle's radius below 15 or past 1294.
  The item and the knob stay at the last factor that was valid.
- A line with an end already past the target's reach, near the screen's edge, cannot be scaled
  at all, larger or smaller, move it first.

The slider spans five doublings each way, x1/32 to x32, as far as the target can follow, but for a
text or a number only the sizes x1 to x8, and for an instance x1/4 to x8 are accepted.
Its doublings are whole ones from the size at the pick, so a part picked within 128 units of
where the target stops, about 878 across the screen or more to the right and 145 or less to the
left, has no slider on that side.
To grow an instance near the right edge, pick it further left.

These are refused, nothing picked up and the dot staying dim:
- a line with both ends fixed
- a line with a circle or an arc on both its ends, a radius or a chord
- a circle or an arc whose start or end is fixed
- a cycling instance

Locate selects the current resizing and stays in Scale.
Stop puts it down and return to Locate mode.
Choosing another menu item resizes it first.
Undo reverts a scale.

### Dismem

Edit > Dismem.
Lift with the dot on an instance to take it apart, as the thesis's Dismember does.
It becomes its master's points, lines, arcs, and texts at its location and its size with the
constraints the master was defined with, just as a copy becomes when it is placed.
The instance is gone.

Each of its tie points becomes the point of the master's attacher there so whatever was drawn on
a tie point and any instance fastened at it stays joined to the parts.
A cycling instance is taken apart as the master it currently shows.
Dismem stays in force for the next instance.

It is refused, nothing changing, if the drawing or the constraint table has no room for the
parts, its texts included.
It is also refused if a part would land where the target cannot reach, under the menu or at the
screen's edge, or if an arc, or if the instance's size would be too large to draw.

Taken apart between the powers of two, each point lands where the instance shows it, on the
locations of its size: at x3 every fourth unit.
A master's number, which the instance showed scaled, then reads the line as drawn.
A 100-unit line reading 1000 shows 3000 in an instance at x3, and may read 3040 taken apart, its line 304
long.

An instance of a master that holds instances is taken apart one level.
The master's own points, lines, arcs, and texts are placed as above, and each instance the master
holds becomes an instance on the canvas its own master, at its place and size inside the
instance taken apart.
One held at x2 in an instance at x2 shows at x4.
Its tie points are the placed points it was tied to, so it stays joined to the parts.
The held instances' texts are shown by the new instances, not placed as texts.
A second Dismem on one of them takes the next level apart.
This is also refused, nothing changing, if a held instance's size would be more than x8 or
less than x1/4, if the drawing would have more than 32 instances, or if the masters the drawing
uses would leave no room for the held instances' masters, see *Limits*.

## Constraints

Edit > Constr opens Horv, Equal, Paral, Perp, Length, Mid, Fix and Show.

A constraint is kept until it is deleted or the object it constrains is deleted.
When a point is moved, the program moves the other points it constrains.

While a point is dragged nothing else moves.
At the drop the figure's constraints are applied, redrawn as it converges on the constraint solution.
A constraint that cannot be satisfied or takes too long to solve stops the constraint solver and leaves
the object in the state at that point.
A fixed point is never moved to keep a constraint.

Each constraint mode locks the dot onto lines alone so that at a corner the nearer line is selected.
Mid's first lift and Fix lock onto points.
Each mode stays in force.

A constraint already held or one that makes no sense, such as Mid on one of its line's ends, is refused,
as is one when the constraint table is full and nothing changes.

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
In Fix mode each fixed point carries a small circled F beside it, with Show on or off.
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
If the constraint table is full, the point is placed without one.
A point already kept on that line or circle gets no new constraint.

### Keeping sizes

Edit > Keep opens x1/3, x1/2, x1, x2, x3, Full and Inch, the thesis's constraints on the size of
an instance.
The first five also keep one line's length a ratio of another's.
An instance's *size* here is its overall size as it appears on the drawing, the longer side of its
master's box times its scale, and an inch is 100 screen units.
Two instances of different masters kept at x1 have the same overall size, not the same scale.

**x1/3, x1/2, x1, x2, x3.**
Lift with the dot on an instance, then on a second instance.
The first is kept that many times the second's size.
A second lift on the first instance, or a pair already kept, is refused.

**x1/3, x1/2, x1, x2, x3 on lines.**
Lift with the dot on a line, then on a second line.
The first line picked is drawn bright until the second lift.
The first is kept that many times the second's length, the two settling at the ratio, each about
its middle, or the free one alone if the other's ends are fixed.
x1 is a plain Equal, the same constraint as Constr > Equal, with the mark E; the other four carry
the mark M.
A second lift on the first line, a line after an instance or an instance after a line, or the same
two lines picked again in the same order, is refused.
The same two lines picked in the other order at another ratio are not refused.
Like any contradiction, the solver cannot meet both, and Undo puts the lines back.
These are ordinary constraints between lines, not constraints on size.
They settle when made and after each drop, as Equal does

The rest of this section, groups, CONFLICT, and the limits on size, concern instances only.

**Full.**
Lift with the dot on an instance.
It goes to x1, its master's size.

**Inch.**
Lift with the dot on an instance.
The number keyboard opens with the prompt `Inches`.
Type a size from 0.001 to 10 and OK.
Digits past the third after the point are ignored.
The instance's overall size becomes that many inches, keeping it if Change gives it another master.
Anything else is refused and Cancel changes nothing.

Size constraints act at the lift, never while a point is dragged.
Instances linked by these constraints are a group, a size change on any of them whether by Scale, by
Size, or by a constraint, changes the whole group, all or nothing.
A group holding a Full, an Inch or a set number (Dim > Size) is pinned.
A constraint that contradicts its group is refused with CONFLICT and removed, nothing changes.
A change that would take a member off the screen, or past x1/4 or x8, does the same.
A cycling instance is refused.
Of the things with a size, only instances can be kept.
A text or a number's digits step through four sizes, so most of these sizes could never be met.

Keep's constraints are ordinary constraints, reverted by Undo, removed by Del cons, and saved
with a picture.
Undo reverts the group's change as well.
Deleting an instance removes those on it, Dismem removes them, and its Undo does not bring them
back.
A master keeps none on an instance, as for every other constraint.

### Show

Edit > Constr > Show turns the constraint marks on or off.
Its label is bright while they are on.
It acts within the menu, the mode and any gesture under way stay as they are.

While Show is on every constraint carries a small dim circled letter at the middle of its points,
in every mode, following them as they move.

| Mark | Constraint |
|---|---|
| H | Horv |
| E | Equal |
| M | Two lines kept at a ratio of length, Keep's x1/3, x1/2, x2 or x3 |
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
| S | Two instances kept at a ratio of size |
| U | An instance kept at full size |
| I | An instance kept a number of inches in size |
| Z | A number kept equal to an instance's size |

With Forces on, a member's B and a load's W change to the force symbols.

A letter whose place falls on a line, H, L, X, N, B and W, are placed beside that line instead, its
circle just clear of it, above a shallow angled line and right of a steep one.
A second letter on the same line is placed on the other side, a third beyond the first, a ring
further out.
A letter for one point, F, A, a designated point's digit and a roller's O, is placed just beside
its point, in whichever of eight directions, every 45 degrees, the point's lines leave clearest,
so that the lines through a pin or a roller do not cross it.
A second letter on the same point takes the next clearest direction.
Near the edge of the screen a direction that would put the letter past the edge is passed over.
Only the drawing's own lines count, so a circle or an instance's lines at the point may still
cross the letter.
A number's letters are placed at its digits' upper left rather than on the lines,
a second letter on the same digits further left.
Two letters on two lines each, E, P, R and M, whose lines' middles average to the same place, as an
E on a hexagon's top and bottom edges and another on two of its diameters, are set apart, the
second a ring above the place, a third a ring below, further ones further out.
They are counted when a constraint is added or removed, not as the lines move, so a letter keeps
its ring after a drag pulls the places apart, until the next such change.

An E, a P, or an R, shown at the middle of its two lines, has arms, a dim dotted run from the
first line's middle to the second's, through its circle, so that it shows which lines it is associated with.
A letter set apart on a ring has its arms bent through it, from the first middle to the letter and on
to the second.
The arms follow the lines as they move and are cut off at the screen's edge.
A mark too close to both middles to display shows nothing.

Show is off when the program starts and after a load and is not saved with a picture.

## Undo

**Undo** reverts the last thing added.
That can be a line, an arc, a text, a number, an instance with its tie points, or a copy, with its
texts, its merges, and its constraints, the drawing is put back to the way it was before the addition.
It can be a constraint with whatever the program moved to satisfy it, a fix, an attacher, a
designation, a member, a load, or a roller.
It can be an instance taken apart, its parts with their merges and constraints restored, the
instances it held removed, and the instance put back as it was, cycling again if it cycled.
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

A cycle's steps do not affect an undo, a change made while cycling can be undone.

The Undo label is dim whenever there is nothing to undo.

## Masters, instances and copies

A *master* is a drawing kept for reuse, stored in one of sixteen slots, M1 to M16, and can have a
name of up to 12 characters.
An *instance* is a display of a master, placed anywhere, at any of Size's four sizes or, by
Scale, any size from x1/4 to x8.
Every instance of a master displays that master.
A master can hold instances of other masters, as a bridge holds its bays, see *Define*.
A *copy* is a master's drawing placed as ordinary points, lines, arcs and texts, which can then be
edited one by one.

### The master picker

Define, Inst, Copy, Name, and Change's Master and Dwell show in place of the drawing the sixteen
masters, M1 to M8 down the left and M9 to M16 down the right, each master's line its slot and its
name, a slot not yet defined marked `empty` and a master with no name its slot showing as blank with
**Cancel** at the bottom.
Touching a master's line, its slot or its name, picks it for the row, Cancel leaves things as
they were.
For Define every line is selectable, a slot already used is replaced.
For the others a slot not defined is dim and touching it does nothing.
Misc's **Library >** uses the picker also, see *The library of masters*.

### Define

Picture > Define then a master picked opens the keyboard for the master's name with the prompt, e.g.
`M2  name` for M2, on the name the slot has, empty for a slot not yet defined.
**OK** makes the present drawing, its lines, arcs, texts, attachers, and constraints that master,
with the name shown, which can be empty, and clears it from the canvas.
**Cancel**, on the picker or the keyboard, defines nothing.
Defining into a slot already used replaces that master and its name.

Every instance on the canvas also goes into the new master at its size.
An instance of another master stays an instance inside it, up to eight of them.
Each instance of the new master shows the inner one at its place and size, with its texts.
An instance of the master being defined is taken apart one level as Dismem would, and its parts
become part of the drawing.
The instances it held stay instances inside the new master, counted with the others against the eight.
Either way, its tie points become the master's points and a designated one an attacher.
A master may be redefined with instances of it on the canvas.

Redefining a master that others hold changes them too, every master holding it, at any depth,
takes the new drawing at the same place and size, and the texts it holds with it.
Each outer master's own lines, arcs, texts and attachers stay as they were.
An outer master's tie points on the inner one go to the inner master's new attachers, so Dismem
ties to those; an attacher of the outer master's own on such a point stays where it was, and a
line ending there still ends there.
The larger the outer masters, the longer it takes, about ten seconds for one near its limit.
All of it is tried first, and if an outer master could not take the change, Define is refused
with nothing changed, its message naming that master, see below.

An attacher with no line or arc on it is not kept and a fix is not kept.

While nothing is drawn and no instance is placed, Define and its rows are dim.
Texts alone cannot be masters, there must be drawn objects.

Define is refused leaving the drawing, the instances, and the master's name as they were, its
message showing under the menu until dismissed, see *Messages*, if:
- the drawing is too large for a master (TOO BIG),
- the drawing would take too long to draw as a master (TOO SLOW),
- it has more than 16 constraints among its points (CONSTR),
- all masters together would hold more than 64 texts (NO ROOM),
- while any instance cycles (CYCLING),
- more than eight instances of other masters would be inside the new master, those held by
  instances of the master being defined included (TOO MANY),
- an instance's master holds, at any depth, the master being defined (LOOP),
- an instance's master, or a master held by an instance of the master being defined, has a
  number among its texts (DIGITS),
- an instance of the master being defined cannot be taken apart because the tables lack room
  for its parts, its texts included, the drawing would have more than 32 instances, or the
  masters' store has no room for its held instances' masters (NO ROOM), a part would lie off the
  screen (OFF EDGE), an arc would be too large at its size (ARC SIZE), or an instance it holds
  would come to more than x8 or less than x1/4 (SCALE),
- a master holding the one being defined could not take the change, named by its number, M2 here:
  it would be too large (M2 BIG), too slow to draw (M2 SLOW), the masters' texts or the masters'
  store would run out of room (M2 ROOM), or the master being defined has a number among its texts,
  which a held master cannot have (M2 DIGIT).

Define takes effect as soon as its keyboard's OK is touched, clears the canvas, and leaves the program in Locate mode.

### Inst

Picture > Inst, then a master picked, places one instance of that master.
The new instance follows the tracking target.
A lift, or any menu action, puts it down and the program returns to Locate mode.
It is refused if the drawing has no room for its tie points, or if the masters the drawing
already uses leave no room for this one's display list, see *Limits*.
It is refused as it is chosen, NO ROOM showing, if the master's texts would exceed the characters
the screen can show.

An instance's texts are its master's, shown at its origninal place and size.
A text's size and the instance's multiply, up to x8, so a text placed at x2 shows at x2 in an
instance at x1 and at x8 in one at x4 or x8.
In an instance between those sizes a text keeps its place in the figure, and its characters show
at the next of x1, x2, x4 and x8 up.
The pen does not select an instance's texts, only the instance.
it.

### Copy

Picture > Copy, then a master picked, places one copy of that master onto the canvas.
It is placed as Inst places an instance, but when placed it becomes ordinary points,
lines, arcs, and texts with the same constraints the master was defined with.

A copy of a master with attachers when being placed is dragged by the last attacher that was marked
and locks onto points, lines, and arcs as in Move's drag, though no bright cross is drawn.
At the drop attacher merges into the point the tracking dot is on or is attached to the line or arc it is
on.

Each other attacher merges into its designated point, see *Desig*, below, however far away.
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

A copy of a master that holds instances is placed as its own parts and each instance it holds
becomes an instance on the canvas of its own master, at its place and size inside the master,
as Dismem does with an instance at x1.
Undo of the copy removes them with the parts.

Copy is refused if the drawing or the constraint table has no room for all its parts, or as
Inst is for the master's display list, and for a master that holds instances, if the drawing
would have more than 32 instances or the masters the drawing uses would leave no room for theirs.

### Clear

Picture > Clear erases the whole drawing, its texts, and every instance.
The masters and their texts are kept.
Clear takes effect as soon as its OK is touched and leaves the program in Locate mode.

### Tie

Picture > Tie.
Lift with the dot on a point to make it an attacher, or on an attacher to make it an ordinary point
again.
In Tie mode each attacher carries a small circled A beside it, with Show on or off.
There can be up to six, and a seventh is refused.
The order in which they are made matters to Copy since the last one made is the one a copy is moved by.
**Misc > Unfix** makes every attacher an ordinary point immediately.

### Desig

Picture > Desig.
Lift with the dot on a point to designate it, next in order, or on a designated point to
undesignate it, the later ones moving up one.
In Desig mode each designated point carries a small circled digit beside it, 1 to 6, its place in the order,
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

### Name

Picture > Name names a master, or renames it, without defining it again.
The masters' picker opens, a slot not defined dim, and touching a master opens the keyboard on its
name, with the prompt `M2  name` for M2.
**OK** gives the master the name shown, which can be empty.
**Cancel**, on the picker or the keyboard, leaves the name as it was.
Name is dim until a master is defined.
The canvas and the mode are left as they were.

A master's name is set by Define and Name, by a library entry loaded into it,its entry's name,
by Library's Save, the name it is saved under, and by a picture's Load, which gives each master
the name it was saved with.

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

Edit > Change > Master, then a master picked.
Lift with the dot on an instance to make it an instance of that master.
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
- where the masters the drawing already uses leave no room for the new master's display list,
- or where the new master's texts would exceed the characters the screen can show (NO ROOM).

### Cycle

Edit > Change > Cycle.
Lift with the dot on an instance to start it cycling.
The row is dim until an instance is placed and at least two masters are defined.
At each step it changes to the next master of the sequence, in M1 to M16 order, passing over slots not defined, and back to M1 unless
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
So is a start on an instance at a size other than x1, x2, x4 or x8, which stays as it is.
While any instance cycles every master of the sequence is in use, so a start, or a new sequence
while one cycles, is refused when they leave no room for one another's display lists, see
*Limits*.

### Dwell

Edit > Change > Dwell opens the masters' picker.
Picking a defined master opens the number keyboard with the prompt `M1 dwell` etc., for how long
a cycle displays that master, in milliseconds.
Type a whole number from 50 to 9999 and OK to accept it.
Anything else is refused, the dwell left as it was.
Cancel, on the picker or the keyboard, leaves the dwell as it was.
Every dwell is 250 when the program starts.
A picture saves its dwells and a load sets them.
The menu row is dim until a master is defined.

### Seq

Edit > Change > Seq.
The number keyboard opens with the prompt `Sequence`: type the masters' numbers, 1 to 16, with a
point between each two, up to 24 characters.
`1.12.3.12` shows M1, M12, M3, M12 and loops.
A sequence left empty is every defined master, M1 to M16, in turn.
OK keeps it, and the next step takes its first entry.
A number missing between points or at either end, 0, a number past 16, or any other character
refuses the whole sequence and the old one is kept.
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
In Roller and Axis modes each roller carries a small circled O beside it, with Show on or off, and two short
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
They are hidden too while Scale is the mode or the pen keyboard is open, and return when it
ends; after a Save, Load, Dir, Define or Dismember they return a moment later, read again from
the drum.

When the truss has no answer that can be calculated, every number shows as three dashes
and UNSTABLE appears under the menu, at its right.
A truss has no answer when it can fold, as a frame missing a diagonal can, when a joint's members
all lie on one line, or when the loads are too large.
While a tape message is being shown under the menu, it takes UNSTABLE's place.

The truss needs supports to stand.
A fixed point is a pin, holding its joint still, and a roller holds its joint across its axis.
A bridge stands on a pin at one end and a roller at the other.

## Numbers and dimension lines

Draw > Dim opens Number, Dist, Change, Gap and Size, the thesis's numbers and dimension lines.

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
With 64 numbers or the drawing's texts or points all used, a lift places nothing and shows NO
ROOM.

**Dist.**
Lift with the dot on a point, then on a second point, then on a number's digits.
The number and the distance between the two points are kept equal.
A free number on one Dist reads the distance and follows as the points move.
A set number moves the points to its distance, just as Length keeps a line's length constant.
A free number on several Dists brings them and itself to their mean, as Equal does.
A value that cannot be met, below 0 or past the screen's diagonal of 14480, moves nothing.

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

**Size.**
Lift with the dot on an instance, then on a number's digits.
The number and the instance's overall size in inches are kept equal, see *Keeping sizes*.
A free number reads the size and follows it.
A set number drives the size and pins the instance's group.
A typed value that conflicts with the group is refused with CONFLICT, and the digits keep the value
typed.
A number takes a Size or distances, not both.

Each point picked carries a small cross until the constraint is made or the mode is left by Stop,
Locate or another row.
The dot passes over a point already picked without it being selectable.
Dist's or Change's second point must be another point.

Each of the four is a constraint.
It is refused as the Constr modes refuse one and reverted by Undo leaving the number as it was.
It is removed by Del cons, the numbers staying, and marked by Show.
Deleting a number's digits removes the Dists, Changes, Gaps and Sizes on it.
A typed value cannot be undone.

In a master, a number keeps its value and whether it is set, with the Dists, Changes, and Gaps
in the master's constraints.
An instance shows the master's numbers at its size, each value multiplied by it, x2 doubling it
and x1.5 making it half as large again, five digits at most.
Copy and Dismem give each of the master's numbers a number in the drawing, its value at the
instance's size.
Copy and Define over an instance are refused when the drawing has no room for the numbers.

## Misc

Misc's list holds the tape's rows, **Save**, **Load**, **Drive n >** and **Dir**, then
**Del cons** and **Unfix**, then **Library >**, the library of masters, see *The library of
masters*, and last **Plot**, the page on the plotter, see *Plot*.

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
and tie points, the masters with their names and the constraints, texts and numbers each was
defined with, and the cycle's dwells and sequence.
It is saved to and loaded from a Microtape.
Each tape holds twelve pictures, P1 to P12, and each picture can have a name.

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
  twelve pictures, P1 to P6 down the left and P7 to P12 down the right, each its name, `-`
  where no picture was saved.
  **OK** closes it.
- **Save** reads the directory and shows the same canvas, with **Cancel** in place of OK.
  Touching a picture's line picks it.
  The keyboard opens with the name if already save or empty with the prompt `P1-12 name`, up
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
Afterward a word under the menu at its right shows the completion status until dismissed, see
*Messages*.
After SAVED or LOADED the picture's name also shows under the status.
If there is no tape or it cannot be read when Save, Load or Dir reads the directory, nothing opens.
A tape that is write-protected is detected at the save itself, after the name is typed.
Each picture holds its masters, which together may take at most 23 of the tape's blocks; a save
whose masters need more is refused before it writes, TOO BIG showing, and the tape is left as it
was.
A picture saved by the earlier Sketchpad, with four masters, loads as EMPTY.
The sample pictures, saved before a master could hold instances, load with their masters holding
none.

The directory is one block in the middle of the tape which each save rewrites after its picture.
P6 and P7 are beside it, so they save and load in about 5 seconds.
The further a picture is from the middle, the longer the tape runs,
up to about 19 seconds for a load of P1 or P12 and 30 for a save.

**Be patient!** It's a tape drive running at the actual speed the real one ran. Welcome to the '60s.

The directory only lists the picture name.
A save cut short can leave its line showing the old name, or `-` if the directory's block
itself was damaged

## The library of masters

Sutherland kept a library tape of drawings to build others with.
Here the library is a Microtape of its own holding masters only, up to 256 of them, each an
*entry* with a name.
An entry holds the master's drawing, its attachers, the constraints kept with it, and its texts
and numbers.
The cycle's dwells and sequence belong to the picture and are not kept.

Mount a tape for the library on a drive and choose that drive with Misc's **Drive n >**, as for
pictures.
- A blank tape is an empty library, the first save makes it one.
- A library tape and a picture tape cannot be mistaken for each other.
- A library command on a picture tape shows NOT LIB, and Save, Load or Dir of a picture on a
library tape shows NOT PICT, each before anything is written.

**Library >** opens a list of **Load**, **Save**, **Dir** and **Delete**.
Save is dim until a master is defined.

- **Dir** shows in place of the drawing a page of the library: sixteen entries, numbered,
  1 to 8 down the left and 9 to 16 down the right, each with its name, a free slot dim and
  marked `-`.
  **Next** and **Prev** turn the page, sixteen pages in all; Next stops at the last page with an entry.
  **OK** closes it.
- **Save** shows the masters' picker, a master with nothing in it dim.
  Touching a master picks it, and the keyboard opens with the prompt `Lib name` on the master's
  name, or on the name last saved for a master with no name.
  **OK** writes the master to the tape as a new entry, in the first free slot, and gives the
  master the name it was saved under.
  **Cancel**, on the picker or the keyboard, saves nothing and leaves the master's name.
  A master that holds instances cannot be saved to the library: NESTED shows, and nothing is
  written.
- **Load** shows the library's page, with **Cancel** in place of OK, and Next and Prev.
  Touching an entry picks it, then the masters' picker opens.
  A master the drawing uses is dim: one with an instance, one held inside such a master at any
  depth, every master while an instance cycles, and one whose instance Undo would bring back.
  Touching an empty or unused master replaces it with the entry, the entry's name becoming the
  master's, which then may be placed with Inst or Copy.
  The canvas stays as it is.
  A master that others hold changes them as a redefinition does, see *Define*.
  If one of them could not take the entry the load is refused with nothing changed, TOO BIG,
  TOO SLOW, NO ROOM or DIGITS showing and that master's name under the word.
  Such a load reads the entry from the tape twice, and takes the longer for it.
- **Delete** shows the library's page; touching an entry deletes it, and its slot and its blocks
  are free for a later save.
  **Cancel** deletes nothing.

Undo cannot bring back a master a load replaced, nor an entry deleted.
After SAVED, LOADED or DELETED the entry's name shows under the status word.

A save of a master that would take more than ten of the tape's blocks is refused, TOO BIG, before
anything is written.
A load whose texts would not fit with the other masters' shows NO ROOM, and the master is left as
it was.
A load refused because of a master that holds the one replaced shows that master's name under the
word, as above.
TAPE FULL means no run of free blocks is long enough for the entry.

The library's directory sits in the middle of the tape, and a save puts its entry in the first
free blocks just past it, so most transfers take a few seconds.
The first after a tape is mounted, which starts from the tape's beginning, takes about 20.
A save cut short can leave blocks taken that no entry names, but never an entry whose blocks a
later save could overwrite.

## Plot

**Plot**, Misc's last row, draws the page on the CalComp plotter as *sketchplot -i* draws it.
It is dim while the canvas has nothing on it.
The page is drawn at full size, one plotter step a screen unit, so a unit is a hundredth of an
inch and the page 10.24 inches square, and it is the whole page as at x1, whatever the view
shows.

Set the pen at the lower left of where the page should go before touching Plot.
The plot starts there and ends there with the pen up, so a second plot draws over the first in
register.
While it runs the screen shows PLOT, and STOP under it.
Touch **STOP** to end the plot early.
The pen lifts and goes back to the start and STOPPED shows.
At the end PLOTTED is shown.
With no plotter available NO PLOT shows after half a second and nothing is drawn.

The lines are drawn each from its end nearer the pen, the nearest next, so lines that meet are
drawn without lifting the pen.
The circles and arcs are true arcs, drawn by the plotter's steps around the circle, not chords.
A character is its dots joined, as *sketchplot* joins them, in as few strokes as the joins allow.
Anything that runs off the page's edge is drawn where it is on the page.

A plot takes about as long as the CalComp 560's 200 steps a second plus a fifth of a second
for each lift and lowering of the pen.
On the 560 the sample pictures take about 25 seconds for Dimension and the Bridge, and about 80
for the Girl and the GirlWink.

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
Draw's Text opens it for a text, Misc's Save for a picture's name, Define and Picture's Name for
a master's name, and Library's Save for an entry's name.
Change's Dwell and Seq, Truss's Stiff, and Dim's Number open the number keyboard.
A pen still resting where it touched when a keyboard opens, on Save's OK, or on the picker's line
when Dwell's keyboard opens presses nothing, lift it first.

## Messages

Each message shows under the menu, at its right, until the pen is next put down on the menu
or picks up the target.
With the pen up it stays up.
A pen that rests on a label from before the message appeared does not dismiss it, lift
it and touch again.
The exceptions are WAIT, which displays in the center of the screen while the
tape runs, and UNSTABLE, which stays visible while the truss has no solution.
UNSTABLE is hidden while another message shows and returns once that message is dismissed.

| Message | Meaning |
|---|---|
| WAIT | The tape is running |
| PLOT | The plotter is drawing the page; STOP under it ends the plot, see *Plot* |
| PLOTTED | The page was plotted |
| STOPPED | The plot was ended by STOP |
| NO PLOT | No plotter answered, nothing was plotted |
| SAVED | The picture or the library's entry was saved, its name shown under the word |
| LOADED | The picture or the library's entry was loaded, its name shown under the word |
| DELETED | The library's entry was deleted, its name shown under the word |
| NO TAPE | No tape is mounted on the drive chosen |
| LOCKED | The tape is write-protected |
| EMPTY | No picture or entry was saved in the slot or its save was cut short |
| BAD BLOCK | A block of the picture, of the entry, or of the library's directory could not be read correctly |
| NOT LIB | A library command found a picture tape on the drive; nothing was written |
| NOT PICT | A picture command found a library tape on the drive; nothing was written |
| TAPE FULL | The library tape has no free run of blocks long enough for the entry |
| NO FLAG | A tape fault, shouldn't happen |
| NO ROOM | A text, a number, or an instance's texts would exceed the program's limits; or a library entry's texts would not fit with the other masters', or a master that holds the one replaced could not be made again, see *The library of masters*; or Define was refused, see *Define*; or Scale's lift or Undo found no room for an instance's size, see *Limits* |
| OFF EDGE | A text would run off the screen; or Define was refused, or a size kept by Keep or Dim > Size would take an instance off the screen |
| CONFLICT | A constraint on size, or a number typed for one, contradicts its group, see *Keeping sizes* |
| NO PARTS | Define was refused: nothing in the drawing draws, its lines all of no length |
| TOO BIG | Define was refused: the drawing is too large for a master; or Save was refused: the masters together are too large for a picture, see *Saving and loading pictures*, or the master is too large for a library entry, see *The library of masters*; or a library load was refused: a master that holds the one replaced, named under the word, would be too large |
| TOO SLOW | Define was refused: the drawing would take too long to draw as a master; or a library load was refused: a master that holds the one replaced, named under the word, would be too slow |
| CONSTR | Define was refused: more than 16 constraints among the drawing's points |
| CYCLING | Define was refused: an instance cycles |
| TOO MANY | Define was refused: more than eight instances of other masters |
| LOOP | Define was refused: an instance's master holds the master being defined |
| NESTED | Library > Save was refused: the master holds instances |
| SCALE | Define was refused: an instance of the master being defined holds one whose size would come to more than x8 or less than x1/4 |
| DIGITS | Define was refused: an instance's master has a number among its texts; or a library load was refused: the entry has a number and a master, named under the word, holds the one replaced |
| ARC SIZE | Define was refused: an instance's arc would be too large at its size |
| M2 BIG | Define was refused: M2, which holds the master being defined, would be too large; the number is the master's, and so below |
| M2 SLOW | Define was refused: M2 would take too long to draw |
| M2 ROOM | Define was refused: making M2 again would run out of room for texts or in the masters' store |
| M2 DIGIT | Define was refused: the drawing has a number among its texts and M2 holds the master being defined |
| UNSTABLE | The truss has no solution, see *The truss* |
| DRUM | Save, Load, Dir, the library's rows, Scale, Define, Dismember, the pen keyboard or the truss's forces could not be read from the drum, which holds another build's overlays or none; the mode is left, but for the forces, which are tried again every 10 seconds. Load the program again with *ovlload* |

## Limits

- Up to 256 points, 264 lines, 19 arcs and 32 instances.
- Sixteen masters, with up to 16 constraints kept with each master, and up to eight instances
  of other masters held inside each.
  A master is up to 128 lines and 530 words of display list: a line drawn on from the last one's
  end takes a word, one elsewhere two, a long line more, and a small circle about 65.
  Every master is kept on the drum. The masters the drawing uses, its instances' and, while any
  instance cycles, every master, are also kept whole in one of two stores in core, of 422 and 530
  words, so together they are up to 952 words, and the room left can be enough in all and too
  little in either.
  A master nothing uses gives its room up when another needs it.
- An instance at a size other than x1, x2, x4 or x8 draws a copy of its master's display list made
  at that size, as long as the master's and kept in the same two stores.
  There are up to sixteen such copies, one for each master and size in use: instances showing the
  same digits share one, and so do sizes a doubling apart, x1.5 and x3.
  A copy nothing uses gives its room up when another needs it, and a master whose instances all
  draw copies is not itself kept in core.
- Up to 64 constraints, counting fixed points, attachers and designations, the truss's members,
  loads and rollers, the distances Dist, Change and Gap keep, and Keep's and Dim > Size's.
- Six attachers in a drawing, so six tie points an instance, and six designations.
- Up to 64 texts in the drawing, and 64 across the masters together, each of up to 24 characters.
- The screen can show 360 characters of text in all, the drawing's texts and the instances'
  together, each text counting two more than its characters, a space counting and Shift not.
  That is about as much text as the display can draw without making the target slow to follow the
  pen.
- Up to 64 numbers in the drawing.
  A number's digits are one of its texts and count against the texts and the characters as any
  text does.
  An instance's digits count as six characters, the most they can show.
  In an instance at a size other than x1, x2, x4 or x8 a value past 65535 does not read true.
- The view magnifies up to x8.
  An instance whose size times the view's exceeds x8 is hidden and a text is drawn at x8 at most.
- A circle's radius is from 15 to 1294 units.
- A picture's name is up to 12 characters, and so is a master's and a library entry's.
- A library tape holds up to 256 entries, each up to ten of the tape's blocks: a master at every
  table's limit still fits, with up to 38 texts.
- Many instances of a large master slow the display: 32 of the largest Define takes draw about
  a frame and a half a second, flickering. Nothing is lost, every command still works, a press
  held a moment longer, and deleting instances, or Undo, brings the speed back.

## Compared with the thesis

Every constraint and nearly every push button in the thesis is here, as a menu row.

Missing:
- Rotation: rotated or mirrored instances, rotated text and digits, and the constraints that
  turn a thing upright or set a text's angle. The Type 340 cannot rotate.
- The Constraint button, which made a constraint on loose points to be merged onto others later.
  Each constraint is a row applied to what you pick, and a new kind is a master with its
  constraints, placed with Copy.
- Truss members inside instances. An instance is rigid, so its members could carry no force.
  Build a truss from copies, as the thesis's bridge was.
- Order and Disorder, which sorted the lines for the plotter. Plot puts them in a good order
  itself.
- Punch, which punched a tape to drive the plotter later. Plot drives the plotter directly.
- A page larger than the screen, and magnification past x8.

In addition:
- Undo of the last thing added, and Desig undesignating, which the thesis calls an oversight.
- Rollers, their axes and each member's stiffness in a truss, its forces solved exactly.
- Instances cycling through the masters by themselves, Change's Cycle, Dwell and Seq.
- A name for each master and each picture, picked on screens that show them.
- Keep > Inch at any size from 0.001 to 10 inches, where the thesis had six.
- Text and numbers typed with the pen, on the pen keyboard.
- *sketchplot*, a saved picture drawn as a PNG.

## sketchplot, a picture as a PNG

*sketchplot* draws a picture saved on a Microtape as a PNG file without the emulator needing to be running.
It replaced the plotter that was on the TX-2 before the pidp-1 had one.
Misc > Plot now draws the same page on the plotter, see *Plot*; *sketchplot* still gives it as a
PNG without the emulator.
It is a plain C program, linked with zlib.
The PNG it writes is compressed, a few kilobytes at the default size.
It is in the *Render* directory and is built when sketchpad is built.

Usage.
```
sketchplot [-d n] [-p n] [-t image] [-s size] [-i] out.png
sketchplot [-d n] [-p n] [-t image] --dump
```
- *-d n* reads the tape on drive n, 1 to 8, the default is 1.
  The drive's image file is its line in */opt/pidp1-mods/microtapes.txt*, as
  *UsingType550Microtape.md* describes, set by *mtp*.
- *-p n* draws picture Pn, 1 to 12, the default 1.
- *-t image* reads that image file instead, ignoring *-d* and the drive list.
- *-s size* makes the PNG *size* pixels square, 16 to 8192.
  The default is 1024, one pixel a screen unit, at other sizes the drawing is scaled to fit.
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
