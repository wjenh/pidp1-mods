This is an implementation of the Type 23 Drum memory.
Just copy to the IOTs directory and make in order to use it.

It simulates the timing of the real drum, more or less.
Transfer times will be correct, 8.5 us per word transferred.
The drum count, 0-4095, which is the current word location for the drum, is not timed exactly.
The real drum increments the count once every 8.5 us, but the emulator calls the poll method once ever 5 us.
So, the count increments faster than the original. This might affect the timing of some programs.

A transfer request is started immediately on execution of dcl, but the completion will appear to be delayed for
the proper time, 8.5 us per drum location needed to reach the start address plus 8.5 us per word transferred.
