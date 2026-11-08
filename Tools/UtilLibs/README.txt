Some useful C functions for use in C code

ad1protoh.h IS A COPY of the one in src/blincolnlignts/pdp1.
It's here so a build outside of the full pidp1-mods tree will work.
Keep it in sync!

Code shared by more than one tool. Each tool's Makefile compiles what it uses from here straight
in, so nothing here needs building first and no object file is left here.

ad1link.c, ad1link.h     the client side of the emulator's debugger link: ad1, fastload, ovlload.
                         The wire format is the emulator's src/blincolnlights/pdp1/ad1proto.h.
                         ad1LinkIsRemote() says whether a -h host is another machine.
tapeimage.c, tapeimage.h a bin or am1 tape read as read-in would, and sent over the link or
                         written to a core-memory file: fastload, ovlload.
ovlfile.c, ovlfile.h     an am1 .ovl file read and checked, and the drum's other tenants
                         recognized: ovlload, drumlist. The drum writer, the overwrite warnings
                         and the stamp check: ovlload, ad1's load and fastload.
flexoConversion.c        ASCII to and from Flexo (concise), as DCS2 (IOT_22) does it.
