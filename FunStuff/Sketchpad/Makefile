all: sketchpad.rim sketchplot

sketchplot:
	cd Render; make

sketchpad.rim: sketchpad.am1
	am1 sketchpad.am1

clean:
	rm -f sketchpad.rim >/dev/null 2>&1
	cd Render; make clean
