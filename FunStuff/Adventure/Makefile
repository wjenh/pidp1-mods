all: adventure.rim
	cd $(RINGDIR) && $(MAKE) install

GENDIR = AdvDataLoader
RINGDIR = DecoderRing
DRUM   = /opt/pidp1-mods/pdp23drum

adventure.rim: adventure.am1 adv_defines.ah skipmacros.ah
	am1 adventure.am1

# One rule regenerates the whole generated-include set + the track data; the
# other ten outputs ride along with adv_msgtab.ah (single generator run).
adv_defines.ah: $(GENDIR)/adventure.adv loaders
	-mkdir Includes
	$(GENDIR)/advdataloader -o Includes $(GENDIR)/adventure.adv
	./advdrumloader advtracks.drm

loaders:
	cd $(GENDIR) && $(MAKE) install

clean:
	@rm -f adventure.rim advdataloader advdrumloader advsave wz \
	    adv_msgtab.ah adv_roomtab.ah adv_drumlayout.ah adv_objdefs.ah \
	    *.ac advtracks.drm 2>/dev/null || true
	@rm -rf Includes advtracks.drm 2>/dev/null || true
	cd $(GENDIR) && $(MAKE) clean
	cd $(RINGDIR) && $(MAKE) clean
