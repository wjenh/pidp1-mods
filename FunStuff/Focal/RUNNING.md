# Running PDP-1 FOCAL

FOCAL,1969, DEC's interpreter for the PDP-8, rewritten in am1 for the PDP-1D.
The language is the one DEC's *FOCAL Programming Manual*, DEC-08-AJAB-D (January 1970) describes.
This guide gives only what you need to run it and what differs from the manual.

It talks over a DCS2 telnet connection on port 2030, or on the typewriter with sense switch 6 on.
Programs are saved on paper tape or by name in a library on a Type 550 Microtape or on the drum.

This is version 1.0\
Edit date 13-Nov-2026\
Initial version.

## Building and starting it

This needs am1 and the pidp1-mods environment.

Just type:
```
make
```

This makes *focal.rim*.

Load *focal.rim* like any other tape using *pdp1central*, *fastload*, or the paper tape reader.
The starging address is 0100.
It needs the multiply and divide instructions (`muldiv=on`), the BBN clock, DCS2, and at least eight banks of memory.
The library operations need a Microtape on a drive,
or tracks 030 to 037 of the Type 23 drum.
Without either FOCAL runs as usual and only the LIBRARY words that use them will give an error.
FMOV and FDRW draw on the CalComp plotter, device 73, and `OUTPUT PRINTER` prints on the Type 62 line printer,
device 45, see below.

When it starts it waits for a connection or if sense switch 6 is on, the Soroban typewriter.
It prints `PDP-1 FOCAL, 1969/2026`, `*` and and is ready.
Starting again at 0100 clears everything, the program, the variables, the format, and the trace.

## Talking to FOCAL

Connect a telnet client to port 2030 of the machine running FOCAL:
```
telnet <host> 2030
```

FOCAL prints its name and `*`.
Type commands and program lines as the manual shows them:
```
*TYPE 2+2,!
=    4.0000
*1.1 TYPE "HELLO",!
*GO
HELLO
*
```
- Delete (or backspace) erases the last character and echoes `\`.
  Left arrow, `_`, erases the whole line.
- Ctrl-C stops a running program, prints `?01.00` and the line it stopped in, then `*`.
- Lower case is read as upper case, except inside quoted strings.
- A typed line holds at most 69 characters, as on FOCAL,1969.
  Up to 128 characters typed ahead are kept.
- `L` alone ends the session and closes the connection.
  The program and the variables are kept for the next connection, as they are if the connection drops.
  A program that is running stops on a disconnect.
- In MODIFY, telnet sends RETURN and LINE FEED as the same character, so both end a line.
  ALT MODE, typed as ESC or `}`, keeps the rest of the line, as LINE FEED does in the manual.

## On the typewriter

With sense switch 6 on, the session is on the typewriter instead.
The switch is read at the start, while FOCAL waits for a connection, and at every `*`.
FOCAL prints its name on the device it moves to, you can change back and forth freely.

The typewriter lacks some of FOCAL's characters so other keys replace them, typed and printed:

| Key | Stands for |
|---|---|
| `\|` | `;` |
| `~` | `:` |
| `&` | $, the dollar sign |
| `'` | `%` |
| `` ` `` | ALT MODE |

In MODIFY only, `\` is FORM FEED and `@` is BELL.

The tab key is Ctrl-C, but the typewriter loses a key typed while it prints so it only stops a program that
computes or waits for input, not one that prints without a pause.

Turning sense switch 5 on is an immediate stop, turn it off again after the stop.

## What differs from the manual

- **Line numbers.** A group is 1 to 255 and a step .001 to .999.
  The step reads as a decimal fraction, so `1.1`, `1.10` and `1.100` are one line.
  WRITE lists a group in at least two digits and a step in two with a third only
  when it is not 0: `01.10`, `01.105`, `120.50`.
- **Numbers** are kept to about 10 digits, not FOCAL,1969's 7, and printed as FOCAL,1969 prints them
  to at most six significant digits.
  S sixth digit may differ from the manual's by one; the manual's is the less exact.
  A FOR whose step is not exact in binary can end a pass sooner or later than expected.
  The table of section 4.1 ends at 1.00009 here, where the manual's goes on to 1.0001.
- **The manual's faults are fixed.** An extra `)` is an error, not endless zeros.
  `%5` prints -0.4 as `-    0`, not `-    1`.
  Results exceeding the range, about 10^616, are errors, not a silent wrap.
- **IF** takes any expression, in parentheses or not.
- **A variable's name** is a letter and an optional letter or digit, a subscript is -2048 to 2047.
- **The functions** are valid to 9 significant digits.
  FRAN gives numbers from 0 inclusive to 1 exclusive with same sequence after every start, its argument is ignored.
- **WRITE** does not end the line it is on: `WRITE 2; TYPE "DONE"` types DONE.
  ERASE and MODIFY end it, as in the manual.
- **`L`** alone ends the session, `L` with a word is LIBRARY, see below.
- **New error messages**, in the manual's form:

| Message | Meaning |
|---|---|
| `?06.55` | a subscript outside -2048 to 2047 |
| `?07.:8` | a function given the wrong number of arguments |
| `?20.36` | FLOG of a negative number |
| `?23.36` | a number written exceeding the range |
| `?27.10` | a result exceeds the range |
| `?31.45` | the library's tape or drum tracks are full |
| `?31.46` | no tape on the drive |
| `?31.47` | the drive is write-locked |
| `?31.48` | a bad block or a device error |
| `?31.49` | no library on the tape or the drum's tracks, use `L F` |
| `?31.50` | the file is read-only, it cannot be deleted |
| `?31.60` | the plotter's X is off the paper |
| `?31.61` | the plotter's Y is past 65,535 either direction |
| `?31.70` | OUTPUT with no word, or a word other than PRINTER or TERMINAL |

The library's other errors are LIBRA's, the manual's disk library:
- `?30.71` an unknown LIBRARY word
- `?30.<0` a bad or missing argument
- `?31.42` the name is not saved
- `?31.43` it is already saved
- `?31.44` the library holds 31 files alreadf

## Drawing on the plotter

Two functions draw on the CalComp plotter, each when it is evaluated and each returns 0.
A program calls them with SET:
```
*SET Z=FMOV(100,0); SET Z=FDRW(300,0); SET Z=FDRW(300,200)
```
- `FMOV(X,Y)` lifts the pen and moves it to (X,Y).
- `FDRW(X,Y)` lowers the pen and draws a straight line to (X,Y).
  The pen stays down.
  Lowering the pen leaves a dot, so `FMOV` then `FDRW` to the same place draws one.

X and Y are in steps of 0.01 inch, rounded to the nearest step.
X runs across the paper from where the pen starts, 0 to 1,099 on a 560 or 565.
Y runs up the sheet as it is read, -65,535 to 65,535, the paper is a roll.
A position exceeding these limits is an error before the pen moves.

A line is a staircase of straight and diagonal steps, the nearest to the true line, 5 ms a step.
The plotter has no single diagonal step up and to the left, so those lines are drawn with small corners.
Ctrl-C stops a line where it is and lifts the pen, FOCAL knows where the pen stopped.

The pen's position is kept when FOCAL is started again at 0100,
the plotter keeps its sheet until the emulator power switch is turned off or the emulator terminated.

FOCAL is built for the 560's paper, 1,100 steps across.
For a 563, set `PLT_WIDTH` to 2950 in *focal.ah* and type `make`.

If `plotterorigin` starts the pen farther right than you want, change `PLT_WIDTH`.
FOCAL waits on the plotter's free flag on `cks` bit 7, the configuration file `plottercks` default.

Functions take a list of arguments.
`FABS(1,2)` and similar give `?07.:8`, where FOCAL,1969 gives `?08.47` if the argument list isn't valid.

## Printing on the line printer

`OUTPUT PRINTER`, or `O P`, sends TYPE's output and WRITE's listings to the Type 62 line printer.
`OUTPUT TERMINAL`, or `O T`, switches them back:
```
*O P
*WRITE ALL
*FOR I=1,10; TYPE I,!
*O T
```
Only TYPE and WRITE print.
The `*`, what you type, ASK's `:` and its answer, error messages, and the trace stay on the terminal.

Output goes back to the terminal at `O T`, at any error, at Ctrl-C, when the connection drops,
and when FOCAL is started again at 0100.

Each of these except the start at 0100 first prints any part of a line not yet printed, a start at 0100 drops it.
FOCAL ejects no pages.

A line is printed when it ends, at up to 600 lines a minute.
A line longer than 120 columns continues at the start of the next printed line.
TYPE's `#` returns to the start of the line, an overprint.
A blank leaves the character already there, any other character replaces it.

The printer writes to */tmp/pdp1lpt.txt*, the IOT's default.
If that file is a FIFO with no reader, FOCAL waits for one with each line just as a printer out of paper would,
and Ctrl-C is not seen until the line is accepted.
FOCAL uses the Type 62's instructions only.
With `lptType64` set in *pidp1.config*, `O P` misprints.

## Saving programs

**On paper tape.** `LIBRARY PUNCH`, or `L P`, punches the program as WRITE ALL lists it with leader and trailer.
To read a tape back, mount it on the reader and type `*`.
FOCAL types `*` for each line it reads and stops at the tape's end or at Ctrl-C.
The tape adds to the program in memory, so type `ERASE ALL` first for a clean copy.

*asr33tape*, installed with the emulator turns a text file into a FOCAL tape and a FOCAL tape back into text.
A program can be written in an editor:
```
asr33tape punch -f primes.fc primes.ptp
asr33tape text primes.ptp primes.fc
```
`-f` warns of anything FOCAL would read differently from the text.
`asr33tape -h` gives the usage.

**In a library.** The library is the shared file system of *Am1Includes/FILESYSTEM* on a Microtape or on the drum.
For a Microtape, mount a tape on drive 1 using *pdp1central*; `L U 0` uses the drum instead.
A new tape or the drum the first time need a library first.
`L F` writes an empty one, overwriting anything already there.

| Command | Short | What it does |
|---|---|---|
| `LIBRARY SAVE name` | `L S name` | saves the program under the name |
| `LIBRARY CALL name` | `L C name` | replaces the program with the saved one and runs it |
| `LIBRARY DELETE name` | `L D name` | deletes the name |
| `LIBRARY LIST` | `L L` | lists every file with its size in blocks, then `FREE BLOCKS n` |
| `LIBRARY FORMAT` | `L F` | writes an empty library on the tape or the drum |
| `LIBRARY UNIT n` | `L U n` | uses drive n, 1 to 8, or 0 for the drum |

A name is 1 to 6 letters and digits, the first a letter.
A name already saved must be deleted before it is saved again.
A library holds 31 files.
It has 573 blocks on a tape and 125 on the drum's tracks.
A program filling the whole text area takes about 34 blocks.
LIST shows other programs' files that use the same library also.
One whose name is longer than 6 characters cannot be called or deleted from FOCAL.

**The drum.** FOCAL's library is tracks 030 to 037, the drum's top eight.
Preservinge those those tracks is up to you, use *pdp1central* to manage drum copies.
To put the library on other tracks, change `DR_LIB0`, the first track, and `DR_LIBN`, how many tracks,
in *focal.ah* and type `make`.
A library already on the drum has to be created again.
On the drum each LIBRARY takes well under a second.
On a tape it can take up to about 30 seconds while the tape searches, and Ctrl-C is seen only once the search is over.

**A saved program** is WRITE ALL's listing as text.
A program can be copied to and from the librarywith *fstool*, the file system's tool, see *Docs/UsingFstool.md*:
```
fstool -l samples.mtp
fstool -x -n CH41 -f text -o ch41.fc samples.mtp
fstool -a -n PRIMES -f text -s primes.fc samples.mtp
fstool -D 030,010 -l /opt/pidp1-mods/pdp23drum
```
`-D 030,010` names FOCAL's drum tracks, eight from track 030.
*fstool*'s numbers are decimal unless they start with 0.
`030` is octal, as is `0777`, `24,8` is the same volume in decimal.
A file added this way is loaded as usual.
A line FOCAL cannot read is an error at the CALL, as it would be from paper tape.
Change a tape or the drum only while FOCAL is not running a LIBRARY word.

An invalid library `L L` gives `?31.49`.

## The samples

*Samples/* has the programs of the manual's chapter 4, each as WRITE ALL lists it.
*samples.mtp* is a Microtape holding them also, made from the listings by *fstool*.
Mount *samples.mtp* on drive 1, type `L L` to see them and `L C CH41` to run one.

| Name | File | Section | What it does |
|---|---|---|---|
| `CH41` | *ch4-1.focal* | 4.1 | a table of FSIN, FCOS, FLOG and FEXP |
| `CH42` | *ch4-2.focal* | 4.2 | circles and spheres from a radius, Ctrl-C ends it |
| `CH43` | *ch4-3.focal* | 4.3 | Fahrenheit to Centigrade, from, to and in steps you give |
| `CH44` | *ch4-4.focal* | 4.4 | a damped sine curve plotted on the terminal line by line |
| `CH45` | *ch4-5.focal* | 4.5 | a damped sine wave plotted, its amplitude and damping asked for |
| `CH46` | *ch4-6.focal* | 4.6 | a dice game against the house |
| `CH47B` | *ch4-7b.focal* | 4.7, Example 2 | the largest of twelve numbers typed, three rows of four |
| `CH47C` | *ch4-7c.focal* | 4.7, Example 3 | simultaneous equations in up to four unknowns |

Section 4.5.1 draws on the PDP-8's scope with FDIS, which PDP-1 FOCAL does not have, so it is left out.
4.7's Example 1 is a single direct command, not a program.
The listings are text, use `asr33tape punch` to make a paper tape of any of them.

*Samples/plot.focal*, not on the tape, draws a damped sine wave and its axes on the plotter,
10 inches by 6, in about half a minute.
Type its lines, or read its tape with `*`, then `GO`.

## Credits

- FOCAL and the programs in *Samples/* are from Digital Equipment Corporation's *FOCAL Programming Manual*,
DEC-08-AJAB-D, January 1970.
