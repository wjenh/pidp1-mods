CC=gcc -O2
#CC=gcc -O0 -g

# ovlload compiles the shared link client, tape reader and .ovl reader from UTILLIBS straight
# in, as fastload and drumlist do, so that no object file is left there; the link's wire format
# is the emulator's ad1proto.h, in PDP1DIR. AM1DIR holds am1's loader and font, which -p punches.
UTILLIBS=../UtilLibs
PDP1DIR=../../src/blincolnlights/pdp1
AM1DIR=../AM1

# The writer target only: the am1 that assembles ovlwriter.am1, and the includes it uses.
AM1=am1
#AM1INC=$(abspath ../../Am1Includes)
WRITERTMP=writer.tmp

all: ovlload

install: ovlload
	cp ovlload ../../bin
	strip ../../bin/ovlload

# ovlwriter.h is checked in, so building ovlload needs no am1; it has no rule of its own,
# and only the writer target makes it again.
ovlload: ovlload.c ovlwriter.h $(UTILLIBS)/ovlfile.c $(UTILLIBS)/ovlfile.h $(UTILLIBS)/tapeimage.c $(UTILLIBS)/tapeimage.h $(UTILLIBS)/ad1link.c $(UTILLIBS)/ad1link.h $(PDP1DIR)/ad1proto.h $(AM1DIR)/xldr.h $(AM1DIR)/font5x7.h
	$(CC) -I$(UTILLIBS) -I$(PDP1DIR) ovlload.c $(UTILLIBS)/ovlfile.c $(UTILLIBS)/tapeimage.c $(UTILLIBS)/ad1link.c -o ovlload

mkwriter: mkwriter.c
	$(CC) -Wall -Wextra mkwriter.c -o mkwriter

# ovlwriter.h from ovlwriter.am1: two -T dumps, at origins 0 and 01000, and the first's
# symbol table, which mkwriter compares and reads.
writer: mkwriter ovlwriter.am1
	rm -rf $(WRITERTMP)
	mkdir -p $(WRITERTMP)/first $(WRITERTMP)/second $(WRITERTMP)/sym
	cd $(WRITERTMP)/first && $(AM1) -T -DOVW_ORIGIN=0  ../../ovlwriter.am1
	cd $(WRITERTMP)/second && $(AM1) -T -DOVW_ORIGIN=01000 ../../ovlwriter.am1
	cd $(WRITERTMP)/sym && $(AM1) -r -s -DOVW_ORIGIN=0 ../../ovlwriter.am1
	./mkwriter $(WRITERTMP)/first/ovlwriter.dmp $(WRITERTMP)/second/ovlwriter.dmp $(WRITERTMP)/sym/ovlwriter.sym ovlwriter.h
	rm -rf $(WRITERTMP)

clean:
	@rm -rf ovlload mkwriter *.o $(WRITERTMP) 2>/dev/null || true

.PHONY: all install writer clean
