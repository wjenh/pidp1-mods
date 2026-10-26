AM1 = am1
AM1FLAGS =

all: eliza.rim

eliza.rim: eliza.am1 eliza.ah bcd.ac change.ac dcsio.ac engine.ac hash.ac lists.ac script.ac \
        scripts.ac tape.ac ttyio.ac ttytab.ac writeback.ac
	$(AM1) $(AM1FLAGS) eliza.am1

clean:
	rm -f eliza.rim >/dev/null 2>&1
