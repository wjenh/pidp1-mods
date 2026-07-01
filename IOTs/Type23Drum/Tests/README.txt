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
