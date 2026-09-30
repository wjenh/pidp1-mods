# Switcher

Switcher is a demo program that uses IOT 22, the DCS2 Data Communications Systerm, IOTs 61-63, the Type 23 drum,
IOT 32, the countdown timer/clock, and am1, the advanced assembler, to provide a remote-access program loader.

This example uses the ascii capabilities of am1.

A rim file will be created, switcher.rim which can be loaded as uusal.

## What does it do?

When loaded, it copies itself to bank 1 then opens a listener on port 2022.
When a connection is made, for example, putty in passive telnet mode, it prompts for a drum track to load onto
bank 0, as was saved by the drumloader program in IOTs/Type23Drum.
Note that is is not a telnet listener, hence putty in passive telnet mode, which is basically just a dumb
terminal.

When given a track number, it loads it into low memory, updates addresses 0-3 with proper interrupt information
and copies management code to locations 7751 and up, replacing the binloader, closes the connection,
then starts the loaded program at its saved start location.

If there was no saved start location, a notification will be given and the program will not be loaded.
Another will be asked for.

When a new connection comes in, control is passed back to the bank 1 code, assuming the loaded program has
not overwritten 0-3 or 7751 and up.

This means that any loaded program that uses interrupts will overwrite 0-3, so a manual restart will be needed.

This process can be repeated indefinitely.

## Why does it use am1?

The beause of the features it adds, such as bank management and local variables!
And of course for the drum, DCS, the clock....
