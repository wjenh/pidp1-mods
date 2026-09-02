# Unified data loader for Adventure

This is a data-driven generator that both creates definition include files for the adventure.am1 game
and loads the Type 23 drum with data such as the text of messages and room desciptions.

**Status (01-Sep-26): complete and live -- this is THE loader.**
`adventure.adv` in this directory is now THE single hand-edited corpus
(the old three-source set plus `convert_legacy.py` are retired), and
verb/action emission is live, not future work.

One run of `advdataloader -o <dir> adventure.adv` emits the whole
generated include set and writes the intermediate track image
`advtracks.drm`; `advdrumloader` then loads that onto the drum. The
project `Makefile` runs both, with `-o Includes`.

**Output shape changed:** the `#define`s that Phase 1/2 emitted as four
separate headers (`adv_msgtab.ah`, `adv_roomtab.ah`, `adv_objdefs.ah`,
`adv_drumlayout.ah`) are now collapsed into ONE file,
`adv_defines.ah`. The table *bodies* stay separate: `adv_msgtab.ac`,
`adv_verbtab.ac`, `adv_surfacebitmap.ac`, and the six `adv_obj*.ac`
object tables. The "Artifacts" section and the per-section examples
below still describe the older split -- read them for the record
formats, not for the file names.

Phase history, for reference: Phase 1 (SPEC-PHASE1.md +
GOLDEN-MASTER-RESULTS.md) reproduced the legacy AdvTextLoader/
AdvRoomLoader pair byte-identically; Phase 2 (SPEC-PHASE2.md +
PHASE2-RESULTS.md) added object tables and constants on a
binary-identical rail. Both phase docs predate the collapse above.
Where this README's original design text disagrees with the specs, the
specs win -- except on output file names, where this note wins.

## The grammar

An input file consists of seven sections, in order (every section after
messages may be empty/omitted):
- messages, defines the text for the in-game messages shown the user
- movement, defines the words that perform movement, e.g. NORTH
- flags, defines various state flags used in the program, e.g. treasureInRoom
- actions, defines the actions associated with verbs used in the game
- rooms, defines rooms, their names, attributes, and exit points.
- objects, defines the objects used in the game, e.g., the lamp
- verbs, defines the action words used in the game and their actions, e.g. TAKE

Numbers use am1 conventions: bare integers are OCTAL, 0d is decimal, 0x is hex.

## Artifacts

Phase 1 emits the two legacy include files byte-identically to the old
AdvTextLoader/AdvRoomLoader pair (that is the golden-master rail):
- adv_msgtab.ah, message drum-location doublets
- adv_roomtab.ah, room #defines plus the ROOMTAB_*/COND_*/mask constants

The unified adv_data.ah (#defines, no code) / adv_data.ac (memory-using
data) split is the phase-2 output, not yet emitted.

The drum image (default ./pdp23drum for now; the deployed
/opt/pidp1-mods/pdp23drum path is an owner decision at retire time) is
updated in place. Text packing starts at track 18 (SAVE_TRACK 16 and
WIZCOM_TRACK 17 precede it) and expands as needed; room records follow
one track past the last text track. The -c flag compares instead of
writing: it validates everything, reads the image, and reports any
region that differs from what it would have written (the standing
golden-master check).

The input corpus, adventure.adv, is generated from the legacy sources
(Text/adventureText.txt + Rooms/adventureRooms.txt) by
convert_legacy.py; data.adv is a small smoke-test sample of the
grammar.

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

Each direction will generate a definition in the .ah include file:
```
#define DIR_name value
```

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
*what gets emitted is not yet defined*

Each verb will generate a definition in the .ah include file:
```
#define VERB_name generated-numeric-id
```
