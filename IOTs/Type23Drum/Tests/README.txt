The orginal DEC-1-137M diagnostic program plus an additional test that fills in some gaps are here.
Just make to assemble them, make clean to clean up.

The DEC test program was painfully copied and fixed from a listing in the manual.
Set the test switches to 020000 before you load it.
That selects mem bank 2 for the test to use.
No output on the typewriter is a good sign.
It takes 5-6 minutes and will eventually finish.
As it was, the test never ended, it looped forever on the final test. This is less than useful.
The code has been changed to print a done message and halt.
And yes, the implementation passes.
Note that the break system test requires the sbs16 break system which needs to be enabled in the pidp1.config file.

Five further probes, each with its expected results in its header comment:
dbaprobe.am1 checks that a DBA break arrives when the drum reaches the DBA address, both on its own and in
the DBA, DWC, DCL sequence the manual gives. Run it with sbs16=off; it halts in about half a second and
leaves a table of results at 2000.
endbrkprobe.am1 checks that every transfer ends with a sequence break, that the break waits while breaks
are off, and that a DBA, DWC, DCL sequence takes the dba break and then the end break. Run it with
sbs16=off; it halts in about half a second and leaves a table of results at 2000.
rdprobe.am1 checks that a drum read of 1 to 3000 words writes exactly that many words, none past the block.
It stops at three halts (continue after the first two) and takes about two minutes.
wrapprobe.am1 checks that a drum write that wraps past address 7777 puts the words after the wrap at address 0 on.
It halts in under a second with AC 0 on a correct drum.
extprobe.am1 checks that the drum sees a change another program makes to the drum file while the machine runs.
It reads drum field 0 word 0 in a loop and halts with the new value in AC once it changes, so another program
must change the drum file (pdp23drum) while it runs.
