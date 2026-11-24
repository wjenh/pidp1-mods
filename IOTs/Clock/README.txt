This is an implementation of the BBN timesharing clock, only available on the PDP-1D.
It provides a millisecond clock that can be read, as well as providing a 32 msec and 1 minute sbs interrupt.
If sbs16 is enabled, a different channel can be assigned to each.

Two nonstandard features are added, a millisecond countdown timer and a special 'clock' that reads the
emulator's time, simtime.
The latter is completely unauthentic and is only for emulator debugging and timing tests.

See the UsingTimesharingClock.md in the Docs directory for details.
