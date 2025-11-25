# Switcher

Switcher is a demo program that uses IOT 22, the DCS2 Data Communications Systerm, IOTs 61-63, the Type 23 drum,
and m1pp, the macro_1 preprocessor to provide a remote-access program loader.

## What does it do?

When loaded, it copies itself to bank 1 then opens a listener on port 2022.
When a connection is made, for example, putty in passive telnet mode, it prompts for a drum track to load onto
bank 0, as was saved by the drumloader program in IOTs/Type23Drum.

When given a track number, it loads it into low memory, updates addresses 0-3 with proper interrupt information
and copies management code to locations 7751 and up, replacing the binloader, closes the connection,
then starts the loaded program at its saved start location.

If there was no saved start location, a notification will be given and the program will not be loaded.
Another will be asked for.

When a new connection comes in, control is passed back to the bank 1 code, assuming the loaded program has
not overwritten 0-3 or 7751 and up.

This process can be repeated indefinitely.

## Why does it use m1pp?

To get include files!
This makes using the various IOTs much easier and also provides some utility code.
The processed switcher.mac is included if you don't want rerun m1pp.
