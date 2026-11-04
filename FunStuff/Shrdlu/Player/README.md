# shrdlu_play: a script player for SHRDLU

A telnet client in C that types a script's sentences into SHRDLU one at a time and waits for each to
finish,  a whole dialogue runs with no one at the keyboard.
It works with both the assember and the C versions of SHRDLU, talking over the same telnet port.

## What it prints

**Stdout is the telnet session**, as a person at a telnet client would see it.
It is printed as it arrives, so a command that pauses during its output is visible.
The telnet commands and NULs are discarded and a CR LF shows as a newline,
so the text also reads cleanly in a file or a pipe.

**Stderr is the player's own output**, each step's time, the run's total and its longest step, and any error.
`-q` leaves the times and the total out, errors always print.

```
shrdlu_play -h localhost -p 2042 dialog.txt > session.txt
```

The default, just shrdlu_play defaults to the above.

A step whose line did not come back with an answer, e.g. a lost connection, is reported on stderr.

## Building

```
make
```

Gcc and POSIX sockets are all it needs.

## Running it

Start either program, t30dpy if you want to see the block world, then the player:

| Program | Start |
|---|---|
| native | `shrdlu` |
| PDP-1 | the emulator with the program loaded, each sentence takes seconds to a minute or more |

| Option | What |
|---|---|
| `-h HOST` | the host (localhost) |
| `-p PORT` | the telnet port (2042) |
| `-t SECONDS` | the most to wait for one step to end (300); the PDP-1 program takes up to about 110 s |
| `-c MS` | pause between typed characters (0, the whole line at once); the stop at an unknown word is always typed a character at a time, at 20 ms at least |
| `-g SECONDS` | pause after each step, a demonstration's pace |
| `-a N` | the answer to a question when the script gives none |
| `-f N`, `-n M` | start at the Nth step and stop after M of them. The world is not reset, so a range starts from the world the earlier sentences would have left |
| `-s` | wait for Return on the player's stdin before each step for manual pacing |
| `-l FILE` | also write the session to FILE |
| `-q` | print nothing on stderr but errors |

## The script

A plain text file, one step a line, so `dialog.txt` is a valid script.

- A blank line, and a line starting with `#`, are skipped.
- A line starting with `>` is the answer to the question back that the step before it asks:
  `> 2`. Several in a row answer several questions in a row. An answer with no question to meet is
  an error, and so is a question with no answer, unless `-a` gives one.
- A line ending in `...` is typed up to the dots and a space, which stops the program at a word it
  does not know. The player waits for `PLEASE TYPE RETURN AND CONTINUE THE SENTENCE.`, then types a
  filler word, Return and Ctrl-U, and the rest of the sentence is forgotten. The program says no
  `*` after that, so the step ends there. It is an error if no stop comes (a word the program
  already knows).
- A line starting with `/` is a debugging command and is sent as it is.
- Any other line is English, sent with a Return.

`dialog.txt` is the whole movie script, the 42 sentences of the transcript and the thesis's additional four,
46 lines, with the answer to the question at the 24th line (`> 2`).

## Exit status

| 0 | every step ended |
| 1 | a timeout, a closed connection, or no connection; the step is named |
| 2 | a script or usage error: a malformed script, an unanswered or refused answer, an unexpected stop |
