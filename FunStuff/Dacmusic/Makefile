# The D/A converters' music. "make" builds the player's image
# (music/dacmusic.rim), the voice tools (voices/dacvoice, voices/dacdrum),
# the renderer (render/dacrender) and the six voice drums (drums/). The
# plugins, IOT 71 and IOT 72, are built in IOTs/DAC ("make install" there).
#
# A drum file is the whole Type 23 drum. Copying one to the emulator's
# pdp23drum replaces everything on it, so save your own drum first;
# UsingDAC.md says how.

DRUMS= drums/pdp23drum-standard drums/pdp23drum-flentrop drums/pdp23drum-recorded-1 \
       drums/pdp23drum-recorded-2 drums/pdp23drum-hammond drums/pdp23drum-bagpipe

all: programs $(DRUMS)

programs:
	$(MAKE) -C music
	$(MAKE) -C voices
	$(MAKE) -C render

voices/dacdrum:
	$(MAKE) -C voices dacdrum

drums/pdp23drum-standard: voices/dacdrum voices/lib/standard.lib voices/lib/*.voice
	@mkdir -p drums
	voices/dacdrum voices/lib/standard.lib $@

drums/pdp23drum-flentrop: voices/dacdrum voices/lib/flentrop.lib voices/lib/*.voice voices/lib/flentrop/*.voice
	@mkdir -p drums
	voices/dacdrum voices/lib/flentrop.lib $@

drums/pdp23drum-recorded-1: voices/dacdrum instruments/recorded-1.lib instruments/lib/*.voice
	@mkdir -p drums
	voices/dacdrum instruments/recorded-1.lib $@

drums/pdp23drum-recorded-2: voices/dacdrum instruments/recorded-2.lib instruments/lib/*.voice
	@mkdir -p drums
	voices/dacdrum instruments/recorded-2.lib $@

drums/pdp23drum-hammond: voices/dacdrum instruments/hammond.lib instruments/lib/hammond/*.voice
	@mkdir -p drums
	voices/dacdrum instruments/hammond.lib $@

drums/pdp23drum-bagpipe: voices/dacdrum voices/lib/bagpipe.lib voices/lib/bagpipe-*.voice
	@mkdir -p drums
	voices/dacdrum voices/lib/bagpipe.lib $@

clean:
	$(MAKE) -C music clean
	$(MAKE) -C voices clean
	$(MAKE) -C render clean
	rm -rf drums

.PHONY: all programs clean
