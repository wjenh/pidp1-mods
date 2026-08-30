all: adventure.rim
	cd $(RINGDIR) && $(MAKE) install

GENDIR = AdvDataLoader
RINGDIR = DecoderRing
DRUM   = /opt/pidp1-mods/pdp23drum

adventure.rim: adventure.am1 adv_msgtab.ah skipmacros.ah
	am1 adventure.am1
	advdrumloader advtracks.drm

# One rule regenerates the whole generated-include set + the track data; the
# other ten outputs ride along with adv_msgtab.ah (single generator run).
adv_msgtab.ah: $(GENDIR)/adventure.adv loaders
	$(GENDIR)/advdataloader $(GENDIR)/adventure.adv

loaders:
	cd $(GENDIR) && $(MAKE) install

clean:
	@rm -f adventure.rim advdataloader advdrumloader wz \
	    adv_msgtab.ah adv_roomtab.ah adv_drumlayout.ah adv_objdefs.ah \
	    *.ac advtracks.drm 2>/dev/null || true
	cd $(GENDIR) && $(MAKE) clean
	cd $(RINGDIR) && $(MAKE) clean
