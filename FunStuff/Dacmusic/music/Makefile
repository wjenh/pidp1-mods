# The player, dacmusic.rim, plays Samson's Harmony Compiler tapes on the D/A
# converters, with its menu on the Type 340 and the light pen. orch.ah is
# its default orchestra; dacdefs.ah, the plugins' mnemonics, is in
# Am1Includes/DAC. The include paths are relative, so the tree builds
# wherever it is.

all: dacmusic.rim

dacmusic.rim: dacmusic.am1 menu.ac menus.ac punch.ac orch.ah
	am1 dacmusic.am1

clean:
	@rm -f dacmusic.rim dacmusic.lst dacmusic.sym

.PHONY: all clean
