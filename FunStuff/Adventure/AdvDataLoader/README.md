# Unified data loader for Adventure

This is a data-driven generator that both creates definition include files for the adventure.am1 game
and loads the Type 23 drum with data such as the text of messages and room desciptions.

**Complete and live -- this is THE loader**, and `adventure.adv` in this
directory is THE single hand-edited corpus. One run of
`advdataloader -o <dir> adventure.adv` emits the whole generated include
set and writes the intermediate track image `advtracks.drm`;
`advdrumloader` then loads that onto the drum. The project `Makefile`
runs both, with `-o Includes`.

The build records for the two phases that got it here were moved to
`../CompletedTasks/AdvDataLoader-Phase2/` on 04-Sep-26. They describe a
retired world (legacy loader pair, `convert_legacy.py`, four separate
`#define` headers) -- read them for the record only, never as a
procedure. A few per-section examples below still name the old
`adv_*.ah` split; read those for the record *formats*, not for the file
names, and see "Artifacts" for what is actually emitted.

## The grammar

An input file consists of eight sections, in order (every section after
messages may be empty/omitted):
- messages, defines the text for the in-game messages shown the user
- movement, defines the words that perform movement, e.g. NORTH
- flags, defines various state flags used in the program, e.g. treasureInRoom
- actions, defines the actions associated with verbs used in the game
- rooms, defines rooms, their names, attributes, and exit points.
- objects, defines the objects used in the game, e.g., the lamp
- aliases, gives an object a second (third, fourth) name, e.g. LANTERN
- verbs, defines the action words used in the game and their actions, e.g. TAKE

Numbers use am1 conventions: bare integers are OCTAL, 0d is decimal, 0x is hex.

## Artifacts

`adventure.adv` is THE corpus and is hand-edited. Nothing generates it:
the legacy `Text/adventureText.txt` + `Rooms/adventureRooms.txt` pair and
the `convert_legacy.py` that once produced it from them were retired
01-Sep-26.

advdataloader emits one #define header plus eleven table bodies, all into
the directory named by `-o` (this tree uses `-o Includes`):

- `adv_defines.ah` -- every #define: message, room, object and
  drum-layout constants, NROOMS, ROOMTAB_*, MAXEXITS, COND_*,
  OBJ_*/NOBJS. This one file replaced the old four-header set
  (adv_msgtab.ah, adv_roomtab.ah, adv_objdefs.ah, adv_drumlayout.ah).
- `adv_msgtab.ac`, `adv_verbtab.ac`, `adv_surfacebitmap.ac`,
  `adv_dwarfbitmap.ac`, and the seven `adv_obj*.ac` object tables
  (`objalias`, `objheremsg`, `objinvmsg`, `objloc`, `objnames`,
  `objtake`, `objtreasure`).

Never hand-edit a generated file.

It also writes the intermediate track image `advtracks.drm` (relative to
cwd). That image is not the drum: `advdrumloader` blits it onto the live
drum image in a second step, and restamps the SAVE/WIZCOM reserved block.
`advdrumloader -i <path>` selects the image; with no `-i` it defaults to
/opt/pidp1-mods/pdp23drum.

Drum layout: SAVE and WIZCOM share **track 16** in fixed 512-word blocks
-- SAVE at word 0, WIZCOM at word 512 (`WIZCOM_BASE_OFFSET`). Message
text starts right after them at word 1024 of that same track and flows
onto later tracks; room records follow one track past the last text
track. There is no separate WIZCOM track, and a test or tool that
invents one silently writes where the game never reads -- see
`Adventure/TESTING.md`, "Test-side addressing of that record", where that
constant drifted twice. Derive the address as SAVE_TRACK +
WIZCOM_BASE_OFFSET.

The `-c` flag compares instead of writing: it validates everything, reads
the image, and reports any region that differs from what it would have
written.

**IMPORTANT** - this data is read-only as far as the game program is concerned, it cannot
be modified by in-game actions.
If there is a need to change any attributes during play, the program must keep its own state to indicate
the change.

## Messages

The syntax is:
```
message *name*
line 1 of text
..`
line n of text
@
```
This loads the message to the drum, records its location, and generates an entry (phase 1: in
*adv_msgtab.ah*, legacy format) of the form:
```
msg_*name*, 0dnnn
    0dmmm
```
where 0dnnn is the combined track and track offset as used by the drum command dia and 0dmmm is the
packed (padCount<<12)|wordCount value.

The text is packed 3 characters per word in DEC-SIXBIT style ((ascii-32)&077), the same format
advtextloader produces and adventure.am1's txtsix decodes; '\n' packs as sixbit 077 ('_' is
reserved as its stand-in and is an error in source text).

An escape sequence \n, the same as used in C programs, inserts a newline.\
Multiple lines are automatically joined by a space *unless* a newline escape is the last character in a line,
in which case no space is inserted.

Example:
```
message FAREWELL
GOODBYE.\nTHANKS FOR PLAYING.
@
```

## Movement

This defines the possible directions a player can move in the game.

The syntax is:
```
direction *name* value *integer*
```
where:
- name is the direction's name as will be recogized when the user types it
- value is an arbitrary integer for use in the program

It does NOT generate anything. The direction list is validated against
`advdataloader.c`'s hard-coded `dirTable`, and the `#define DIR_name` it
looks as though it would emit is hand-written in `adventure.am1`
instead:
```
#define DIR_name value
```
So a direction lives in three files and all three must agree.
`addDirection()` compares the first two; nothing in the build reads the
third, and `adventure_fr2_motion_vocabulary_test.py` compares all three.

**A direction's value must fit `MAX_EXIT_DIRCODE` to be usable in a
room's `exit` row.** `buildRoomRecord()` packs the code into the exit
entry's seven-bit direction field (`dirCode << EXIT_DIR_SHIFT`, read back
by `adventure.am1`'s `GET_DIRECTION_CODE`), and `MAX_EXIT_DIRCODE` *is*
that field's mask, 127. `newRoomExit()` rejects a larger code rather than
truncating it -- a truncated code is a valid-looking exit in the wrong
direction, which the room table cannot show.

127 covers `adven.dat`'s own motion numbering (75 distinct codes, highest
77) with room to spare, so nothing in the corpus is out of reach. It was
31 between the 04-Sep-26 and 05-Sep-26 cuts, when the entry was three
words with a five-bit field; see
`CompletedTasks/TASK-FR2-MOTION-WORDS-AND-EXITS.md`'s 05-Sep-26 addendum
for the two-word entry and why the record stayed 64 words.

## Flags

This defines flags that can be used in multiple places.
A flag is effectively a manifest constant, a named value.
The meaning of the value is determined by the program.

The syntax is:
```
flag *name* value *integer*
```
where:
- name is the flag's name
- value is an arbitrary integer for use in the program

Each flag will generate a definition in the .ah include file:
```
#define FLAG_name value
```

## Action

This defines actions that can be used.
An action is similar to a *flag*, effectively a manifest constant, a named value.
The meaning of the value is determined by the program.

The syntax is:
```
action *name* value *integer*
```
where:
- name is the actions's name
- value is an arbitrary integer for use in the program

Each action will generate a definition in the .ah include file:
```
#define ACT_name value
```

## Objects and attributes

This defines the objects in the game that are interacted with, such as the lamp, and can be fixed
or carriable.

The syntax is:
```
object *name* room *roomname* placed *msgname* carried *msgname* attribute-list
```
where:
- name is the object's name
- roomname is the name of the room it is in, from rooms below
- placed msgname is the message name to use when the item has not been picked up
- carried msgname is the message name to use when being carried
- attribute-list is zero or more of *flagname* and a value of *yes* or *no*

The *roomname* can have the special name *%nowhere* to indicate it has no initial placement.

Example:
```
object lamp room well placed lampmsg1 carried lampmsg2 treasure no fixed no
```
*what gets emitted is not yet defined*

Each obj will generate a definition in the .ah include file:
```
#define OBJ_name generated-numeric-id
```

## Object aliases (added 03-Sep-26)

`adven.dat` gives most objects more than one word -- the lamp is LAMP,
LANTE and HEADL. An `object` row carries exactly one `name voc_*`, and
`findObj` walks `objNames`, `objLoc` and `objTake` in lockstep on a
single index, so a second name cannot be added to `objNames` without
breaking that correspondence. The aliases section is the second table
that solves it.

The syntax is:
```
alias voc *word* object *OBJNAME*
```
where:
- word is the vocabulary word, written the way a `voc_*` symbol is (the
  `voc` keyword is what puts the lexer in raw-word mode, which is
  required: several alias words -- `box`, `key`, `nest` -- collide with
  loader keywords and would otherwise tokenize as grammar).
- OBJNAME is an object defined in the objects section above. It must
  already be defined; a forward reference is an error.

Example:
```
alias voc lante object LAMP
alias voc headl object LAMP
```

Emits `Includes/adv_objalias.ac`, two words per row (the `voc_*` string
pointer and the `OBJ_*` index), plus `#define NOBJALIAS <count>` in the
.ah include file. The rows are tagged for **bank 3**, which is where
`adventure.am1` includes them and where their `voc_*` strings live --
bank 2 is full. Changing that means changing the tag in
`emitObjAlias()`, not just the `#include`.

`findObj` consults the table only after `objNames` misses
(`foAliasScan`), and on a hit re-derives `objLoc`/`objTake` from their
bases, so an aliased word behaves identically to the object's own name
from that point on.

## Verbs

This defines the verbs in the game that can used, such as TAKE.

The syntax is:
```
verb *name* direction *name|%none* *flag-list*
```
where:
- name is the verb's name
- direction is the name of the direction associated with this verb, or *%none* if there is no direction
- flag-list is a list of zero or more flag names, their values are or'd together to give a final value

Example:
```
verb up direction up 
```

**The live syntax is the one the corpus uses**, not the sketch above --
`verb NAME voc WORD [bank N] <argspec> handler HANDLER`, documented in
`adventure.adv`'s own verbs-section header comment.

### The optional `bank N` clause (added 04-Sep-26, TASK-FR2 step 1)

```
verb ROAD voc road bank 3 move ROAD handler doMove
```

`N` is the memory bank the row's `voc_*` string is defined in, and it
becomes the emitted reference's tag (`voc_road:3`). **Omitted, it means
bank 2**, which is where every vocabulary word lived until bank 2 filled
up, so no row that predates the clause changed.

It is not cosmetic. am1 resolves `voc_<word>:<n>` against bank `n`
whatever bank the label is really in, so a mis-tagged row assembles
cleanly and then never matches. If a `voc_*` string moves banks, its verb
row's clause moves with it. The object-alias table has the same problem
and still solves it the older way -- `emitObjAlias()` hardcodes `:3`.
*what gets emitted is not yet defined*

Each verb will generate a definition in the .ah include file:
```
#define VERB_name generated-numeric-id
```
