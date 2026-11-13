AM1 = am1
AM1FLAGS =

SRCS = focal1c.am1 focal.ah editcmd.ac expr.ac fparith.ac fpconv.ac interp.ac iocmd.ac \
        lineed.ac ptape.ac store.ac termdata.ac termio.ac ttytab.ac vars.ac

all: focal1c.rim focal1cfn.rim

# focal1c.rim has no extended functions, leaving the most room for a program.
focal1c.rim: $(SRCS)
	$(AM1) $(AM1FLAGS) focal1c.am1

# focal1cfn.rim has FSQT to FRAN. am1 names its output after the source,
# so the source is assembled under a second name.
focal1cfn.rim: $(SRCS) fpfunc.ac fncoef.ac
	cp focal1c.am1 focal1cfn.am1
	$(AM1) $(AM1FLAGS) -DF1C_FUNCS focal1cfn.am1; s=$$?; rm -f focal1cfn.am1; exit $$s

clean:
	rm -f focal1c.rim focal1cfn.rim >/dev/null 2>&1
