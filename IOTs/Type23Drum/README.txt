This is an implementation of the Type 23 Drum memory.
Just copy to the IOTs directory and make in order to use it.

It simulates the timing of the real drum, more or less.
Transfer times will be correct, 8.5 us per word transferred.
The drum count, 0-4095, which is the current word location for the drum, is not timed exactly.
The real drum increments the count once every 8.5 us, but the emulator calls the poll method once ever 5 us.
So, the count increments faster than the original. This might affect the timing of some programs.

A transfer request is started immediately on execution of dcl, but the completion will appear to be delayed for
the proper time, 8.5 us per drum location needed to reach the start address plus 8.5 us per word transferred.

drumtest.mac exercises the drum

drumloader.mac is a special loader that can save a core image to drum and later restore it.
It resides in the same space as the default bin loader, except a few words bigger.

Set the test word to the drum track to use, 0-37. Set sense swith 1 on for write to drum, 0 to load from drum.
Typically you would have a program loaded already via the usual rim readin. Then, set up to write as just stated
and rim load drumloader.mac. It will immediately execute and write all of the current 4096 words of memory to the drum.
As long as you are loading programs from the drum, there should be no need to reload drumloader, just set the
proper track, turn ss1 off, and start at 7744.
