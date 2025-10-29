This is an implementation of the Type 23 Drum memory.
Just copy to the IOTs directory and make in order to use it.

IMPORTANT - The H23 Paralell Drum Jul 64 manual has some very signifiant errors, don't believe it all.
The opcode for DCL is totally wrong.
The memory field is actually bits 1-5 to support the full extened memory possible.
The address field is as you would expect, 6-17.
The opcode for DRA is 722062, NOT 723062. Using the latter will hang the -1.
The listing for the diagnostic routine is missing some defines and has several typos and errors.

It simulates the timing of the real drum, more or less.
Transfer times will be correct, 8.5 us per word transferred.
The drum count, 0-4095, which is the current word location for the drum, is not timed exactly.
The real drum increments the count once every 8.5 us, but the emulator calls the poll method once ever 5 us.
So, the count increments faster than the original. This might affect the timing of some programs.

However, if you don't want to twiddle your thumbs for many milliseconds, there are 2 nonstandard additons.
IOT 161, DIA 100, will turn on fast mode, operations will complete immediately.
IOT 261, DIA 200, turns that off.

A transfer request is started immediately on execution of dcl, but the completion will appear to be delayed for
the proper time, 8.5 us per drum location needed to reach the start address plus 8.5 us per word transferred.

Some sample programs and tests are included.
Just make to assemble them, make clean to clean up.

drumtest.mac tests the drum implementation, covering all features and edge cases.
When started, it will execute multiple tests, halting after each one.
The program flags will show the test number, and the values in the AC and IO registers should match.
Press continue to go on to the next test.
When all tests have been done, the AC and IO will be zero and program flags will be off.

Also included is the DEC-1-137M diagnostic test program painfully copied and fixed from the manual.
Set the test switches to 020000 before you load it. That selects mem bank 2 for the test to use.
No output on the typewriter is a good sign.  It takes 5-6 minutes and will eventually finish.
As it was, the test never ends, it loops forever on the final test. This is less than useful.
The code has been changed to print a done message and halt.
And yes, the implementation passes. Note that the break system test requires the sbs16 break system which needs
dynamic IOT_60 to enable.

drumloader.mac is a special loader that can save a core image to drum and later restore it.
It resides in the same space as the default bin loader, 7751-7777.
It will halt on readin.
The test switches 1-5 should be set to the drum track to use, 0-37. Switch 0 is unused.
During writing to drum, if switches 6-17 are set, those are the starting address that will be jumped to
when the image is loaded.
If you don't know the start address for a program, use the disassembler and look at the 'start xxxx' at the end.
If the starting address in the switches is 0, then the loader will just halt at location 7772 after loading.
You can then set the start addr of the program and press start.

Typically you would have a program loaded already via the usual rim readin.
Then, rim load drumloader.rim, set up to write as just stated.
Press continue and it will execute and write all of the current memory to the drum and halt.
As long as you are loading programs from the drum, there should be no need to reload drumloader, just set the
proper track, turn ss1 off, and start at 7751 or 7752 if you don't want it to halt initially.
If you load something via readin, this will get overwritten by the bin loader.

Also note that some programs might overwrite this area. If so, you can't use this.

Included here is a pdp23drum file. This is preloaded with 4 programs:
0 - circle
1 - pdp1m13 music player
2 - snowflake
3 - spacewar48

Copy it to /opt/pidp1.
Just readin drumloader.rim, set ss1 off, set the leftmost test switched to 000000, 000001, 000010, 000011.
Set addr to 7752, start. Your program of choice will start. You can set the switches again at any time, start at 7752,
and the new prgram will start.

IMPORTANT: the loader MUST be assembled with the -r switch to macro1 to make it a pure rim loader.
If not, it will fail.

rotate.mac is a fairly useless cute special loader that rotates thru up to 3 differnt programs from the drum,
switching every minute.
Readin the rim file. It will halt so you can set the test switches to the programs you want to run.
Each 6 bits specify a drum track, 000000-011111, or 0 - 31. The high bit is ignored.
So, switches 0-5 are one prog, 6-11 another, and 12-17 the last. The programs are actually cycled from low selection
to high.
Naturally, the selected program can't halt, although if it does it can be continued. However, that kind of spoils
the effect.
Press continue and it will cycle every minute.

IMPORTANT: this also uses IOT_32, the timesharing clock, for the 1 minute interrupt.
If not, it will fail.

