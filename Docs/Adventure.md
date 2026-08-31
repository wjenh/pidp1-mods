# Adventure for the PDP-1!

This is version 1.0\
31-Aug-2026 Bill Ezell - add wizard mode and advsave descriptions

Yes, the real deal, a full implementation of the classic Woods 1977 PDP-10 Fortran Adventure 350,\
including wizard mode, cave closing hours, *everything*.

## Background

The original game was written by Will Crowther at BBN in 1976 and called *Collosal Cave Adventure*.
In 1977 it was massively expanded by Don Woods, which is what is now the iconic version, generally known
as *Adventure 350* because that's the maximum score you can achieve.
They were written in Fortran(!) on a PDP-10.

The game is a true piece of gaming history, the foundation of interactive fiction, role-playing games, and
of course the successors like Zork and all of the other Infocom games.

So why a PDP-1 port? The PDP-1 was obsolete by the time this game came out. Well, just because.
It's a retrofit of a historical game onto what was a retro historical computer at the time.

## The strategy

The goal was an authentic, complete replication, but not by just brute force translating Fortran.
Instead, the original program served as the authoritative source for the behavior, and the PDP-1 version
written in PDP-1 assembler.

It would have been a huge tape if everything had been kept memory resident, so instead the Type 23 drum is
used to store the message text, the room properties, etc. as well as the save game slot.

It grew to use 3 memory banks and 3 drum tracks, far larger than originally estimated.

## Is it really complete?

Yes, other than the fact that it's not running on a PDP-10 with an actual operating system.
The gameplay is authentic and complete, with wizard mode, closing hours, save/restore anti-spoofing,
everything.

All of the game mechanics, items, rooms, everything, was verified by using that newfangled AI stuff, Claude Fable,
to completly analyze the Fortran and its associated dat file and compare that against the PDP-1 implementation.

It even uses the exact challenge/response generation logic of the original that used 20 bit math,
a response generator from the PDP-10 era would still create a valid response string!

## So how can I run it?

Only one way, the pidp1-mods version of the pidp-1.

Why? Because it needs the Type 23 drum, the DCS communications system (more on that later), and since the
full game requires a time-of-day clock for the closing hours and such, the Chrono-Log 20,000 clock.
These were all also written by me, and not ported elsewhere.

It also uses a few of the basic and quite useful PDP1-D extensions, liai, lai, and szi.

It is also written in am1, the new assembler for the PDP-1, because of its intrinsic multi-bank support as well
as just being a better assembler.
However, the code it generates will load and run on any -1 that has the proper 'hardware' IOTs.

## Yes, but how do I run it?

Easy. Type 'make' in its directory. That will build the program and load the drum with the necessary data.\
Load the tape. Be patient, it's big, remember?\
If you get tired of waiting, you can use the *fastload* utility, see *UsingAM1.md*.\
When it's loaded, telnet to port 2030 and relive history.

If you quit and leave it running, you can telnet back in, restore your game or start a new one.
The game is saved on the drum, so you can load other programs, come back to Adventure later and
keep going.

If the drum tracks get overwritten, you can reload them without rebuilding everything.
In the Adventure directory, type 'make drum' and it will copy the data to the drum again.

**WARNING** an install or a reload will wipe any existing adventure save game on the drum.
If you have one, save it with *advsave*, see below.

Why telnet? Because you'd go insane using the Soroban. Been there, done that. No thanks.\
And this brings us to DCS.
It uses my implementation, DCS2, which while it has all the standard IOTs of the original
DCS, it adds socket support and telnet emulation.
That's nonstandard, so you need mine.

## Wizard access

The original PDP-10 version had a wizard mode to allow setting of what hours the game could be played,
a holiday schedule, a new magic word, and some other bits.
It required solving a cryptographic challenge/response to access it using a PDP-10 program named wz.

It's fullly replicated.
The wz command is run to generate the response to the challenge, just like the original.

The full procedure is:
- Start a fresh game, it can only be done as the very first thing.
- At the "Would you like instructions?" prompt, type: NO
- As your very first command, type: MAGIC MODE
- At the "Are you a wizard?" prompt, type: YES
- At the "Password:" prompt, type: DWARF
- When asked if you know what it was, type: NO
- Solve the challenge provided by the game using *wz* and type the response.

If the cave is closed, you will be allowed to play.
Immediately repeat the above if you want to change the hours and such.

If the cave is open, you will enter the configuration setting mode initially.

## Saving a save

The utility program *advsave* can be used to save a copy of your saved game and of the wizcom configuration
to a file.
You can then restore it if the drum data is ever overwritten.

This is vaguely similar to saving your core image on the PDP-10.

## What else is there?

You can set sense switch 6 on to disable the wizard challenge/response.\
This also gets you into the cave even when closed.

You can set sense swith 5 on to disable closing time, save/restore anti-spoofing,
anything related to the time.

In fact, with ss5 and ss6 on, the Chrono-Log IOT won't be called so you won't need that,
although it is part of the standard pidp1-mods distribution.

Just be sure to turn the switches on before loading.

There's also a brand-new Easter egg, yours to discover.
Don't cheat by looking at the sources.

## Where do I get it?

It's included in the https://github.com/wjenh/pidp1-mods.git repository, check it out, look in FunStuff/Adventure.

## How does it work?

The original Fortran dat file was completely parsed to get all of the room, message, etc. information.
That, in conjunction with looking at the Fortran code, resulted in a definition file that is used by *advdataloader*.
That produces image data for what is stored on the drum, all of the messages, rooms, verbs, everything that is
static.
It also creates include files for the program that define the locations of all of the above.

A second program, *advdrumloader*, actually writes the image data to the Type 23 drum.
This allows the game to be reloaded if the drum is used for something else without rebuilding everything.

There are two non-static areas used on the drum, a single save game slot like the original, and the data
that can be set in wizard mode.
If the drum is overwritten, you will lose both of those.

The adventure program itself has a huge amount of logic derived from the original to handle all of the non-static
game flow.

The end result, a massive 4 memory bank program.

## Wait, you said it was exactly the same but...

It's exactly the same *as far as a player can see*.

The original had 140 rooms.  This one has fewer.  So how is that the same?\
Easy, there were rooms defined in the original that were never hooked up to anything, they're unreachable
and so not included here.

There were some words that were never used for anything, those have been left out.

But everything a player would ever see or do is in it. I hope. Claude insists everything's there, and Claude
is an AI, so he must be right.

## Licensing and all that

None. It's free to use. After all, it came from someone else's Fortran, however indirectly.
When you play, tip your hat to Crowther and Woods.

All I ask is that you credit me and keep my comments in the code.
This turned into a huge obsession and consumed many hours of my time and many baby seals powering Claude.

By the way, love it or hate it, this would not have happend without Claude to deal with all the research, validation,
etc.
Credit where credit is due.

Bill Ezell, wje\
August 2026
