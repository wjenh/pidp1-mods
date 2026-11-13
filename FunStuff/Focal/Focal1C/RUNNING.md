# Running FOCAL-1C

FOCAL-1C is PDP-1 FOCAL cut down to run on a PDP-1C with two banks of memory, the typewriter, and the paper tape.
It does not use any PDP-1D instructions, the Microtape, the Type 23 drum, DCS, the Type 340, or the BBN clock.
It does use 2 banks of memory, but it should run on the Computer History Museum's PDP-1C.

PDP-1 FOCAL is FOCAL,1969, DEC's interpreter for the PDP-8, rewritten in am1.
The language is the one DEC's *FOCAL Programming Manual*, DEC-08-AJAB-D (January 1970) guide,
which is the language reference.
This guide gives only what you need to run it and what differs from the manual.

It talks via the typewriter only.
Programs are saved on paper tape.

This is version 1.0\
Edit date 13-Nov-2026

## Building and starting it

You don't even need am1, the rims are here, just load them and go.

But, if you want to built it yourself and you have am1 installed:

Just type:
```
make
```

This makes two images of FOCAL-1C:
| Image | Functions | Room for a program |
|---|---|---|
| *focal1c.rim* | FABS, FSGN and FITR | about 1,360 words |
| *focal1cfn.rim* | FABS, FSGN and FITR, and FSQT, FSIN, FCOS, FATN, FEXP, FLOG and FRAN | about 440 words |

Pick *focal1cfn.rim* for a program that uses the other seven functions, and *focal1c.rim* for anything else.
In *focal1c.rim* the other seven give `?31.<7`, as an unknown function does with FOCAL,1969.
The room is shared by the program's lines, its variables and its working storage, see below.

Load the image like any other tape using the reader or *pdp1central* and start it at 0100.
It needs the multiply and divide instructions (`muldiv=on`), the extended-memory control, and banks 0 and 1.

When it starts it prints `PDP-1 FOCAL, 1969/2026` and `*` on the typewriter.
Starting again at 0100 clears everything, the program, the variables, the format, and the trace.

## On the typewriter

Type commands and program lines as the manual shows them:
```
*TYPE 2+2,!
=    4.0000
*1.1 TYPE "HELLO",!
*GO
HELLO
*
```
FOCAL echoes nothing itself.
The typewriter echos each key as it is struck.

The typewriter lacks some of FOCAL's characters so other keys stand for them, typed and printed:

| Key | Stands for |
|---|---|
| `\|` | `;` |
| `~` | `:` |
| `&` | the dollar sign |
| `'` | `%` |
| `` ` `` | ALT MODE |

- Backspace erases the last character.
  Underline, `_`, erases the whole line.
- Lower case is read as upper case except inside quoted strings.
- A typed line holds at most 69 characters, as with FOCAL,1969.
  A key typed past the limit is refused with `?10.:5` even though it was locally echoed  and begins the next line.
- In MODIFY, `\` is FORM FEED and `@` is BELL.
  ALT MODE, typed as `` ` ``, keeps the rest of the line, just as LINE FEED does in the manual, RETURN ends it.
- `L` alone ends the line and any program running.

The tab key is the Ctrl-C equivalent.
It stops a running program, prints `?01.00` and the line it stopped in, then `*`.
*Note* that the typewriter loses any key typed while it prints,
the tab key stops a program that computes or waits for input, not one that prints without a pause.
Turning sense switch 5 on is an immedate stop; turn it off again after the stop.

## What FOCAL-1C leaves out

FOCAL-1C has FOCAL,1969's commands, MODIFY and the trace included, and of PDP-1 FOCAL's additions
only `LIBRARY PUNCH` and reading a tape with `*`.
- **No DCS2.** FOCAL-1C does not answer on port 2030, and does not read sense switch 6.
- **No Microtape library.** `LIBRARY` with any word but PUNCH is `?30.71`.
- **No line printer.** `OUTPUT` is `?03.28`, as an unknown command is.
- **No plotter.** FMOV and FDRW are `?07.;6`, as an unknown name is with FOCAL,1969.
- **One argument.** A function takes only one, as with FOCAL,1969, so `FABS(1,2)` is `?08.47`.

## Room

The space mentioned at the beginning of this document holds the variables and the push-down list:
- a stored line takes 2 words plus 1 more for every two of its characters
- a variable takes 6 words when it is first used, each subscript of an array counts as one variable
- while a program runs, each DO in progress takes 5 words, each FOR 10 words,
and each expression 2 words while it is evaluated.

When the sapce runs out, FOCAL gives FOCAL,1969's errors:
- `?10.:5` for a line
- `?06.54` for a variable
- `?02.79` for a DO, a FOR or an expression

`ERASE` clears the variables and frees their space, `ERASE ALL` clears the program also.

## What differs from the manual

- **Line numbers.** A group is 1 to 255 and a step .001 to .999.
  The step reads as a decimal fraction, so `1.1`, `1.10` and `1.100` are one line.
  WRITE lists a group with at least two digits and a step in two,
  a third only when it is not 0: `01.10`, `01.105`, `120.50`.
- **Numbers** are kept to about 10 digits, not FOCAL,1969's 7, and printed as FOCAL,1969 prints them
  to at most six significant digits.
  S sixth digit may differ from the manual's by one, the manual's is the less exact.
  A FOR whose step is not exact in binary can end a pass sooner or later than expected.
  The table of section 4.1 ends at 1.00009 here, where the manual's goes on to 1.0001.
- **The manual's faults are fixed.** An extra `)` is an error, not endless zeros.
  `%5` prints -0.4 as `-    0`, not `-    1`.
  Results greater than the range, about 10^616, are errors, not a silent wrap.
- **IF** takes any expression, in parentheses or not.
- **A variable's name** is a letter and an optional letter or digit, a subscript is -2048 to 2047.
- **The functions** of *focal1cfn.rim* are valid to 9 significant digits.
  FRAN gives numbers from 0 inclusive to 1 exclusive with the same sequence after every start.
  Its argument is ignored.
- **WRITE** does not end the line it is on.
  `WRITE 2; TYPE "DONE"` types DONE.
  ERASE and MODIFY end it, as in the manual.
- **New error messages**, in the manual's form:

| Message | Meaning |
|---|---|
| `?06.55` | a subscript outside -2048 to 2047 |
| `?20.36` | FLOG of a negative number |
| `?23.36` | a number written exeeding the range |
| `?27.10` | a result exceeds the range |
| `?30.71` | LIBRARY with a word other than PUNCH |

## Saving programs on paper tape

`LIBRARY PUNCH`, or `L P`, punches the program as WRITE ALL lists it, with leader and trailer.
Nothing is typed meanwhile, wait for the punch to complete.
Remember that by default it punches only 63 characters a second.

To read a tape back, mount it on the reader and type `*`.
FOCAL types `*` for each line it reads and stops at Ctrl-C or once the reader has no character available
after about 2 seconds.
Wait those 2 seconds after the tape ends before typing or the characters will be lost.
The tape adds to the program in memory, type `ERASE ALL` first for a clean copy.

*asr33tape*, installed with the emulator, turns a text file into a FOCAL format tape,
and a tape back into text, so a program can be written in an external editor:
```
asr33tape punch -f primes.fc primes.ptp
asr33tape text primes.ptp primes.fc
```
`-f` warns of anything FOCAL would read differently from the text. `asr33tape -h` gives the usage.
A tape holds the text's characters, not the typewriter's stand-ins.
`;` on the tape is `;`, though it prints on the typewriter as `|`.

## The samples

*../Samples/* has the programs of the manual's chapter 4, each as WRITE ALL lists it.
`asr33tape punch` makes a paper tape of any of them.
Its Microtape, *samples.mtp*, is for PDP-1 FOCAL only and *plot.focal* draws on the plotter,
which FOCAL-1C also does not have.

| File | Section | Image |
|---|---|---|
| *ch4-1.focal* | 4.1 | *focal1cfn.rim* |
| *ch4-2.focal* | 4.2 | either |
| *ch4-3.focal* | 4.3 | either |
| *ch4-4.focal* | 4.4 | *focal1cfn.rim* |
| *ch4-5.focal* | 4.5 | *focal1cfn.rim* |
| *ch4-6.focal* | 4.6 | *focal1cfn.rim* |
| *ch4-7b.focal* | 4.7, Example 2 | either |
| *ch4-7c.focal* | 4.7, Example 3 | *focal1c.rim*, as it does not fit in *focal1cfn.rim*'s memory |

## Credits

- FOCAL and the programs in *../Samples/* are from Digital Equipment Corporation's *FOCAL Programming Manual*,
DEC-08-AJAB-D, January 1970.
