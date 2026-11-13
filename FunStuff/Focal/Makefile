AM1 = am1
AM1FLAGS =

all: focal.rim

focal.rim: focal.am1 focal.ah editcmd.ac expr.ac fncoef.ac fparith.ac fpconv.ac fpfunc.ac \
        interp.ac iocmd.ac lineed.ac lpt.ac fslib.ac plotter.ac ptape.ac store.ac termdata.ac \
        termio.ac ttytab.ac vars.ac
	$(AM1) $(AM1FLAGS) focal.am1

clean:
	rm -f focal.rim >/dev/null 2>&1
