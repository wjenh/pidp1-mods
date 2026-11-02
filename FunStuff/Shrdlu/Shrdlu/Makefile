# am1 writes shrdlu.ovl, the drum overlays, beside shrdlu.rim.
shrdlu.rim: shrdlu.am1 shrdlu.ah cache.ac dbreak.ac debug.ac dlist.ac hand.ac hidden.ac project.ac term.ac world.ac \
		atoms.ac db.ac blocks.ac natives.ac engine.ac events.ac goals.ac theorems.ac share.ac \
		lexicon.ac reader.ac parse.ac print.ac cmds.ac room.ac tree.ac sm.ac read.ac keep.ac \
		semantics.ac pgm.ac pgsys.ac collect.ac hooks.ac drop.ac eval.ac planner.ac plcold.ac evlisp.ac r12.ac learn.ac tables.ac hotget.ac labels.ac
	am1 shrdlu.am1

install: shrdlu.rim
	cp shrdlu.rim shrdlu.ovl ..

clean:
	rm -f shrdlu.rim shrdlu.ovl >/dev/null 2>&1
