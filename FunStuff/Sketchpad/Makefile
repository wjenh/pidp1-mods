SHELL := /usr/bin/bash

all: sketchpad.rim sketchplot

install: all
	@echo 'Removing any old .img tapes'
	@for f in Tapes/*; do \
		r=$$(basename $$f); \
		r=$${r%.*}.img; \
		r=/opt/pidp1-mods/Microtapes/$$r; \
		if [ -f "$$r" ]; then \
			echo "removing $$r"; \
			rm "$$r"; \
		fi; \
    done
	cp Tapes/* /opt/pidp1-mods/Microtapes

sketchplot:
	cd Render; make

sketchpad.rim: sketchpad.am1
	am1 sketchpad.am1

clean:
	rm -f sketchpad.rim sketchplot >/dev/null 2>&1
	cd Render && make clean
