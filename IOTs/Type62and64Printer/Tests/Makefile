AM1     = am1

# All test programs
AM1_SRCS = lpttest62.am1 lpttest64.am1

AM1_RIMS = $(AM1_SRCS:.am1=.rim)

all: $(AM1_RIMS)

%.rim: %.am1
	$(AM1) $<

clean:
	@rm -f $(AM1_RIMS) >/dev/null 2>&1
