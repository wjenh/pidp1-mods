# PENKBD: a light-pen keyboard for the Type 340

A keyboard of keys drawn on the 340 that the light pen types on. While it is open it replaces
the program's own frame. It hands back the typed text at OK, or leaves the text alone at
Cancel. There are three kinds: text (letters in both cases, digits, punctuation, Shift and
Space), number (digits and `-`) and decimal (a number with a point). The layout is Sketchpad's
pen keyboard. The code is resident and issues no IOT. The program reads the pen and the clock
and starts the display, and the library answers each call.

| File | What it is |
|---|---|
| `penkbd.ah` | The definitions: the open block, the kinds, the answers, and every constant that can be overridden. |
| `penkbd.ac` | The code, the keyboard's state and its key tables. |
| `penkbdlist.ac` | The display list: the prompt, the field, the 50 keys, and the halt that ends the frame. |

## Including it

```
#include <memory.ah>                // farjda, if the code is in another bank
#include <TYPE340/type340defs.ah>
#define PKHALT lpoff halt           // any override, before the include
#include <PENKBD/penkbd.ah>
...
bank 2
#include <PENKBD/penkbd.ac>         // any bank
    constants                       // the library's literals, in its own bank
...
bank 3
#include <PENKBD/penkbdlist.ac>     // any bank: the list holds no jump and no save
```

Include each file once. A second inclusion does nothing; each file has a guard. Every
global label starts with `pk`, and every macro but the guards with `PK`, `PUI` or `pk`, so a program can
use any other name.

## The contract

- **Calls.** Every entry is a `jda` entry, entered with the extend window open (`eem`). AC
  holds the clock in milliseconds and IO holds the argument. The entry returns its answer
  in AC, with the window still open. From another bank, call through `farjda(pkHit:*)` or
  `crossBankjda(pkHit:*, NOEEM)` (both in `memory.ah`). An entry's 16-bit address must be
  named with `:*`.
- **The clock** is the BBN clock's `rck`. It counts 0 to 59999 ms and wraps, and the library
  takes every difference modulo `PUI_WRAP`. Enable the clock first (`lac [clsena]`, `cls`), or
  `rck` does not count.
- **The pen.** The program reads each hit itself (`dsp`, then `drc`). It passes the `drc` word
  to `pkHit` unchanged, in IO: x in the high 9 bits, y in the low 9, each without its low bit.
  The program also resumes the display (`drs`).
- **The display.** `pkOpen` returns the list's 16-bit address. The program starts its frame
  there with `dla` instead of its own, and starts it again at each end, until `pkHit` returns
  an answer that closes the keyboard. The list ends in `PKHALT`.
- **Not reentrant.** Call it from the main loop only, never from a break routine.
- **Literals.** The code's literals go in the pool of its bank, which the program places
  (`constants`).

## The open block

Five words. `pkOpen` takes the block's 16-bit address in IO.

| Word | Offset | Holds |
|---|---|---|
| `PKBKIND` | 0 | The kind: `PKTEXT`, `PKNUM` or `PKDEC`. |
| `PKBLIM` | 1 | The limit in characters, at most `PKMAX` (a larger one is taken as `PKMAX`). |
| `PKBPROMPT` | 2 | The prompt's 16-bit address: 340 text (am1's `type340`). The first `PKPW * 3 - 1` codes are shown. |
| `PKBBUF` | 3 | The buffer's 16-bit address: 340 text. Its text is the field's starting value, and OK writes the typed text there. |
| `PKBRUN` | 4 | The buffer's length in codes, `PKBUFC` for a buffer of `PKBUFW` words. OK pads the text with upper shifts to this length, so a frame that shows the buffer keeps its place. 0 means a text ended at its end code, with no padding. |

A buffer of `PKBUFW` words holds any text of `PKMAX` characters as OK writes it: an upper
shift first, a shift before each change of case, and the end code.

## The entries

| Entry | Argument (IO) | Answer (AC) | When |
|---|---|---|---|
| `pkInit` | none | 0 | Once, at the program's start, before any other entry. The keyboard is closed. |
| `pkOpen` | the block's address | the list's 16-bit address | To open the keyboard. It reads the buffer's text into the field, writes the prompt and the keys for the kind, and starts a press. A pen still resting where it touched (on the button that opened the keyboard, say) acts on nothing until it is lifted. The list is complete before the answer comes back, so it may be started at once. |
| `pkHit` | the `drc` word | `PKSTILL` (0), `PKOK` (1) or `PKCANCEL` (2) | For each light-pen hit while the keyboard is open. `PKOK` and `PKCANCEL` mean the keyboard has closed, and the program shows its own frame again. A hit while the keyboard is closed is ignored and answers 0. |
| `pkTick` | none | 0 | On each pass of the main loop while the keyboard is open, so that a press ends and a flash goes out. |

**A press.** A key acts at the first hit of a press. The pen reports no lift, so a press ends
when `PKGAPMS` passes with no hit. A pen held on a key types once. A pen dragged across the
blank between keys in less time than that is still in the same press, and types nothing
more.

**The keys.** A character key types its character. A letter is typed in lower case while
Shift is on. Nothing is typed once the text is at its limit. Shift toggles the case, and is
lit while on. Back takes the last character off, and Clear takes them all. OK writes the text
into the buffer and closes. Cancel closes and leaves the buffer as it was. A key that acts
flashes for `PKFLASHMS`. A number keyboard draws only its own keys, and the others are blank,
so the pen cannot hit them. Shift and Space are blanked there too.

**State a program may read:** `pkOn` is nonzero while the keyboard is open. `pkN` is the
count of characters, and `pkCh` holds them, one code a word, with `PKLOWER` set for a
lower-case letter.

## The overrides

Define any of these before the include. The defaults are Sketchpad's.

| Constant | Default | What |
|---|---|---|
| `PKMAX` | 0d24 | The longest text, in characters. `PKBUFW`, `PKBUFC`, `PKFW` and the size of `pkCh` follow from it. |
| `PKPW` | 4 | The prompt's run in words (`PKPW * 3 - 1` codes shown). |
| `PKX0` | 0d112 | The left of the key grid, the prompt and the field. |
| `PKDX`, `PKDY` | 0d80, 0d80 | The key grid's pitch across and down. |
| `PKY0` | 0d720 | The top row's baseline. |
| `PKYB` | 0d280 | The action keys' row. |
| `PKPY`, `PKFY` | 0d880, 0d820 | The prompt's line and the field's. |
| `PKXSHIFT`, `PKXSPACE`, `PKXBACK`, `PKXCLEAR`, `PKXOK`, `PKXCANCEL` | 0d48, 0d228, 0d408, 0d560, 0d740, 0d836 | Each action key's x. |
| `PKR`, `PKRX` | 0d24, 0d12 | A hit's reach: `PKR` left of a key, below it and above it, and `PKRX` right of its text. |
| `PKGAPMS` | 0d250 | The blank, in ms, that ends a press. |
| `PKFLASHMS` | 0d150 | How long a key that acted stays lit. |
| `PKNORMI`, `PKLITI` | 5, 7 | The intensities of a key and a lit key. |
| `PKHALT` | `lpoff halt interrupt` | The list's last word. The default raises the 340's break, for a program whose break routine restarts the frame. A program that polls `dss` defines it as `lpoff halt`. |
| `PUI_WRAP` | 0d60000 | The clock's modulus, shared with PENMENU. |

The keys are drawn at scale 2, `PKADV` (28) apart along a text and `PKH` (26) high. Those
two are fixed by the character size. Key rows closer than `PKH + 2 * PKR` overlap their
reach.

## Size

Measured with the defaults, am1 v5.5:

| Part | Words |
|---|---|
| `penkbd.ac`: the code | 557 |
| its state, `pkCh` included (`PKMAX` words) | 47 |
| its templates and tables: the keys' list addresses, codes and hit records (4 a key) | 309 |
| its literals, in its bank's pool | 40 |
| `penkbdlist.ac` | 238 |

That is 953 words in the code's bank and 238 in the list's.

## An example

The program polls the display and takes no break. `kbd` is nonzero while the keyboard is open,
and `start` is the frame's start. The library is in bank 2; the program is in bank 0.

```
#define PKHALT lpoff halt
#define CALL(e) crossBankjda(e:*, NOEEM)

go, lem
    eem
    lac [clsena]
    cls
    rck
    dio now
    lac now
    CALL(pkInit)
    ...
frame,
    lio start
    dla
poll,
    dsp
    jmp none
    drc
    dio hw
    drs
    rck
    dio now
    jsp onHit
none,
    dss
    jmp tick
    jmp frame
tick,
    rck
    dio now
    lac kbd
    sza i               // skip if the keyboard is open
    jmp poll
    lac now
    CALL(pkTick)
    jmp poll

// Open: IO = the block's address.
    lio [block:.]
    lac now
    CALL(pkOpen)
    dac start           // the keyboard's list replaces the frame
    law 1
    dac kbd

// A hit while the keyboard is open.
    lio hw
    lac now
    CALL(pkHit)
    sza i               // skip once the keyboard has closed
    jmp back
    dac answer          // PKOK or PKCANCEL
    dzm kbd
    lac [myFrame:*]
    dac start

block, PKTEXT
    PKMAX
    prompt:.
    buf:*
    PKBUFC
prompt, type340 "Name:"
buf,
    table PKBUFW - 1, PKPAD
    PKBLANK
```

`Projects/MiscTasks/penui/uidemo.am1` is a complete program that uses both pen libraries.
