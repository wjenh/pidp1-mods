# Makefile for DCS2 diagnostic programs.
#
# Targets:
#   all        -- build dcstest.rim and dcsecho (default)
#   dcstest    -- assemble dcstest.am1 only
#   dcsecho    -- compile dcsecho.c only
#   clean      -- remove all generated files
#
# am1 -d generates the binary (.rim), a listing (.lst), and a symbol table (.sym).

CC      = gcc
CFLAGS  = -Wall -Wextra
AM1     = am1

.PHONY: all dcstest dcsecho clean

all: dcstest.rim dcsecho

# ---- PDP-1 assembler target ------------------------------------------------

dcstest: dcstest.rim

dcstest.rim: dcstest.am1
	$(AM1) $<

# ---- C echo server target --------------------------------------------------

dcsecho: dcsecho.c
	$(CC) $(CFLAGS) -o $@ $<

# ---- clean -----------------------------------------------------------------

clean:
	@rm -f dcstest.rim dcsecho
