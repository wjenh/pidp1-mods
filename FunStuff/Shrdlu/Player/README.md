# shrdlu_play: a script player for SHRDLU

A telnet client in C that types a script's sentences into SHRDLU one at a time and waits for each to
finish, so a whole dialogue runs with no one at the keyboard. It works against both programs, which
speak the same dialogue on telnet port 2042: the PDP-1 program on the emulator (DCS2 in telnet mode)
and the native C program (`Modern/`). It starts and stops neither.

## What it prints

**Stdout is the telnet session**, as a person at a telnet client would see it and nothing else: the
banner, then each line of the script as the program echoes it, its reply (`WORDS: ...`, the answer, the
`OK.` after the hand's moves) and the `*` line that ends the reply, then the next line. It is printed as
it arrives, so a long command shows at its own pace. The telnet commands and NULs are taken out and a
CR LF shows as a newline, so the text reads cleanly in a file or a pipe.

**Stderr is the player's own matter**: each step's time, the run's total and its longest step, and any
error. `-q` leaves the times and the total out; errors always print.

```
shrdlu_play -h localhost -p 2042 dialogue.txt > session.txt
```

The programs echo what is typed, so the echo is theirs. A step whose line did not come back (a server
that does not echo) is reported on stderr.

## Building

```
make
```

gcc and POSIX sockets are all it needs.

## Running it

Start either program, then the player:

| Program | Start |
|---|---|
| native | `shrdlu` (`Modern/README.md`; `-f` makes the hand jump, which a test run wants) |
| PDP-1 | the emulator with the program loaded (`../RUNNING.md`); each sentence takes seconds to a minute or more |

`t30dpy` connects to the display on its own, so the scene can be watched while the script plays.

| Option | What |
|---|---|
| `-h HOST` | the host (localhost) |
| `-p PORT` | the telnet port (2042) |
| `-t SECONDS` | the most to wait for one step to end (300); the PDP-1 program takes up to about 110 s over a sentence |
| `-c MS` | pause between typed characters (0, the whole line at once); the stop at an unknown word is always typed a character at a time, at 20 ms at least |
| `-g SECONDS` | pause after each step, a demonstration's pace |
| `-a N` | the answer to a question back when the script gives none |
| `-f N`, `-n M` | start at the Nth step and stop after M of them; the world is not reset, so a range starts from the world the earlier sentences would have left |
| `-s` | wait for Return on the player's stdin before each step |
| `-l FILE` | also write the session to FILE |
| `-q` | print nothing on stderr but errors |

## The script

A plain text file, one step a line, so `dialogue.txt` is a valid script.

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

`dialogue.txt` is the whole dialogue: the 42 sentences of the transcript and the thesis's four, 46 lines, with the answer to the
question back at the 24th line (`> 2`).

## Exit status

| 0 | every step ended |
| 1 | a timeout, a closed connection, or no connection; the step is named |
| 2 | a script or usage error: a malformed script, an unanswered or refused answer, an unexpected stop |

## What it does not do

It does not judge the replies (no expected answer is compared), read the display, or reset the world.
