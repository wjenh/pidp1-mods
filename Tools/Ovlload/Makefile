CC=gcc -O2
#CC=gcc -O0 -g

# ovlload compiles ad1's debugger link client and fastload's tape reader straight
# in, as fastload does, so that no object file is left in either directory.
# ovlfile.c, the .ovl reader, is compiled in the same way by drumlist.
LINKDIR=../AD1
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
ovlload: ovlload.c ovlfile.c ovlfile.h ovlwriter.h $(AM1DIR)/tapeimage.c $(AM1DIR)/tapeimage.h $(AM1DIR)/xldr.h $(AM1DIR)/font5x7.h $(LINKDIR)/ad1link.c $(LINKDIR)/ad1link.h $(LINKDIR)/ad1proto.h
	$(CC) -I$(LINKDIR) ovlload.c ovlfile.c $(AM1DIR)/tapeimage.c $(LINKDIR)/ad1link.c -o ovlload

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
