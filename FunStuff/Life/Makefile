all: life.rim

install: all
	cp Tapes/LifeLibrary.mtp /opt/pidp1-mods/Microtapes

life.rim: life.am1 life.ah lifefield.ac lifelib.ac lifemenu.ac lifetape.ac
	am1 life.am1

clean:
	rm -f life.rim >/dev/null 2>&1
