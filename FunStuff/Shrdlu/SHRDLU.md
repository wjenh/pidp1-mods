# SHRDLU and how to run it

Version 1.1\
4-Nov-2026\
Minor update for color markings moving with their objects.

SHRDLU, Terry Winograd's blocks-world program (MIT, 1968-70), originally running on a PDP-6,
rewritten in am1 for the pidp1-mods PDP-1D.
It is the complete blocks world rendered on the Type 340 as the orginal was,
with hidden lines, the arm and hand, and the original's action theorems.

It accurately reproduces the actions of all the original sentences in the Winograd movie, except for real.
The original was basically faked, scripted for the camera, that is not SHRDLU running.
See the explanation in *A demonstration* below.

However, the original SHRDLU would execute those sentences if it was run.
Mostly.
There were 4 additional thesis sentences that were dropped from the movie, probably because they didn't work.
However, here they do.

Each sentence typed is read word by word as it is typed using the original's dictionary and morphology,
parsed by Winograd's grammar, and answered.
A command is carried out by the hand if motion is invoved.

The grammar, the semantics, and the answers are derived directly from Winograd's code.
The original micro-Planner is replicated, but with some improvements.
It still reproduces what the original did, but far more efficiently.

It answers all of Winograd's demo dialog's 42 sentences as well as the four addtional ones from
his thesis.

## What is here

| File or directory | What |
|---|---|
| `SHRDLU.md` | This guide. |
| `README.md` | What it says. |
| `Shrdlu/` | The PDP-1 program, assembled by `make`. |
| `Modern/` | SHRDLU in C, one Linux executable implementing the same behavior with no emulator. |
| `Player/` | `shrdlu_play`, a client that simulates a real user, sending sentences from a text file. |

## Building and running

In SHRDLU:
```
make
```
This makes *shrdlu.rim* and *shrdlu.ovl*, the program's drum overlays.
The overlays hold the compiled Lisp and micro-planner, about 40K words,
as well as all the '/' commands and support code.

*semantics.ac* and *lexicon.ac* are generated from the original's Lisp and dictionary.

Load it with its overlays using ** *pdp1central* or fastload.
Loading it via the paper tape reader is possible, but only it you want to wait a very long time, the program
is huge.
It starts at 0100.

It uses Type 340 display, DCS2, the timesharing clock, and the Type 23 drum.

When started, the initial scene appears after a few seconds.

## Talking to it

Connect a telnet client to port 2042 of the machine running pidp1:
```
telnet <host> 2042
```
SHRDLU answers with a banner, `SHRDLU: TYPE A SENTENCE, OR A COMMAND AFTER /:` and
the specal commands, then `*`.\
Each SHRDLU reply ends with a line having only `*`.\
Input is folded to upper case.\
Every number, typed or printed, is octal, as in the original.

If an output line is longer than 79 characters it wraps at the prior space so a telnet window of
80 columns is sufficient.\
`WORDS:` follows and echoes your sentence.\
The line you type is an exception, it doesn't wrap as you type it but your telnect client probably does.\
The `/` command output is also not wrapped.

A line that starts with `/` is a command.
Anything else is English that is interpreted as a sentence.

## What's different?

Externally, not much signifcant.

However, the movie and some other videos show the objects labeled with their color.
The original code did not do this, they were added in the movie as was the text supposedly on the screen.
Here, they are shown because it's much easier to identify what's what.

Internally, a lot.

The code executed is essentially the orginal, but the execution engines for lisp and micro-planner
are highly optimized.
The major change is that micro-planner had a huge problem, its backtracking algorithm was terribly
inefficient, the initial version here took 10 minutes to solve the most complex sentence in the demo set.
This was known back in the Winograd days and is why micro-planner disappeared.

The solution was to add Prolong-like cutpoints, reducing the time for that sentence down to under 1 minute.

Oh, and a few real bugs were found in the original code and fixed.
No one noticed them for 55 years?

## Some history

The importance of SHRDLU should not be underestimated, particularly now.
In the 1960's and 70's, two different AI philosophies evolved, the 'Symbolists' and the 'Connectionists'.

Symbolists, of whom Minksy, Papert, Winston, and Winograd were major figures, beleived that intelligence
could be represented by sets of rigid rules.
Write enough rules, you could have a true sapient AI, since humans worked the same way.

Connectionists thought the opposite, intelligence was cause by connections of facts, what were called
loosely *neural nets*.
Sound familiar?
They were definitely in the minority, Rosenblatt was the principal figure.

When Terry Winograd wrote SHRDLU, it stunned the world.
Here was what looked like an actual thinking machine you could converse with and have it understand.
It also turend out to be significant in the downfall of the Symbolist AI camp.
It became clear that it coud not possibly scale to the real world, there would need to be a virtually infinite
number of rules, a combinatorial explosion.

Shortly after SHRDLU was shown, a devastating report was written by Sir James Lightill in the UK, and one
of his main examples was SHRDLU.
That caused funding to disappear for Symbolist AI research, and since Minksy and Papert had killed
Connectionist pathways, the great 'AI Winter' ensued.

But there's one more plot component.
How did Minksy and Papert kill off Connectionist research?
One small book, 'Perceptrons'.
It was a carefully crafted hatchet job that mathematically proved neural nets couldn't sovle a basic problem,
they couldn't do a simple exclusive-or.
Or so it was claimed, but both of then knew full well that layered nets could and that Rosenblatt was actively
working on them.
The hatchet job was purely to keep more money flowing to the MIT AI labs, cutting Rosenblatt out.

I was at MIT while this was all happening, I started there the year after SHRDLU was revealed, so
I saw a lof the history unfold.
I still get livid when I think of Minsky.
And, I still have my copy of Perceptrons.

Bill

## Using it

### Sentences

Type a sentence, ending it with `.`, `?` or `!`.\
Return alone does not end a sentence, it ends the word, as space does, and the sentence continues on the next line.\
This also is the original behavior.\
`:` and `;` are part of the sentence.'\
A comma or hyphen is dropped, so *pick-up* is typed *pick up*.

The reply is the sentence's words as recognized by the dictionary, then SHRDLU's answer, then `*`.
A command or sentence that moves objects on the display completes before its `OK.`:
```
pick up a big red block.
WORDS: PICK UP A BIG RED BLOCK .
<thinking time, then the movement time>
OK.
*
```

The answers can be `OK.`, `I CAN'T.`, `YES.`, `NO.`, `I DON'T KNOW.`,
a count (`FOUR OF THEM.`), objects named (`THE BLUE PYRAMID AND THE BLUE BLOCK.`),
a yes with what it answe (`YES, THE RED CUBE.`),
a count with its list (`YES, THREE OF THEM: A LARGE RED ONE, A LARGE GREEN CUBE, AND THE BLUE ONE.`),
or the original question back when a phrase is ambiguous (`I DON'T UNDERSTAND WHICH PYRAMID YOU MEAN.`).
When it chooses what "it" means, it says what that means first, then answers:
```
BY "IT", I ASSUME YOU MEAN THE BLOCK WHICH IS TALLER THAN THE ONE I AM HOLDING.
OK.
```

When a sentence has two meanings and neither is clearly the likelier, SHRDLU asks which, and waits:
```
I'M NOT SURE WHAT YOU MEAN BY "ON TOP OF" IN THE PHRASE "ON TOP OF GREEN CUBES".
DO YOU MEAN:
1 - DIRECTLY ON THE SURFACE
2 - ANYWHERE ON TOP OF?
```
Type the number and Return.
Anything else responds `PLEASE TYPE ONE OF THE NUMBERS` and it waits again.

Ask when, why or how it did something, and it answers from its memory of the previous events:
```
why did you do that?
WORDS: WHY DID YOU DO THAT ?
TO CLEAN OFF THE RED CUBE.
*
```

"That" is the last event named by eiher you or SHRDLU, and "it" in "do it" the last thin you named.
"why?" alone asks about the event that was last mentioned.
A statement about the past is checked, and corrected: `NO, ONLY FOUR OF THEM: ...`.
Ask with `move`, `did you move the red cube?` or `what did you move?`;

`did you put the blue pyramid in the box?` is not understood since a place after a question's
object is considered part of the object, so it replies
`I DON'T KNOW WHAT YOU MEAN BY " THE BLUE PYRAMID IN THE BOX ".`, as the original does.

Other replies:
- `I DON'T UNDERSTAND.`: no parse with a meaning was found, `why?` with nothing done yet.
- `I CAN'T EXPLAIN A NON-EXISTENT EVENT.`: asked why it did something it never did. A yes-or-no
  question about such an event gets `NO.`
- `I DON'T KNOW WHAT ... REFERS TO`, with the pronoun quoted: "them" or "it" with nothing
  earlier to stand for.
- `I CAN'T.`: the command cannot be done, such as a stack too high for the hand.
- `NO ROOM`: the sentence was too long, or it needed more memory than is available.
  None of the dialog's 42 sentences will cause `NO ROOM`.
  **Commands can run out after it** that move something keeps some of the planner's memory,
  such as what `/ROOM` calls `EVENTS` and `DATABASE`.
  About 50 one-move commands after the dialog will fill memory, fewer if they move several things.
  Every command then replies `NO ROOM` until the program is loaded again.
  **Questions about moves cost memory also:** `did you move ...?` keeps an event for each time the
  object was moved, and `what did you move?` one for every move, so about a dozen of the second fill
  the same table after the dialog.
  `what did you move?` after the dialog has too many things to name so it replies `NO ROOM`,
  though the sentences after it work as usual.
- `FAULT` and an address: the program hit something it cannot do.
  None of the dialog's 42 gets one; a sentence of your own could.
- `OVERLAY ERROR 2`: the drum does not hold this build's overlays

`/TREE ON` also types each sentence's tree after its answer.
A unit's line is the unit, CLAUSE, NG for a noun group, VG for a verb group, PREPG, ADJG with its features,
then the words it spans.
A word's line is the word, then its features.
Each level is indented two spaces under the unit that holds it.
A sentence abandoned or one that ends the dialog has none.

**A sentence takes time.**

This is a PDP-1 after all, but since this SHRDLU has been heavily optimized, it comes surprisingly close
to the original PDP-6 times.
The most complex sentence in the original set takes a little over 1 minute to evaluate.

**An unknown word stops the sentence as soon its delimiter is typed**

```
SORRY, I DON'T KNOW THE WORD "STEEPLE".
PLEASE TYPE RETURN AND CONTINUE THE SENTENCE.
```
The word is dropped and after the Return the sentence so far without the
word (`DOES A `) is printed and you can continue typing.
The stop comes as the word is typed only if the telnet client sends each character as it is typed.
A client that sends a line at a time sends nothing until Return, but the behavior will be the same.

**A quoted word is a new word**

`a "steeple" is a stack which contains two green cubes and a
pyramid.`, SHRDLU learns STEEPLE, and from then on it is a word like any other, with its plural STEEPLES.
`are there any steeples now?` tests the scene for one, and `build one.` builds one.

`call the biggest block "superblock".` names a block, the name is a word too, kept with the
block.
What a word or a name means lasts until the program is started again.
A typed `'` is the apostrophe of *'s* and *n't*.

**Telling it what you like**:

`I like ...` and `... is nice` are fact and rule creators.
`The blue pyramid is nice.` is a fact.
`I like blocks which are not red, but I don't like anything which supports a pyramid.` makes rules
which `Do I like the box?` then uses.
Other statements get `I DON'T UNDERSTAND.`.
Each rule keeps some permananet storag, so giving many can cause `NO ROOM` in a long sentence later.

**Editing**

When the client sends character-by-character, Rubout (ASCII DEL, 0177, Putty's default) or
Backspace (Ctrl-H, 010, MobaXterm's default) erases the last character or at a word's start
erases the word before and the sentence so far is shown again on a new line.

Ctrl-U erases the whole sentence and starts a new line.

A client that sends a line at a time edits the line itself, before sending it.

### Commands

| Command | Does |
|---|---|
| `/WORD x` | The word's features and its root, derived by the morphology if need be: `TALLER:`, its features, `ROOT TALL`. `x: UNKNOWN` for a word neither found nor derived |
| `/WORD x f ...` | Gives the word these features for the rest of the run. A wrong word or feature gets `ERROR` |
| `/TREE ON`, `/TREE OFF` | While on, each sentence's tree is printed after its answer, initially off |
| `/SM` | The last sentence's meaning: each structure the semantics built, one per line, as the original Lisp did |
| `/PLAN ON`, `/PLAN OFF` | While on, each movement the planner makes for a motion commande as `/DO` prints them, before the answer, initially off. Finding a block prints `BLINK :Bn`, which draws nothing, as in the original |
| `/TIMES` | `PLAN p ALL a PARSE q ANSWER r`: the last `/DO` or `/TRY`'s planning time and its total time, the last sentence's parse with its meaning, and its answer, with any actions carried out in ms, in octal |
| `/ROOM` | How much of each memory arena has been used, each line an arena, its name, its size and the words used, in octal, then the events in it, then `DROPS n m`, the times since the heap flushed what it read from the drum and how much it flushed |
| `/ROOM CLEAR` | Typed first after a start, clears what each arena has clear so that `/ROOM` then gives the most used since. Without it, what an earlier run left in core counts as used |

The planner and hand commands, each typed after `/`.

### Goals for the planner

| Command | Does |
|---|---|
| `/DO goal` | Works out how to do the goal, prints the hand's calls one a line, and carries them out on the screen. If it cannot be done, `I CAN'T` |
| `/TRY goal` | Works out how, prints the calls and `OK` or `I CAN'T`, and does nothing |
| `/TRACE ON`, `/TRACE OFF` | While on, `/DO` and `/TRY` also print each theorem tried with its arguments and `OK` when it succeeds or `BACK` when a failure backtracks, indented by depth |
| `/EVENTS` | What the planner has done, oldest first: `E7 #RAISEHAND START 4 END 5 WHY E1`, each event's start and end and the event it was done for |
| `/HISTORY :Bn` | Where the object has been, newest first, the time, the place, and what supported it |
| `/FIND pred args` | What the planner knows, every assertion that matches, `?` matching anything. `/FIND SUPPORT ? :B7`, `/FIND COLOR ? RED`, `/FIND ?` for everything |
| `/SEED n` | Sets the random numbers that choose where things are put down |

The goals are the original's: `PICKUP :Bn`, `GRASP :Bn`, `UNGRASP`, `RAISEHAND`, `PUTON :Bn :Bm`
(or `:TABLE`), `PUTIN :Bn :BOX`, `STACKUP :Bn :Bm ...`, `CLEARTOP :Bn`, `GET-RID-OF :Bn`,
`NOTICE :Bn`, `BLINK`, which draws nothing.
A goal the planner cannot finish in its memory gets `NO ROOM`, nothing moves.

### The hand

These move the hand and objects directly.
The planner does not see what they do, so after them its idea of the world and the picture disagree until `/RESET`.

| Command | Does |
|---|---|
| `/MOVETO x y z` | Moves the hand and whatever it holds to `x y z`: up to `z = 1300`, across in `y`, across in `x`, then down |
| `/GRASP :Bn` | Puts the hand on the object's top center and grasps it. Only a block or pyramid can be grasped. Grasping while holding something drops the first object where it hangs |
| `/UNGRASP` | Lets go. The object stays where the hand put it, resting on whatever is under it |
| `/REFRESH` | Draws every object again |
| `/SCENE` | Lists the objects, as the CMU manual's listing does, and what the hand holds |
| `/STATUS` | `FRAMES f EDGES e CLAMPS c OVER o MS m`: frames drawn, edge violations, screen coordinates clamped to the raster, display words that did not fit, and the last scene rebuild in ms |
| `/RESET` | Puts the world back as it started with the hand at `40 0 0`. The planner forgets its events and starts its random numbers again. Any words learned are kept |
| `/PROJ x y z` | The screen point of a world point |

A `/` line that is not a command givs `ERROR` and the line.

Coordinates run from `0` to `1200` in each axis. `x` runs to the right, `y` away from you, and
`z` up. The table is at `z = 0`.

## What to expect

- **Hidden lines.** Each object hides the parts of the others behind it.
  The box is open at the top so what is inside shows over its front wall.
- **Grasping makes wireframes.** The held object, and every object that hid part of it, is drawn
  completely, back edges included.
  A MOVETO with a load does the same to whatever would hide the load
  where it is going. After an `/UNGRASP` the object stays a wireframe until `/REFRESH`; a
  `/DO` ends with a REFRESH.
- **Colors are intensities.** Red is 6, green 4 and blue 2; the box and the table are 7.
  You can change these in *shrdlu.ah* if you wish.
  The initial scene's colors, as `/SCENE` prints them: `:B1` red block, `:B2`
  green pyramid, `:B3` green block, `:B4` blue pyramid (in the box), `:B5` red pyramid, `:B6`
  red block, `:B7` green block, `:B10` blue block, `:BOX` white.
- **Objects are also labeled with their color.** Each will be identifed with BL, RD, GR for blue, red, and green.
  The box and the table are not labeled.
- **The scene stays on the screen while the program thinks**:  parsing, planning, and rebuilding never stop it.
- **The hand has two crossbars**, as in the film of the original's demonstration: a short one
  at the arm's top and a longer one at the hand, resting on the top of what it carries.
- **The hand moves one step a frame**. A move steps `STEPX` and `STEPY` screen points (20 and 14 octal).
  These are defined in *shrdlu.ah*.
  For slower motion set `PERIOD` greater than a frame time, about 23 msecs, and for smoother
  motion make the steps smaller.
- **Each change rebuilds the picture**, only what the change can touch is clipped again.
  This takes about 0.6 to 2.1 s after a GRASP or UNGRASP, and up to 2.9 s after a REFRESH.
- **Where things are put down is random**, as in the original, but repeatable.
  after `/RESET` the same goals give the same plans.
  `/SEED` changes them.
- **A command takes time**, typed in English or as `/DO`.
  Planning takes about half a second for a pickup; carrying it takes several seconds a move.

## A demonstration

One dirty secret of the original Winograd demo seen in the old movie?
It was completely faked.
That is not SHRDLU solving anything.
It was mocked up because no one wanted to sit through minutes of 'thinking'
before anything happened and because of limitations in making movies from a screen.
This was actually common at the time.
The text that appears to be on the screen was added in post-production, the
real program never did that.

Did that mean SHRDLU itself was a fake?
No, almost everything in the movie it could actually do.

Almost? There were a couple of sentences that seem to have been faked, the original code
wouln't handle them. This version will.
When you go through this script, SHRDLU will actually be doing what it pretended to do in the movie,
even if you use shrdlu_play, it just feeds the text to SHRDLU directly.

**The dialog.** Start the program, then type, in order, the dialog's 42 sentences from
the transcript of the original's demonstration, and `2` after 20's question.
`Player/dialog.txt` has them, and `shrdlu_play` will type them for you.
In that file the thesis's original four missing sentences, below, have been added between the
demo's 12 and 13.
```
sentence 1   OK.  (the hand puts the green cube off the big red block, then picks it up)
sentence 2   I DON'T UNDERSTAND WHICH PYRAMID YOU MEAN.
sentence 3   BY "IT", I ASSUME YOU MEAN THE BLOCK WHICH IS TALLER THAN THE ONE I AM HOLDING.
             OK.  (the hand puts the red block down, then the blue block into the box)
sentence 4   THE BLUE PYRAMID AND THE BLUE BLOCK.
sentence 5   THE BOX.
sentence 6   FOUR OF THEM.
sentence 7   YES, THE RED CUBE.
sentence 8   YES, BY THE TABLE.
sentence 9   NO.
sentence 10  YES.
sentence 11  I DON'T KNOW.
sentence 12  I CAN'T.  (nothing moves)
sentence 13  OK.  (the hand stacks the red block, the small red one and the green cube)
sentence 14  THE LARGE GREEN ONE WHICH SUPPORTS THE RED PYRAMID.
sentence 15  YES, THREE OF THEM: A LARGE RED ONE, A LARGE GREEN CUBE, AND THE BLUE ONE.
sentence 16  OK.  (the small red block onto the green cube that supports a pyramid)
sentence 17  OK.  (the green pyramid onto the small red block)
sentence 18  YES, THE GREEN PYRAMID.
sentence 19  BY "IT", I ASSUME YOU MEAN THE SHORTEST THING THE TALLEST PYRAMID'S SUPPORT SUPPORTS.
             RED.
sentence 20  the question back (see "A question back")
2            THREE OF THEM.
sentence 21  YES, THE GREEN ONE.
sentence 22  WHILE I WAS STACKING UP THE RED CUBE, A LARGE RED BLOCK AND A LARGE GREEN CUBE.
sentence 23  TO GET RID OF IT.
sentence 24  TO CLEAN OFF THE RED CUBE.
sentence 25  TO PUT IT ON A LARGE GREEN CUBE.
sentence 26  TO STACK UP THE RED CUBE, A LARGE RED BLOCK AND A LARGE GREEN CUBE.
sentence 27  BECAUSE YOU ASKED ME TO.
sentence 28  BY PUTTING A LARGE RED BLOCK ON THE TABLE; THEN LETTING GO OF IT; THEN PUTTING A
             LARGE GREEN CUBE ON IT; THEN LETTING GO OF THAT CUBE; THEN PUTTING THE RED CUBE
             ON THAT CUBE; THEN LETTING GO OF THAT CUBE.
sentence 29  FOUR OF THEM.
sentence 30  THE GREEN PYRAMID.
sentence 31  NO, ONLY FOUR OF THEM: THE RED CUBE, TWO LARGE GREEN CUBES AND A LARGE RED BLOCK.
sentence 32  OK.  (the blue pyramid onto the blue block)
sentence 33  YES, THE BLUE BLOCK.
sentence 34  the stop at STEEPLE (see "Sentences"); Return, then Ctrl-U to drop what is left
sentence 35  I UNDERSTAND.
sentence 36  NO.
sentence 37  OK.  (the hand builds the steeple)
sentence 38  OK.
sentence 39  YES.
sentence 40  BY "IT", I ASSUME YOU MEAN THE SUPERBLOCK.
             I CAN'T EXPLAIN A NON-EXISTENT EVENT.
sentence 41  YES, FOUR THINGS: THE BOX, THE RED CUBE, THE BLUE PYRAMID AND THE BLOCK NAMED
             SUPERBLOCK.
sentence 42  YOU'RE WELCOME!
```
Each answer is the transcript's line, with three small differences in this program.
20's second choice reads `ANYWHERE ON TOP OF?`, where the transcript has `ANWHERE ON TOP OF ?`,
and 19's answer is `RED.`, with a period. The whole takes about 23 minutes, most of it the
sentences' thinking (37 alone nearly 2), the rest the hand's moves, with your typing added.
`/TIMES` gives its parse and its answer times.
Some answers depend on the sentences before: typed without 4 before it,
5 gives `I DON'T UNDERSTAND WHICH PYRAMID SUPPORTED YOU MEAN.`, and typed without 7, 8
gives `I DON'T UNDERSTAND.`, as the original would.
`/PLAN ON` first shows the hand's calls for the
commands (1, 3, 13, 16, 17, 32 and 37), and `/TREE ON` each sentence's tree.

**The thesis's four sentences** are outside the 42, and come from Winograd's thesis, after 12.
Start fresh, type 1 to 12, then:
```
the blue pyramid is nice.                                                I UNDERSTAND.
i like blocks which are not red, but i don't like anything which supports a pyramid.
                                                                         I UNDERSTAND.
do i like the box?                                                       NO.
do i like anything in the box?                                           YES, TWO THINGS: THE BLUE PYRAMID AND THE BLUE BLOCK.
```

**The motion planner by itself.** The goals of the dialog's commands, in order, after a `/RESET`:
```
/DO PICKUP :B6
/DO PUTIN :B10 :BOX
/DO STACKUP :B2 :B5
/DO STACKUP :B6 :B1 :B7
/DO PUTON :B1 :B3
/DO PUTON :B2 :B1
/DO PUTON :B4 :B10
/EVENTS
```
The first picks up the big red block, putting the green one off it first. The second puts the
blue block in the box: the hand puts down the red block first. The third gets `I CAN'T`: two
pyramids cannot be stacked. The fourth stacks the red block, the small red one and the green
cube, clearing the small red block's top on the way. The last three end as the original's film
of the dialog ends. `/EVENTS` then lists all of it, each step with the step it was done for.

## A final footnote

In the full Cosmic Joke style, I'll now tell you:

This SHRDLU implementation was done by Claude Opus 5.5, an LLM that can be considered the Connectionist dream.
That it implemented the pinnacle of Sybolist AI is one layer of the joke.

The next is, maybe making this a Cosmic Cosmic Joke, is that the very combinatorial explosion that ended the Symbolists
is now present in LLMs.
LLMs as they are now can be considered the SHRDLU of the Connectionist camp; they suffer from combinatorial explosion
that is also totally non-scalable, but for a different reason, the quadratic decay of contexts as they grow.

And for the final roll-around-the-floor-laughing twist, it is beginning to look like the solution is going to be
a hybrid of the two camps.

I havent had this much fun in years!

Bill Ezell (and Claude)\
November, 2026
