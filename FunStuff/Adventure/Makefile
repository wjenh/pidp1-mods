all: adventure.rim
	cd $(RINGDIR) && $(MAKE) install

GENDIR = AdvDataLoader
RINGDIR = DecoderRing
DRUM   = /opt/pidp1-mods/pdp23drum

# -S makes am1 print its per-bank "highest address used" summary, which
# bank0check.sh then holds against bank 0's REAL ceiling of 07750 -- am1
# itself only complains at 4096, twenty-four words into the read-in
# loader it is about to be overwritten by.  See bank0check.sh's own
# header and CompletedTasks/TASK-BANK0-SPACE-RECLAMATION.md (B-5).
#
# tee, not a plain redirect, so the summary still reaches the terminal.
# A pipeline's exit status is tee's, so an am1 failure is caught on the
# next line instead -- bank0check.sh fails when it finds no bank-0 line,
# which is what a failed am1 (or one too old to know -S) leaves behind.
adventure.rim: adventure.am1 adv_defines.ah
	am1 -S adventure.am1 | tee bank-usage.txt
	./bank0check.sh bank-usage.txt
	@rm -f bank-usage.txt

# One rule regenerates the whole generated-include set + the track data; the
# other ten outputs ride along with adv_msgtab.ah (single generator run).
adv_defines.ah: $(GENDIR)/adventure.adv loaders
	@mkdir Includes 2>/dev/null || true
	$(GENDIR)/advdataloader -o Includes $(GENDIR)/adventure.adv
	./advdrumloader advtracks.drm

loaders:
	cd $(GENDIR) && $(MAKE) install

clean:
	@rm -f adventure.rim advdataloader advdrumloader advsave wz \
	    adv_msgtab.ah adv_roomtab.ah adv_drumlayout.ah adv_objdefs.ah \
	    *.ac advtracks.drm bank-usage.txt 2>/dev/null || true
	@rm -rf Includes advtracks.drm 2>/dev/null || true
	cd $(GENDIR) && $(MAKE) clean
	cd $(RINGDIR) && $(MAKE) clean
