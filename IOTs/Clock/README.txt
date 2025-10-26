This is an implementation of the BBN timesharing clock, possibly only available on one PDP-1D.
It provides a millisecond clock that can be read, as well as providing a 32 msed and 1 minute sbs interrupt.
If sbs16 is enabled, a different channel can be assigned to each.

The documentation for the original is very sparse, extra features have been added to make it more useful.

The standard support used IOT 32, but apparently had no way to enable or disable it, nor to enable or diable
interrupts or specify the channels to use.
Calling it returned the current millisecond timer value in the IO register.
This call has been preserved and does just that.

However, some control funtionality has been added that is invoked by using IOT 2032.
It uses the AC register to control the clock.

The AC register flags are:
000 000 0eI iMM MMm mmm
e enables the clock, 1 to enable, 0 is as a regular IOT 32, disabling also clears the interrupt enables.
I enables the 1 min interrupt. 1 enables, 0 disables.
i enables the 32 ms interrupt. 1 enables, 0 disables.
MMMM is the channel to use for the 1 min interrupt.
mmmm is the channel to use for the 32 ms interrupt.
Why AC? Because all IOTs 30-37 automatically clear the IO register!

