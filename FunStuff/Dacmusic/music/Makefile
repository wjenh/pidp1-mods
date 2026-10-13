# The player, dacmusic.rim: Samson's Harmony Compiler tapes on the D/A
# converters, with its menu on the Type 340 and the light pen. orch.ah is
# its default orchestra; dacdefs.ah, the plugins' mnemonics, is in
# Am1Includes/DAC. The include paths are relative, so the tree builds
# wherever it is.

INCROOT= ../../../Am1Includes

all: dacmusic.rim

dacmusic.rim: dacmusic.am1 menu.ac menus.ac orch.ah $(INCROOT)/DAC/dacdefs.ah
	am1 -i $(INCROOT) -I $(INCROOT)/DAC -I . dacmusic.am1

clean:
	@rm -f dacmusic.rim dacmusic.lst dacmusic.sym

.PHONY: all clean
