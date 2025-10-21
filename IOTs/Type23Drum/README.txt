This is an implementation of the Type 23 Drum memory.
Just copy to the IOTs directory and make in order to use it.

It simulates the timing of the real drum, more or less.
Transfer times will be correct, 8.5 us per word transferred.
The drum count, 0-4095, which is the current word location for the drum, is not timed exactly.
The real drum increments the count once every 8.5 us, but the emulator calls the poll method once ever 5 us.
So, the count increments faster than the original. This might affect the timing of some programs.

A transfer request is started immediately on execution of dcl, but the completion will appear to be delayed for
the proper time, 8.5 us per drum location needed to reach the start address plus 8.5 us per word transferred.

drumtest.mac tests the drum implementation, covering all features and edge cases.
When started, it will execute multiple tests, halting after each one.
The program flags will show the test number, and the values in the AC and IO registers should match.
Press continue to go on to the next test.
When all tests have been done, the AC and IO will be zero and program flags will be off.

drumloader.mac is a special loader that can save a core image to drum and later restore it.
It resides in the same space as the default bin loader, except a few words bigger.

It will halt on readin.
Set the test word to the drum block to use, 0-37 octal. Set sense switch 1 on for write to drum, 0 to load from drum.
Typically you would have a program loaded already via the usual rim readin. Then, rim load drumloader.rim,
set up to write as just stated.
Press continue and it will execute and write all of the current memory up to 7751 to the drum and halt.
As long as you are loading programs from the drum, there should be no need to reload drumloader, just set the
proper track, turn ss1 off, and start at 7751.
It will load the image and halt. You can then set the start addr of the program and press start.
If you load something via readin, this will get overwritten by the bin loader.
