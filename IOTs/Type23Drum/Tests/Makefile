AM1 = am1

# All test programs
AM1_SRCS = drumtest.am1

AM1_RIMS = $(AM1_SRCS:.am1=.rim)

all: $(AM1_RIMS) drumdiag.rim

drumdiag.rim: drumdiag.mac
	macro1 drumdiag.mac

%.rim: %.am1
	$(AM1) $<

clean:
	@rm -f $(AM1_RIMS) drumdiag.rim drumdiag.lst >/dev/null 2>&1
