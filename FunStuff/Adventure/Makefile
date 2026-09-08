all: adventure.rim
	cd $(RINGDIR) && $(MAKE) install

GENDIR = AdvDataLoader
RINGDIR = DecoderRing
DRUM   = /opt/pidp1-mods/pdp23drum

AM1 = am1
adventure.rim: adventure.am1 adv_defines.ah
	$(AM1) -S adventure.am1 | tee bank-usage.txt
	@rm -f bank-usage.txt

adv_defines.ah: $(GENDIR)/adventure.adv loaders
	@mkdir Includes 2>/dev/null || true
	$(GENDIR)/advdataloader -o Includes $(GENDIR)/adventure.adv
	./advdrumloader advtracks.drm

loaders:
	cd $(GENDIR) && $(MAKE) install

clean:
	@rm -f adventure.rim adventure.sym adventure.lst \
	    advdataloader advdrumloader advsave wz \
	    adv_msgtab.ah adv_roomtab.ah adv_drumlayout.ah adv_objdefs.ah \
	    *.ac advtracks.drm bank-usage.txt 2>/dev/null || true
	@rm -rf Includes advtracks.drm 2>/dev/null || true
	cd $(GENDIR) && $(MAKE) clean
	cd $(RINGDIR) && $(MAKE) clean
