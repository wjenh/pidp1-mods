# Running ELIZA

Joseph Weizenbaum's ELIZA (MIT, 1965-66), rewritten in am1 for the PDP-1D.
It is the version from the 6 March 1965 listing with the additions the January 1966 CACM paper describes,
the keystack, `NEWKEY`, `PRE` and `=` links, as Anthony Hay's reconstruction has them and with the
1965 program's quirks replicated.

It talks over a DCS2 telnet connection on port 2030 or on the typewriter with sense switch 6 on.
Three DOCTOR scripts are also provided.
The 1965 program wrote its script back to tape at the end of a conversation.
This version writes it to a Type 550 Microtape.

This is version 1.0\
Edit date 26-Oct-2026\
Initial version

## Building and starting it

Just type:
```
make
```

This makes *eliza.rim*.

Load *eliza.rim* like any other tape using *pdp1central*, or the reader and start it at 0100.
It uses DCS2,the microtape, and eleven banks of memory.

When it starts it waits for a connection or for sense switch 6 on.

For the microtape, mount a tape on drive 1 using *pdp1central* or "mtp*.
The tape can be empty.

Without a tape mounted on drive 1 ELIZA still converses but of course can't save a script.

## Talking to Eliza

Connect a telnet client to port 2030 of the machine running Eliza:
```
telnet <host> 2030
```
ELIZA asks `WHICH SCRIPT DO YOU WISH TO PLAY`.\
Type a number:
| Number | Script |
|---|---|
| `100` | The DOCTOR script printed with the 1965 listing (tape 100) |
| `102` | The other DOCTOR script printed with the listing (tape 102) |
| `200` | The DOCTOR script of the 1966 CACM paper's appendix, the one the paper's conversation used |

A script written to the microtape is chosen by its number the same way.
`DIR` typed instead of a number lists the scripts on the tape and asks again, as does an unknown number.
An empty line closes the connection and waits for a new connection.

The script is then read which takes about 6 seconds for 100 and 102 and 9 seconds for 200,
then ELIZA prints its greeting.

Type a line and press RETURN, ELIZA replies.
Again, an empty line ends the conversation.
ELIZA then asks `WHAT IS TO BE THE NUMBER OF THE NEW SCRIPT` then closes the connection.
Each connection is a fresh conversation.
The script is read again,  nothing is remembered from the last conversation except what was written to the tape.
Backspace and delete erase the previous character.

## On the typewriter

With sense switch 6 on, the conversation is on the typewriter instead, or `telnet <host> 1041`. 
he switch is read while ELIZA waits for a connection and before each `WHICH SCRIPT DO YOU WISH TO PLAY`,
but never during a conversation.

It works as via DCS, except:
- An empty line ends the conversation, asks `WHAT IS TO BE THE NUMBER OF THE NEW SCRIPT`, and
  then asks `WHICH SCRIPT DO YOU WISH TO PLAY` again; an empty answer to that asks again too.
- `"` and backspace each erase the last character typed, it is not cleared from the typewriter.
  The 22 March 1965 printout shows `"` used this way.
- The typewriter prints at 10 characters a second, be patient.

## What to expect

- **Upper case output and the original's spacing.** Input is folded to upper case.
  A comma or period in a reply prints with a blank before it, just as the 1965 program printed it,
  as in `WHO , MAY I ASK` and `HOW DO YOU DO . PLEASE TELL ME YOUR PROBLEM`.
  A reply breaks into lines of at most 72 characters, a Teletype's line, at the last blank.
- **Characters.** `?` and `!` read as `.`, `:` and `;` as `,`, and `(` and `)` as blanks; any
  other character the original IBM 7094's code lacks reads as a blank.
  A digit starts a word.
  A line is limited to 255 characters.
- **Keywords.** Scripts 100 and 102 match a keyword on its first six characters, as the 1965
  program did, so a keyword that has the same six as an earlier one is never found.
  Script 200 matches the whole word, as Hay's program does, so its `MACHINES` and `COMPUTERS` are distinct.
- **Out of space.** A conversation's lists share 4,095 cells with the script, script 200 leaves 1,318.
  When they run out, ELIZA prints `LIST SPACE EXHAUSTED` and closes the connection.

## Editing the script with + and *

The 1965 program let its user change the script during a conversation, and so does this one.
An edit lasts until the conversation ends.

Type `+` as an input.
ELIZA prints `PLEASE INSTRUCT ME` and takes a command a line, printing `PLEASE INSTRUCT ME` again after each,
until `START` goes back to the conversation.
A command names a rule by its keyword with its lists in parentheses.
The name-previx above applies here also.

| Command | What it does |
|---|---|
| `TYPE ALWAYS` | Prints the rule as the keyword line, then each decomposition after a blank line with its pattern, its counter and its reassembly rules |
| `SUBST ALWAYS (WHEN) (WHEN DID THAT HAPPEN)` | Replaces the first pattern or reassembly rule beginning `WHEN` |
| `APPEND ALWAYS (0) (IS IT REALLY ALWAYS)` | Adds a reassembly rule at the end of the decomposition whose pattern or rule begins `0` |
| `ADD ALWAYS ((0 ALWAYS LATE 0) (WHY ARE YOU ALWAYS LATE))` | Adds a decomposition, before the last one if the rule ends in one |
| `RANK ALWAYS 5` | Sets the rule's rank |
| `DISPLA` | Prints every rule, each with its bucket number, then the memory rules |
| `START` | Back to the conversation |

Only the first six characters of a command count, as in 1965, so `DISPLAY` is `DISPLA`.
Anything else prints `CHANGE NOT RECOGNIZED`; a keyword, list or number that is missing or is
not found prints `LIST NOT FOUND`.
Numbers print as the 7094 printed them, six characters,`000004`, with the character whose code it is for 10
and over (`COMPUTER 00000S` for rank 50).
A list inside a pattern prints in parentheses, `( * WANT NEED )`.

An input whose first word is `*` adds the rest of the line as a rule, nothing is printed.
`* MOM = MOTHER` makes `MOM` read as `MOTHER` from then on.

The 3 March 1965 printout shows the original doing this on a script like 100:
```
+

PLEASE INSTRUCT ME

TYPE ALWAYS

ALWAYS 000001

000000
  1
CAN YOU THINK OF A SPECIFIC EXAMPLE
WHEN
WHAT INCIDENT ARE YOU THINKING OF
REALLY , ALWAYS
PLEASE INSTRUCT ME

START
```

## Keeping a script on the microtape

When a conversation ends, ELIZA asks `WHAT IS TO BE THE NUMBER OF THE NEW SCRIPT`, just as the 1965
program did.
Answer with a number from 1 to 999 and the script as the conversation left it is
written to the tape on drive 1, with every `+` and `*` edit and each rule's place in its
rotation of replies.
The next conversation with that number continues where this one left off.
What a conversation remembers, its `MY` memories and the count that paces them, is not kept, as in 1965.
- An empty answer writes nothing.
- `100`, `102` or `200` asks again, since the built-in scripts cannot be changed.
- A number already on the tape is written over; a new one takes a free slot. The tape holds 17 scripts.
- No tape on drive 1, a locked tape, a tape error, a full tape, or a script larger than 24,576
  characters prints `SCRIPT NOT WRITTEN`.

A script from the tape is chosen at `WHICH SCRIPT DO YOU WISH TO PLAY` by its number and takes
just as long as it would have taken to read a real Microtape.
It keeps the keyword matching of the built-in script it was first made from.
A built-in number always uses the built-in script.

`DIR` typed at `WHICH SCRIPT DO YOU WISH TO PLAY` is an addition to the original.
It lists the tape's scripts, each with its size and the built-in script it came from, then asks again:
```
104  1936 WORDS  FROM 100
201  3246 WORDS  FROM 200
```
An empty tape prints `NO SCRIPTS ON TAPE`, no tape on drive 1 prints `NO TAPE`.

Unlike 1965, the script is written with its greeting and with every keyword's `DLIST`, the
`(/FAMILY)` and `(/NOUN)` lists, so a script read back converses as it did before it was saved.

## A demonstration

The conversation the 1966 CACM paper prints, on script 200.
Its first exchanges:
```
Men are all alike.
IN WHAT WAY
They're always bugging us about something or other.
CAN YOU THINK OF A SPECIFIC EXAMPLE
Well, my boyfriend made me come here.
YOUR BOYFRIEND MADE YOU COME HERE
He says I'm depressed much of the time.
I AM SORRY TO HEAR YOU ARE DEPRESSED
```
Typed in this order on a fresh connection, every reply of the paper's 15 comes out as printed.

## Credits

- ELIZA and the DOCTOR scripts are by Joseph Weizenbaum, MIT.
  The 1965 listing and tapes 100 and 102 were released under CC0 by the Weizenbaum estate.
- SLIP, the list language ELIZA was written in, which this program's list code follows, is
  Copyright 1965 MIT under the MIT license in *SLIP-LICENSE.txt*.
- Script 200 is (c) 1966 Association for Computing Machinery, from the appendix of J. Weizenbaum,
  "ELIZA -- a computer program for the study of natural language communication between man and
  machine", CACM 9(1), January 1966.
- Anthony Hay's C++ reconstruction (CC0), the reference for the 1966 additions, and the
  transcriptions and reconstructions by Hay, Arthur Schwarz and Rupert Lane.
