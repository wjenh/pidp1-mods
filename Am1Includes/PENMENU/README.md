# PENMENU: a light-pen menu for the Type 340

A bar of items across the top of the screen. Each item is either a button or a list that drops
below it. A row of a list may own one sublist, which opens beside it. There is one Select box,
OK, beside the lit row, which acts on that row. The behavior is Sketchpad's menu: the pen
lights a row by moving onto it, a dim leader runs from the lit row to OK, and OK acts only at
the start of a press. The program declares its menu in one macro, and the library generates
the display list and the tables from it. The code is resident and issues no IOT.

| File | What it is |
|---|---|
| `penmenu.ah` | The declaration's macros and every constant that can be overridden. |
| `penmenu.ac` | The code, the menu's state, and the tables generated from the declaration. |
| `penmenulist.ac` | The display list: the bar, the selector, every list and sublist, the box, the leader, and the jump back to the program's frame. |

## Declaring the menu

Before the includes, the program defines `PM_BAR(L, B)`, with one entry for each column, in
order:

- `L(name, "label", LIST)` for an item that owns a list;
- `B(name, "label", action)` for a button.

Each `LIST` is a macro `LIST(R, S)` with one entry for each row, top first:

- `R(name, "label", action)` for a row that acts;
- `S(name, "label", SUBLIST)` for a row that owns a sublist. The `SUBLIST(R, S)` may hold `R`
  rows only (an `S` there is an `#error`).

Every name must be unique in the menu, and every action is a nonzero word. A label is at most
7 characters. At 14 units a character, a longer label would leave less than the pen's
aperture between it and its neighbor. There are at most `PMCOLS` (8) items. A sublist's row
ends in `>` by convention only; the library does not check it.

```
#define PM_BAR(L, B) \
    L(draw, "Draw", DRAW) \
    L(edit, "Edit", EDIT) \
    B(text, "Text", ACTEXT)
#define DRAW(R, S) \
    R(line, "Line", ACLINE) \
    S(shape, "Shape >", SHAPE)
#define SHAPE(R, S) \
    R(square, "Square", ACSQUARE) \
    R(tri, "Tri", ACTRI)
#define EDIT(R, S) \
    R(move, "Move", ACMOVE)
#define PM_RETURN myRest:*
```

`PM_RETURN` names the program's word where its frame goes on after the menu, as a full
address. `penmenulist.ac` refuses to assemble without it.

## Including it

```
#include <memory.ah>
#include <TYPE340/type340defs.ah>
#define PM_BAR(L, B) ...            // the declaration, and any override
#define PM_RETURN myRest:*
#include <PENMENU/penmenu.ah>
...
bank 2
#include <PENMENU/penmenu.ac>       // any bank
    constants
...
bank 1                              // bank 0 or 1 only
myFrame,
#include <PENMENU/penmenulist.ac>
myRest, ...                         // the rest of the frame
```

The list must be in bank 0 or 1, because the 340's jumps reach only those banks. The frame
enters the list at `pmBar` in parameter mode: either the list is included where the frame
stands, as above, or the frame jumps to it with `jump pmBar:N next(parameter)`. The list
leaves by a jump to `PM_RETURN` in subroutine mode, with the scale and intensity changed, so
the words there must set their own. Each file has a guard. Every global label starts with
`pm`, and every macro but the guards with `PM`, `PUI` or `pm`.

## The contract

- **Calls.** Every entry is a `jda` entry, entered with the extend window open. AC holds the
  clock in milliseconds and IO holds the argument. The entry returns its answer in AC, with
  the window open. From another bank, call through `farjda(pmHit:*)` or
  `crossBankjda(pmHit:*, NOEEM)` (`memory.ah`).
- **The clock** is the BBN clock's `rck`. It counts 0 to 59999 ms and wraps, and the library
  takes every difference modulo `PUI_WRAP`. Enable the clock first (`lac [clsena]`, `cls`).
- **The pen.** The program reads each hit itself and passes the `drc` word to `pmHit`
  unchanged, in IO. A program that also has other pen targets (a drawing, a keyboard) decides
  which hits are the menu's. The menu ignores hits outside its bar and its open list.
- **Not reentrant.** Call it from the main loop only.
- **Literals** go in the pool of the code's bank, which the program places.
- **`pmInit` before the display.** The list's labels have no places until `pmInit` writes
  them, so call it before the display may reach the menu.

## The entries

| Entry | Argument (IO) | Answer (AC) | When |
|---|---|---|---|
| `pmInit` | none | 0 | Once, at the program's start. It writes every label's place, shows every label normal, opens no list, and shuts the box. |
| `pmHit` | the `drc` word | the action, or 0 | For each light-pen hit the program gives the menu. The answer is a button's action, or the action of the row OK chose. 0 means nothing was acted on. |
| `pmTick` | none | 0 | On each pass of the main loop. It closes a list that has had no menu hit for `PMIDLEMS`. |

`pmOpen` is a word, not an entry. It is nonzero while a list is open. Read it with
`lac i [pmOpen:*]` from another bank.

**A press.** The pen reports no lift, so a press ends when `PMGAPMS` passes with no hit.

**The bar.** A bar item fires at a press's first hit, or when the pen comes to it from another
column. A pen resting on a label fires it once. With a list open, another column's item closes
that list first. The list's own item, at a press's first hit, closes it with nothing acted on.
A button answers its action. An item that owns a list opens it.

**Rows.** A hit on a row lights it, and the light stays when the pen leaves. A lit row that
owns a sublist opens the sublist, with the sublist's first row level with it. A sublist's row
lights the same way. The box stands beside the lit row of the list, or of the open sublist. A
row that owns a sublist is never acted on itself.

**OK** acts only at a press's first hit. It answers the lit row's action and closes the list.
The leader keeps hits coming as the pen slides from the row to OK, so the slide stays one press
and does not act. Without the leader (`PMLEADER` 0), a slow slide reads the blank beside a
short label as a lift, and OK acts. Any other hit below the bar does nothing.

## The overrides

Define any of these before the include. The defaults are Sketchpad's.

| Constant | Default | What |
|---|---|---|
| `PMMARGIN` | 0d15 | A label's indent in its column. |
| `PMBARY` | 0d1000 | The bar's baseline. Every hit at or above `PMBARY - 8` is on the bar. |
| `PMPITCH` | 0d36 | The rows' pitch. The first row is one pitch below the bar. |
| `PMSLOP` | 0d9 | How far below its baseline a row's band starts. |
| `PMSUBDX` | 0d144 | A sublist's offset right of its list's column. |
| `PMBOXDX` | 0d144 | The box's offset right of its list's column (or its sublist's). |
| `PMBOXW` | 0d32 | The box's side. |
| `PMLEADER` | 1 | 1 for the leader, 0 for none. |
| `PMGAPMS` | 0d250 | The blank, in ms, that ends a press. |
| `PMIDLEMS` | 0d4000 | The idle time, in ms, that closes a list. |
| `PMNORMI`, `PMLITI`, `PMLEADI` | 5, 7, 3 | The intensities of a label, a lit label and the leader. |
| `PUI_WRAP` | 0d60000 | The clock's modulus, shared with PENKBD. |

Columns are 128 units wide (`PMCOLS` 8, so a hit's column is x >> 7). A list's box falls off
the screen's right edge in the last column, and a sublist's box in the last two, so place
lists in the earlier columns.

## Size

Measured with the defaults, am1 v5.5:

| Part | Words |
|---|---|
| `penmenu.ac`: the code | 602 |
| its state, and the leader's lengths | 36 |
| its literals, in its bank's pool | 40 |
| its tables | 1, plus 3 for each bar item, 3 for each list and sublist, and 3 for each row |
| `penmenulist.ac`, fixed: the selector, the box, the leader and the end | 19 |
| each label, on the bar or in a list | 3, plus a word for each three codes of its text (its letters, its case shifts and its end) |
| each list and sublist's tail | 2 |

One button (`"One"`) makes 682 words in the code's bank and 24 in the list's. The demo's menu
has six items, two lists of 3 and 2 rows, and a sublist of 2 rows. It makes 49 words of
tables and a 95-word list.

## An example

A frame that shows the menu, then the program's own words. `PM_RETURN` is `rest:*`, and the
library is in bank 2.

```
#define CALL(e) crossBankjda(e:*, NOEEM)

    lac now
    CALL(pmInit)
    ...
// A hit for the menu.
    lio hw
    lac now
    CALL(pmHit)
    sza i               // skip if the menu answered an action
    jmp back
    dac action          // ACLINE, ACSQUARE, ACTEXT, ...
    ...
// Each pass of the main loop.
    lac now
    CALL(pmTick)

bank 1
frame,
#include <PENMENU/penmenulist.ac>
rest, ...               // the program's own words, setting their own scale and intensity
    lpoff halt
```

`Projects/MiscTasks/penui/uidemo.am1` is a complete program that uses both pen libraries.
