# Using the Type 340 display for the PDP-1

This document describes the Type 340 display and how to use it.

This is version 1.5

Edit date 24-June-2026\
Add details of iots and the registers used

## What is it?

The Type 340 display was a successor to the original Type 30 point-plotting display. It was used on just about all
of DEC's 18-bit computers, definitely PDP1's, 4's, 6's, 7's and 10's.

While it seems to have used the same CRT and deflection circuitry as the Type 30, it added vector drawing, sprites,
character drawing both vertially and horizontally, as well as point plotting and lightpen support.

It could also drive up to 16 additional Type 343 'slave' monitors.
These seem to have been the regular Type 30 display without any circuitry other than the analog deflection
and lightpen circuitry.

It could run inependently requring minimal interaction by a program.
It accomplished this by having what was essentially its own simple computer which fetched its instructions
directly from main memory via a high-speed channel.

With the Type 347 Subroutine Interface, it had multi-level graphic subroutine support for its instructions.

Character drawing required the additon of the Type 342 Character Generator.
It supported one or two character sets, at least one of which was field-modifiable.
With only one character set, selecting second set mode changed the drawing of the characters from horizontal
to vertical.

This implementation is complete, it has all of the optional features.

The primary reference for the display is the *H_340_Precision_Incremental_CRT_System* document from Nov 1964.
This document does not provide all the information about use, but summarizes the operation and describes the
support added as an **am1** *include* file.

## The instruction set

8 instructions are provided, some with suboperations.
They are:

- Parameter, basic control over intensity, size, lightpen operation, and interrupt enable
- Point, similar to the Type 30 dpy IOT, move to a location and draw a point
- Slave, enable one or more of attached Type 343 monitors and each's lightpen enable control
- Character, draw a string of characters, shifted and unshifted
- Vector, draw a vector in a direction and length relative to the current position
- Vector contine, draw a vector in a direction from the current location to the edge of the screen
- Increment, draw a pattern by moving one dot spacing up, down, right, left, or diagionally and repeat
- Subroutine, perform a jump, a call, or set up for multi-depth calls

In operation, a single IOT tells the 340 where its program is located in memory, which can be in any bank.
It then executes instructions until it hits a stop condition or sees a lightpen hit if lightpens are enabled.

It is important to stress that a display program runs without any interaction with the PDP-1 program umtil it
needs to, which can be never, it is autonomous.

## Execution flow

At any time, the 340 is in one of three states, *halted*, *running*, or *paused*.
It is initially in the halted state.

When a *dla*, *display load address counter* IOT is issued it enters running state and remains in that state
until it executes a *halt* instruction, an edge violation occurs, or a lightpen hit is seen.

For a stop or edge violation, it enters the halt state and sets the *stop* flag which can be tested with the
*dss*, *display skip on stop interrupt* IOT.
If actual interrupts have been enabled by the user program, it will also issue a break request to SBS channel 0.

For a lightpen hit, if enabled, it enters the pause state and sets the lightpen flag which can be tested with the
*dsp*, *display skip on light pen flag*, IOT.
Only for this case, the display program can be resumed from where it was executing by issuing a *drs*,
*display resume sequence* IOT.
If interrupts have been enabled by the user program, it will also issue a break request to SBS channel 0.

While running or paused, it is always in a *current mode*, which is which of the 8 instructions is executing.

The *parameter*, *slave*, *point*, and *subroutine* commands have a field to specify the mode to enter
next.

The *vector*, *vector continue*, *character*, and *increment* commands always set *parameter* as the next mode
when they complete.
This is because they execute a sequence of their own operations until a *done* flag is seen in their current
subcommand.
That command completes, then parameter mode is entered automatically.

It can be tricky to keep track of all of this, so careful attention must be paid.
For example, if no halt is encouterd, the 340 will happily keep going through main memory executing random
instructions.
This can lead to some interesting displays.

## The IOTs

The display is controlled by 3 IOTs with a total of 8 subinstructions.
Of these, only one is needed to start a display program.
The rest are for determining the status of the display.

The assigned IOTs are 15, 16, and 17. It apparently was not unusual for different installations to use
different IOT assignments.

- dla - display load address, start a display progran at the address in the IO register - see note
- drs - display resume sequence, used to resume after a lightpen event
- dcf - display clear flags, clears the flags following
- dra - display read address counter, the last address executed
- drc - display read coordinates, the x and y position of the lipthpen hit
- dsp - display skip on lightpen flag
- dss - display skip on stop
- dsv - display skip on vertical edge violation
- dsh - display skip on horizontal edge violation


In more detail:

Only one IOT, dra, takes a value passed in the IO register.\
Only two IOTs return a value, dra in the IO register, drc in the IO and AC registers.

| IOT | pdp-1 opcode | input | output | notes |
|-----|--------------|-------|--------|-------|
| dla | 720015 | IO has prgram adress | none | full 16 bit address, see note 1 |
| drs | 720115 | none | none | use after lightpen hit or edge violation to resume execution |
| dcf | 720215 | none | none | clears the 340 dkip flags |
| dra | 720o16 | none | IO has current execution address | if the 340 is halted, will be the next location to execute |
| drc | 720116 | none | IO and AC have the last lightpen hit coordinates | see note 2 |
| dsp | 720117 | none | none | dsp, dss, dsv, dsh can be combined, see note 3 |
| dss | 720217 | none | none ||
| dsv | 720417 | none | none ||
| dsh | 721017 | none | none ||

**Note 1** - the address is a full 16 bit address, any location in any bank can be used without
enabling extended memory.
Memory access by the 340 is via a high speed dma channel and is independent of any pdp-1 memory mode.

**Note 2** - The x and y coordinates do not include the least-significant-bit of the screen coordinate, the values
are x >> 1 and y >> 1.
The x coordinate is in IO bits 0-8, the y coordinate in AC bits 0-8.
The values are indeterminate if there has not been a lightpen hit.

**Note 3** - All of the skip subcommands can be combined, just as for the normal PDP-1 skip instructions, e.g.\
dsv dsh dss

## Scale factor

A scale factor can be set via the *parameter* command.
This can be confusing.

Scaling can be speciifed as 0, 1, 2, or 3 and changes pixel spacing by a factor of 1, 2, 4, or 8.
It affects only *relative* motion.

An abolute mvve goes to the coordinates specified.\
For drawing vectors, characters, or increments the spacing between each plotted point is increased by the scaling
factor.

This has the effect of magnifying what is being drawn.
For example, a vector command of length 10 will be drawn as 10, 20, 40, or 80 in length and the new position
will be adjusted accordingly.

## Command details

This lists the provided definitions for the various commands and shows the format of the commands.
It is not exhaustive, refer to the original manual mentioned at the beginning of this document.

Unless otherwise specified, the settings for each command are or'd together.

## Parameter

The definitions relevant to this are:

- parameter - the command itself
- next(command) - set the next command to execute, if not set defaults to parameter
- scale(0-3) - set the spacing between displayed dots, 0 is 1 pixel, 1 is 2, 2 is 4, and 3 is 8 pixels
- intensity(0-7) - set the intensity, 0 the dimmest, 7 the brightest
- lpon - enable the lightpen
- lpoff - disable the lightpen
- halt - enter the stopped state, set the stopped flag
- interrupt - if halt is set, also initiate a channel 0 break
- charsets(1,2) - change between single and dual character sets mode
- specialinterrupt - enable or disable interrupts from a lightpen hit or edge violation, nonstandard

Example:
```
parameter scale(2) intensity(4) lpon next(vector)
```

The added *charsets* setting allows switching between one and two character sets.
The original hardware could have one or two, but it was a wired-in feature.
This allows emulating either configuration.
The default setting comes from the system config file.

The character set selection automatically resets to the configuration default if *charsets* is not explicitly
used after a *dla*.

The added *specialinterrupt* setting allows the original unmaskable light pen and
edge violaton interrupts to be disabled.
The original was quite inconvenient if other code was using the interrupt system.
If disabled, a light pen hit will only set the status flag and pause execution, an edge violation
will set the status flags and hanlt.

The default setting is enabled to match the original behavior.

## Slave

There are four groups of four displays, 16 in total. The actual number is (and was) implementation dependent.

- slave - the command
- next(mode) - the standard selector for the next command
- group(0-3) - selects a group of 4 displays
- slave1(x) - first display in a group, x below
- up to slave4(x)
- enabled - the display is enabled, it will display the same image as the primary display
- lpenabled - the lightpen for the display will be tracked, assuming the primary is enabled

Example:
```
slave group(0) slave1(enabled|lpenabled) slave2(enabled) next(vector)
```

## Point

Move to a point, draw if enabled

- point - the command
- next(mode) - the standard selector for the next command
- x(xcoord) - specify the x location, 0-1023, 0 being the left side
- y(ycoord) - specify the y location, 0-1023, 0 being the bottom
- show - display the point, else just move

Example:
```
point x(100) y(500) show next(point)
```

## Character

This is one of the special case commands that executes a string o subcommands until it sees an end
character.

Using the *end* character is **mandatory** to terminate a character string!

There are up to 2 character sets each with 64 characters, one of which in each is an escape character, 037.
Characters are packed 3 to a word. Words will be fetched and displayed until an escape character is seen,
in which case the mode reverts to *parameter*.

One or two character sets can be enabled in the */opt/pidp-1/mods/pidp1.config* file, the option is *two340charsets*. 
Also see the section on *Parameter*.

**IMPORTANT** - these *are not* concise/fiexo characters!

The easiest way to create a character string is to use the **am1** *type340* directive.
See *The character sets* below for the binary character codes.

- character - the mode name

Example:
```
parameter next(character)
100514
141737
```
This displays 'HELLO' and enters parameter mode.\
See the document *TYPE_340_Programming_Manual DECUS NO. 7-13* for details.

## Vector

Draw a vector relative to the current positon.
Like *character*, it consists of a list of subcommands that will be fetched until one with the *end* flag
is seen.

- vector - the mode name
- up - draw upwards
- down - draw downwards
- left - draw left
- right - draw right
- deltay(length) - limited to 0-127 decimal
- deltax(length) - limited to 0-127 decimal
- visible - display the vector, else just move
- end - stop and return to *parameter*

The directions and endpoints determine the angle and length of the vector.

Example:
```
parameter next(vector)
up deltay(10) right deltax(50) visible end
```
Draw a line from the current location to a point 10 display points up and 50 to the right.

But why is there an invisible vector modoe? Why not just use *point*?
It's because it takes the vector operation only 1 microsecond per pixel to move, while *point* takes
a fixed 35 microseconds. So, for short moves, vector is faster.

## Vector continue

This is very similar to *vector*, except the endpoints only establish the angle.
A line will be drawn from the starting point until it hits an edge.
It is a single-word command, hitting an edge returns to *parameter* mode.

- vcontinue - the mode name
- visible - display the vector, else just move

Example:
```
parameter next(vcontinue)
up deltay(10) right deltax(50) visible
```
This will draw a vector at the same angle as the example above, but it will continue until it hits an edge.

## Increment

This is an interesting command. Like *vector* etc, it is a list of subcommands that are executed until a word
with the *done* flag is executed.
Each word contains 4 subfields, each of which, in succession, moves the current display point one position in
any of 8 different directions and optionally displays it.
This can be used to draw sprites or complex shapes. It is how the sine wave is displayed in the *type340demo.am1*
example.

- increment - the mode name
- set1(x) - the first move in a word
- set2(x) - the second move in a word
- set3(x) - the thrid move in a word
- set4(x) - the fourth move in a word
- incup - move up
- incdown - move down
- incleft - move left
- incright - move right
- visible - the 4 points are displayed, applies to all of them
- done - finish and return to *parameter* mode

Example:
```
parameter mode(increment)
visible set1(incup) set2(incdown|incright) set3(incdown|incleft) set4(incup|incleft) done
```

Draws 4 points in a diamond shape and returns to *parameter* mode.

## Subroutine

This has 3 subcommand variants.\
Save, below, use a special register, the *ASR*, address save register.
All take an address, a 13-bit address(!). This means they can only address bank 0 and bank 1.

- subroutine - the mode name
- jump - subcommand to jump to a location, like to the *jmp* instruction
- save - subcommand to jump and save the return address, similar to *jsp*
- deposit - this one is a bit difficult to understand, but it is used for nested subroutines

Examples:
```
    parameter mode(subroutine)
    jump foo mode(vector)

    parameer mode(subroutine)
    save bar mode(increment)

    parameter mode(subroutine)
    deposit zip mode(vector)
```
For *jump*, control transfers to the commands ar location foo, exactly the same as a normal PDP-1 *jmp*.

The *save* subcommand only works if the final command in the code it calls is a *vector*, *character*, or *increment*
command that specifies *done*. When this happens, control transfers back to the location one after the original *save*
and *parameter* mode is set.

The *deposit* subcommand places a constucted command in the location it addresses which will be
a *jump (asr) + 1*, one location past the last *save* command.
See the *Type 340 Programming Manual*, noted above, to see just how this makes sense.

## Edge violations and lightpen hits

Unlike the Type 30 display, there is no automtic wraparound at the edges of the screen.
Any movement past an edge is a hard error.
This will halt processing and set a horizontal or vertical or both edge violation flag that can be
checked with the skip IOTs above.
The display must be restarted with a *dla* to continue.
A violation will also cause an interrupt unless the nonstandard *specialinterrupt* flag has been set via *parameter*,
a *sequence break*, assuming the SBS system has been enabled.

If enabled, a lightpen hit will also immediately halt the display, but it *can* be resumed via the *drs* IOT.
If given before a new *dla*, processing will resume from where it left off.
A mandatory interrupt will also be issued, the same as for an edge violation, unless disabled with the
nonstandard *specialinterrupt* flag.

When the display halts because of a lightpen hit, the location of the hit can be fetched via the *drc* IOT.
When resumed, the lightpen is automatically disabled. It must be enabled again via a *parameter* command.

## Timing

The Type 340 has some fairly complex timing and most of the drawing commands have variable timing.
Even more confusing, the timing differed depending upon which computer was the host.
The PDP-4 was a slow machine and used a 2.8 usec setup time for a dla start command, the PDP-7 was a much faster
machine, the PDP-1 was in the middle. It is unclear exactly what the setup time for the PDP-1 was, it is assumed
to be 1.5 usecs.

Each instruction also has a setup time of 500 nanosecods.

Interestingly, the scale factor does not seem to affect the positioning time. The display could apparently
move its beam up to 8 real pixels within the 1 usec timing window.

Here are some of the basic timings:

- every command fetch from main memory takes 5 usecs via high speed channel 3
- every command executed has a setup time, as noted above, 0.5 usecs
- a point command always takes 35 usecs, but see below
- vector and vector continue x take 1 usec for each position moved, plus 0.5 usec per displayed position
- increment is similar, each move takes 1 usec, plus 0.5 usecs if the point is displayed.
- character takes 1 microsecond per position move in the 5x7 matrix plus 0.5 usecs for each visible dot

When using the point command, the order the x and y coordinates drastically affects the timing.

Setting the y position does not incur the 35 usec delay as long as it does not have the display flag set,
which should generally be the case.

Setting the x position *always* causes a 35 usec delay.

So, if moving in both axes, set y without visible first, then x.
If moving only in the y axis, then visible will be set and the 35 usec delay will occur.

Setting both x and y with visible set on both will cause a 70 usec delay!

## The character sets

Two character sets are included.

Somewhat confusingly, *upper shift* selects character set 1, while *lower shift* selects charater set 2.

The second is available if configured via the *two340charsets=yes* directive in the pidp-1 config file
or it has been set in the *parameter* command.

If it is not enabled, then using a lower case shift means to display the character set 1 text vertically.

Both character sets have special escape characters with specific meanings:

| Character code | Meaning |
|-----------------|-----------|
| 033 | linefeed |
| 034 | carriage return |
| 035 | upper shift |
| 036 | lower shift |
| 037 | end |

When using the **am1** *type340* directive to create strings of text, the following escape sequences
can be used:

| Escape sequence | Meaning |
|-----------------|-----------|
| \\e | end |
| \\U | upper shift+ |
| \\L | lower shift+ |
| \\A | automatic shift+ |
| \\b | backspace* |
| \\n | newline, a carriage return and linefeed |
| \\l | linefeed |
| \\r | carriage return |
| \\s | superscript* |
| \\u | subscript* |
| \\00 | 1 or 2 octal digits 0-7 |

The characters marked with an asterisk, *, are only valid when character set 2 is in use.\
If octal digits are given, the a character of that octal value is inserted.

For the shift escapes, marked with a +, explicitly shifting disables automatic shifting
until the command completes or until automatic shifting is enabled again.

Character set 1

| Octal value | Character |
|-------------|-----------|
| 00 | blob |
| 01 | A |
| 02 | B |
| 03 | C |
| 04 | D |
| 05 | E |
| 06 | F |
| 07 | G |
| 10 | H |
| 11 | I |
| 12 | J |
| 13 | K |
| 14 | L |
| 15 | M |
| 16 | N |
| 17 | O |
| 20 | P |
| 21 | Q |
| 22 | R |
| 23 | S |
| 24 | T |
| 25 | U |
| 26 | V |
| 27 | W |
| 30 | X |
| 31 | Y |
| 32 | Z |
| 33 | LF |
| 34 | CR |
| 35 | HORIZ |
| 36 | VERT |
| 37 | ESC |
| 40 | space |
| 41 | ! |
| 42 | " |
| 43 | # |
| 44 | $ |
| 45 | % |
| 46 | & |
| 47 | ' |
| 50 | ( |
| 51 | ) |
| 52 | * |
| 53 | + |
| 54 | , |
| 55 | - |
| 56 | . |
| 57 | / |
| 60 | 0 |
| 61 | 1 |
| 62 | 2 |
| 63 | 3 |
| 64 | 4 |
| 65 | 5 |
| 66 | 6 |
| 67 | 7 |
| 70 | 8 |
| 71 | 9 |
| 72 | : |
| 73 | ; |
| 74 | < |
| 75 | = |
| 76 | > |
| 77 | ? |

Character set 2

| Octal value | character |
|-------------|-----------|
| 00 | blob |
| 01 | a |
| 02 | b |
| 03 | c |
| 04 | d |
| 05 | e |
| 06 | f |
| 07 | g |
| 10 | h |
| 11 | i |
| 12 | j |
| 13 | k |
| 14 | l |
| 15 | m |
| 16 | n |
| 17 | o |
| 20 | p |
| 21 | q |
| 22 | r |
| 23 | s |
| 24 | t |
| 25 | u |
| 26 | v |
| 27 | w |
| 30 | x |
| 31 | y |
| 32 | z |
| 33 | LF |
| 34 | CR |
| 35 | HORIZ |
| 36 | VERT |
| 37 | ESC |
| 40 | space |
| 41 | ??? |
| 42 | ??? |
| 43 | ~ |
| 44 | ??? |
| 45 | ??? |
| 46 | up arrow |
| 47 | left arrow |
| 50 | down arrow |
| 51 | right arrow |
| 52 | \ |
| 53 | [ |
| 54 | ] |
| 55 | { |
| 56 | } |
| 57 | ??? |
| 60 | _ |
| 61 | ??? |
| 62 | \| |
| 63 | ??? |
| 64 | ??? |
| 65 | ??? |
| 66 | ` |
| 67 | ^ |
| 70 | ??? |
| 71 | block? |
| 72 | backspace |
| 73 | subscript |
| 74 | ??? |
| 75 | ??? |
| 76 | ??? |
| 77 | superscript |

For the second character set, the *subscript* and *superscript* characters are modal, like the shift characters.
They stay in effect until the opposite one is used.

## A real example

This is fully functional **am1** assembler, try it.
```
Draw a triangle on the Type 340
#include <TYPE340/type340defs.ah>

// Remember, range is only 7 bits, 0-127
100/
top, lio [prog
    dla
    jmp .

// Begins in parameter mode
prog, scale(1) intensity(4) next(point)
    y(0d512) next(point)
    x(0d512) show next(vector)
    visible up deltay(0d109) right deltax(0d63)
    visible down deltay(0d109) right deltax(0d63)
    visible deltay(0) left deltax(0d126) end
    mode(subroutine)
    jump prog

start top
```
Note that it takes exactly 2 PDP-1 instructions to set up and start the display.
It then just spins, the display itself continually redraws the triangle with no program interaction.
