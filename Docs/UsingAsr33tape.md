# asr33tape -- Convert an ASR-33 stype  paper tape to and from a text file

`asr33tape` converts between an ASR-33 tape and a text file.
Use it to convert FOCAL and TECO tapes.

## Building

It is built as part of the Tools/Utils build:
```
make
-or-
make install
```

## Running

```
asr33tape punch [-l N] [-f] [TEXT [TAPE]]    a text file to a tape
asr33tape text [TAPE [TEXT]]                 a tape to a text file
asr33tape -h                                 the usage
```

A missing name or `-` is standard input or output.
The exit status is 0 or 1 for an error with a message on standard error.

**`punch`** creates an ASR-33 tape:
- every character with its eighth bit set
- CR LF after each line
- blank frames of leader and of trailer, 60 by default

The text's lines can end in LF, CR LF or CR.
A last line with no terminator has one added.
A NUL or a byte of 0200 or more is an error and no tape is written.

With **`-f`** it also warns of what FOCAL's `*` reads differently from the text.\
The tape is still punched:
- `_` (the left arrow) erases the line read so far
- DEL (RUBOUT) erases the character before it
- `@` is skipped
- a line starting with `*` stops reading, it is an end marker
- a tab or other control character is stored as it is
- a line over 74 characters is taken only as a stored line, one that does
  not start with a line number is `?10.:5`.

**`text`** drops the eighth bit, drops blank frames,
all-holes frames, and makes CR LF and CR one LF.
Everything else is kept as its.

## Examples

A program written in an editor, punched for the reader:

```
asr33tape punch -f primes.fc primes.ptp
```

A tape punched by FOCAL's `LIBRARY PUNCH`, back to text:

```
asr33tape text primes.ptp primes.fc
```
