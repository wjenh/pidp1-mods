# Makefile -- the PDP-1 Z-machine: builds zmachine.rim and zloader here.
#
#   make              both
#   make clean
#   make LPT64=1      the interpreter for the Type 64 line printer
#   make clean

.PHONY: all zloader zmachine.rim clean

all: zloader zmachine.rim

install: all
	sudo ln -s `pwd`/zstart /usr/local/bin/zstart
	sudo ln -s `pwd`/zloader /usr/local/bin/zloader

zloader:
	$(MAKE) -C Loader install

zmachine.rim:
	$(MAKE) -C Interpreter install

clean:
	$(MAKE) -C Loader clean
	$(MAKE) -C Interpreter clean
	@rm -f zloader zmachine.rim
	sudo rm -f /usr/local/bin/zstart
	sudo rm -f /usr/local/bin/zloader
