all: switcher.mrim switcher.arim

switcher.mrim: switcher.mac
	/opt/pidp1/bin/macro1_1 switcher.mac

switcher.arim: switcher.am1
	../../Tools/AM1/am1 switcher.am1
	mv switcher.rim switcher.arim

switcher.mac: switcher.m1p
	../../Tools/M1pp/m1pp -o switcher.mac switcher.m1p
