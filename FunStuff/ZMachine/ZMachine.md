# Zork, and friends, for the PDP-1!

This is version 1.0
18-Sep-2026 - first draft

This is a Z-machine interpreter for the PDP-1.
It runs Infocom's story files, Zork I, II and III and the rest,
version 3, 4 and 5, on a machine that was designed in 1959.

## Background

Zork was written at MIT from 1977 to 1979 on a PDP-10 in MDL.
When its authors founded Infocom and wanted to sell it for the microcomputers of the day, they didn't port
the game to each machine.
Instead, they invented a virtual machine, the *Z-machine*, compiled the game for it, and
wrote a small interpreter for each computer.
The same story file ran on the Apple II, the TRS-80, the Commodore 64, the IBM PC, and a couple of dozen others.

Every Infocom game after that shipped the same way, and the Z-machine has been documented in detail
since the 1990s, down to the last opcode.

Z-machine interpreters are now widely available. Except for the PDP-1. Well, here it is.
Why? Why not? There's something that's just poetic about using a modern 2026 computer
using the latest AI technology to port a 1980s game engine to a 1959 computer.

The PDP-1 only needed this interpreter, and now you can play all of Infocom's games plus many other
independently-available games.

## Quick start, aka TLDR

Get a v3, v4, or v5 game file, Zork I, II, and III are included. Beyond Zork will work.

Type 'make' in FunStuff/ZMachine.\
Type './zloader *gamefile*'.\
Load zmachine.rim with the tape reader or use fastload, it starts at address 4.\
If you use the tape reader, it takes about 1 minute 20 seconds to load.\
If you use fastload, it takes about 1 millisecond to load. :)

MobaXterm works perfectly with no adjustment.\
Start a telnet session, connect to your pidp-1 on port 2031, say yes to the VT220 question.

For putty, select a telnet session, set VT100+ on in Terminal/Keyboard, Cascadia Mono in Window/Appearance, UTF-8 and
Use Unicode for line drawing on in Window/Translation.\
Set your pidp-1 as host, port 2031, connect and enjoy.\
This is really only necessary if you are playing v5 games, otherwise use whatever font you like.

If you're on a Windows box, you can use Windows Terminal.
Type 'telnet pidp1hostname 2031', but color won't work, say yes to the VT220 question so you get the graphics.

When the game starts, if you want all the bells and whistles, say yes to the VT220 question.

If you use some other telnet client, you might have to experiment.

Turns can take several seconds, this is a PDP-1 after all, and it's working flat-out.

Keep reading if you want to save and restore games, or want more details.

## The implementation

The Z-machine is a byte machine with 16-bit words. The PDP-1 has 18-bit words and no bytes at all.
So each 18-bit word holds two of the story's bytes, and every memory access goes through the interpreter.

A story file is far bigger than a PDP-1's 4K-word bank.
Zork I alone is 85K bytes, and its writable part is bigger than one bank all by itself.
So the story lives on the Type 23 drum.
At boot the interpreter reads the story's header off the drum, copies as much of the story as fits into
memory banks 1-12, and pages the rest in from the drum as it's needed.

The interpreter itself uses banks 0, 13, 14 and 15.

There is one interpreter image, and it runs any version 3, 4 or 5 story: it reads the story's version
when it boots and sets itself up for that version.
Changing games means putting a different story on the drum, not reassembling, even when the new
story is a different version.

## What does it run?

Version 3, 4 and 5 story files, up to 256K bytes (the size of the drum).
That is almost all of Infocom's text games:

- version 3: Zork I, II and III, Enchanter, Planetfall, Deadline, Wishbringer and the rest of the
  version 3 games;
- version 4: Trinity, A Mind Forever Voyaging, Bureaucracy, Nord and Bert;
- version 5: Sherlock, Border Zone, Beyond Zork.

When Beyond Zork asks whether your terminal is a VT220, answer YES.
Answered yes, it draws its box and map in a graphics font of its own, and the interpreter draws that font
on your terminal; see the terminal settings above.
Answered no, it draws them in plain characters.

Versions 1 and 2 (the earliest releases) and version 6 (the graphical games) are not supported;
*zloader* won't put them on the drum.

For versions 4 and 5 there is a real screen, an upper window for status lines and menus, a scrolling lower
window, cursor positioning, bold and reverse video, and italic shown as underline (a VT100 has no
italic), all sent as VT100 escape sequences.
Timed input works too, so Border Zone's clock really ticks while you think.

Text in the main window is word-wrapped at the terminal's width, as the terminal
query found it (80 columns if the terminal did not answer).
A story that turns buffering off gets the terminal's own wrap.

The status line of a version 3 game (Zork I-III, Planetfall and the rest) is the top row of the screen,
in reverse video, with the score or time at the right.
The game's text scrolls below it. A transcript does not include it.

## Where do I get games?

Zork I, II, and III are provided here, Microsoft has released them under the MIT license.

However, the other Infocom games are technically still under copyright, owned by Microsoft.
They don't seem to care that there are plenty of downloadable copies of every Infocom game, but
they aren't included here to comply with their copyrights.

Do a web search for Infocom games or Z-machine games, v3, 4, or 5.

## So how can I run it?

Only one way, the pidp1-mods version of the pidp-1.

It needs the Type 23 drum for the story, the Type 550 microtape for the saves, DCS2 for the terminal,
and the BBN timesharing clock for timed input. The line printer is used if you ask for a transcript.
All of these are part of the standard pidp1-mods distribution.

It is written in am1, and uses the PDP-1D's extended memory: up to all 16 banks, for a big story.

## Yes, but how do I run it?

In FunStuff/ZMachine, type 'make'.
That builds *zmachine.rim*, the interpreter, and *zloader*, which puts a story on the drum.

Put a story on the drum:
```
./zloader Games/zork1.z3
```

Load the ZMachine rim via the paper tape reader, or use *fastload*:
```
fastload zmachine.rim
```
with the pidp-1 running in shared memory mode, or with it shut down,
```
fastload -m zmachine.rim
```
If you used fastload the first way, it will ask you if you want it to start the program.

Otherwise, use the front panel to start at address 4.

Like Adventure, the rim tape is big, taking a little over a minute to read in via the paper tape reader.
When the excitement of twiddling your thumbs gets old, use fastload.
Real hackers of the era would have used it if they had it.

Then telnet to port 2031.
The interpreter waits for the connection, loads the story from the drum, which takes a few seconds,
and the game begins.

Use a terminal that understands VT100 or VT220 escape sequences.
Almost any terminal program does.
When you connect, the interpreter asks the terminal its size, and the game uses the whole window.
A terminal that does not answer within 2 seconds gets plain text, with the status line printed as an ordinary line.

When the game ends, or you type QUIT, or the telnet connection drops, the connection closes
and the interpreter waits for the next one, like Adventure.
Telnet to port 2031 again to play again.
Each connection starts a fresh game and the story is reloaded from the drum, so a story
installed with zloader in the meantime is the one the next player gets.
If the interpreter hits an error it cannot continue from, it says so, followed by
"Type a key, you will be disconnected and the ZMachine restarted", and the next connection
starts over.

## Saving a game

SAVE and RESTORE work the way they always did, except that you're asked for a position:
```
Position 1-9? (but keep reading)
```
Type the digit. Anything else cancels.

This is the save slot number.
The saves are on a Type 550 microtape which the interpreter expects on drive 2.

The tape must have been mounted first, either by editing the mount table in /opt/pidp1-mods
or more conveniently by using the *mtp* command.

Every story gets 9 save slots, except the three biggest.
A Mind Forever Voyaging and Beyond Zork get 8, and Trinity gets 7.

A save takes a few seconds, plus the time to move the tape to the position, up to half a minute for
the last position of the biggest stories.
Yes, the tape speed is replicated, it takes as long as it would have on a real PDP-1.

To keep a set of saves, copy the tape file.
A read-only image can be restored from but not saved to.

If a save is cut short, that position reads "Nothing saved there.", and if a tape has been damaged
a restore says "Restore failed.".
Either way the game carries on as it was, and the other positions still work.

RESTART and VERIFY work too. VERIFY checks the story on the drum against its stored checksum.
UNDO works in the version 5 games that offer it, such as Beyond Zork and Sherlock, and takes back one turn.
Version 3 and 4 games, Zork I, II and III among them, have no UNDO.

Don't change what is mounted on drive 2 while a story is running unless you want to lose it.
Your tape would replace what the Z-machine thinks is its tape and then write over whatever that tape holds.
Also unmount any mounted drive 2 tape if you want another program to use that drive.

Use one of the other drives if you keep drive 2 permanently mounted via the /opt/pidp1-mods/microtapes.txt file.

The drum also holds Adventure's data, the programs and index for rotator, selector, etc.,
and each overwrites the other.
After playing Zork, Adventure's 'make drum' puts its data back,
and after Adventure, zloader puts a ZMachine game back. Reload your favorite programs with drumloader.

You can also just copy the drum image, /opt/pidp1-mods/pdp23drum, to save it, copy it back to restore it.

## What else is there?

The story's SCRIPT command sends a transcript to the line printer, which on the pidp-1 is the file
*/tmp/ztranscript.txt*.
The printer appends, so delete the file for a fresh transcript.

The interpreter is built for the Type 62 printer.
If *pidp1.config* selects the Type 64, build with 'make LPT64=1'.

Characters above 127 that a terminal sends, accented letters in UTF-8 for instance, are ignored,
the way a real terminal line would ignore line noise.

## Where do I get it?

It's included in the https://github.com/wjenh/pidp1-mods.git repository, check out the repository
and look in FunStuff/ZMachine.

Only Zork I, II and III are included. The other Infocom games are still under copyright,
although enforcement of that seems to have been abandoned.

## How does it work?

*zloader* copies the story file to the drum, two bytes to a word, starting at track 0, and clears the
rest of the drum.
It reads only the story's header: the version, to refuse a story that isn't version 3, 4 or 5,
and the story's declared length, so any padding at the end of the file is left off the drum.

At boot the interpreter reads the story's header from the drum and works out everything else from it:
the story's version, the size of the writable memory, where the dictionary and objects are.
Then it copies the story into memory banks 1-12, as much as fits.
Zork I fits entirely; for bigger stories the rest stays on the drum and is read into an 8-page cache as
it's needed, a page at a time.
The first save or restore works out the tape's name and how many positions fit.

The versions differ in the details: a packed address is doubled in version 3 and quadrupled after it,
an object entry is 9 bytes or 14, a dictionary word is 6 letters or 9, and some opcodes change meaning.
Each of those differences is one word of the interpreter, 81 in all.
At boot, having read the story's version, the interpreter writes that version's value into each of them
from a table, nothing tests the version while the game runs, and the next player's story can be a
different version.

The interpreter proper is in bank 0, the instruction decoder and the opcode handlers.
Bank 13 has the Z-machine's stack, the page cache, and the screen code.
Bank 14 has the save code and the table of version differences.
Bank 15 has the object, text, and dictionary code.

## Licensing and all that

None. It's free to use.
All I ask is that you credit me and keep my comments in the code.
This turned into another obsession like Adventure, and another first for a PDP-1.

Zork I, II and III are Infocom's.
Microsoft, which owns them now, released them under the MIT license.
Its terms apply to them.

## Some philosophy

For all the 'purists', yes, there were modern tools used to develop this and
it uses the PDP-1D instruction set along with some IOT features that do things no actual PDP-1 did.
Fastload and zloader completely bypass the PDP-1 and directly manipulate the core image and the drum.

However, true hackers of the era would have used any of these tools and features if they had them.
They didn't sit around loading paper tapes because they enjoyed it.
How do I know this? Because I was one of them.

The PDP-1 was the beginning of hacking, and PDP-1s were hacked constantly.
The pidp1-mods general philosophy is, if it is plausible that a PDP-1 could have had a feature, then it's
fine to have that feature as long as the timing restrictions of the real hardware are honored.

The extras like fastload are present just as conveniences.
Zloader is present for practical reasons, it would take an absurdly long time to load the drum from a paper tape.
Finally, some changes bow to the reality that pidp-1 has to run in the modern world on a modern computer, such as
the DCS2 embedded telnet handler.
It, however, still supports the original DCS IOTs and functionality, the rest are new IOT subcommands.

But, the Microtape, Type 23 drum, the 1D instructions, and the BBN clock are all 100% authentic and were present
on a real PDP-1.

Adopt the true spirit of the time.

Finally, love it or hate it, this would not have happened without Claude Opus and Fable
to deal with all the research, validation, and major portions of the code generation.
Claude created its own test harness for running the pidp-1 emulator, loading the rim, connecting to the game
and playing it just as a human would, and used the ad1 debugger to track down problems, quite amazing.

The sheer volume of work and its complexity would have made it totally impractical for me to do it alone.

Credit where credit is due.

Bill Ezell, wje\
September 2026
