Ovlload puts a program's drum overlays, the file.ovl am1 writes, on the Type 23 drum,
then loads the program's tape into the pdp-1 as fastload does.

By default, the drum image is the file '/opt/pidp1-mods/pdp23drum'.
Each overlay goes on the track am1 chose for it, at the drum addresses of its core window;
nothing else on the drum is touched. The program's stamp on its tape must match the .ovl's,
so a drum and a program from different builds are never loaded together.

A track holding another tool's data (a drumupdater program, Adventure's save, a Z-machine
story) is written all the same, and ovlload warns of it afterwards.

-c compares the drum with the .ovl instead, for a script to ask whether the drum is current.
-n loads the drum only.

-p tapefile punches a paper tape instead, for the pdp-1 to write the overlays to its own
drum from the reader: am1's loader, a small writer program, the overlays, and then the
program's own tape, unless -n is given. Read it in; the pdp-1 halts with AC 0 when the
drum is written, and continue loads the program. The writer warns of other tools' data on
the typewriter, checks each overlay's checksum (a halt with AC 1: back the tape up to that
overlay and continue), and reads each one back (a halt with AC 2: continue writes it again).
A halt with AC 3 is a drum transfer that failed 4 times; continue tries it 4 more.

ovlfile.c reads and checks the .ovl and recognizes the other tools' data; drumlist compiles
it in too. ovlload is also built with fastload's tape reader, ../AM1/tapeimage.c, and ad1's
link client, ../AD1/ad1link.c. See the usage in ovlload.c, and Docs/UsingOverlays.md.

The writer is ovlwriter.am1. Its words are in ovlwriter.h, checked in, so building ovlload
needs no am1. After a change to ovlwriter.am1, 'make writer' assembles it with am1 and
makes ovlwriter.h again with mkwriter.c; AM1= names another am1.
