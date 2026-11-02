all: shrdlu.rim shrdlu_player shrdlu

shrdlu_player:
	cd Player; make install

shrdlu:
	cd Modern; make install

shrdlu.rim:
	cd Shrdlu; make install

clean:
	rm -f shrdlu.rim shrdlu.ovl shrdlu shrdlu_play >/dev/null 2>&1
	cd Shrdlu; make clean
	cd Modern; make clean
	cd Player; make clean
