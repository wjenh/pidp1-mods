all: switcher.rim

switcher.rim: switcher.mac
	/opt/pidp1/bin/macro1_1 switcher.mac

switcher.mac: switcher.m1p
	../M1pp/m1pp -o switcher.mac switcher.m1p

dtest.rim: dtest.mac
	/opt/pidp1/bin/macro1_1 dtest.mac

dtest.mac: dtest.m1p
	../M1pp/m1pp -o dtest.mac dtest.m1p
