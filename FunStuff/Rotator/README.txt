Rotate.am1 is a fairly useless cute special loader that rotates thru up to 6 differnt programs
from the drum, switching every minute.
Readin the rim file. It will halt so you can set the test switches to the programs you want to run.
Each 6 bits specify a drum track, 000000-011111, or 0 - 31. The high bit is ignored.
So, switches 0-5 are one prog, 6-11 another, and 12-17 the last.
After a continue, enter the next 3 programs.
Continue again to start runnig.
The programs are cycled from low selection first word to high selection second word and repeat.
Naturally, the selected program can't halt, although if it does it can be continued.
However, that kind of spoils the effect.
Rotate loads into 0-3 and 7751 up in bank 0, and loads its control program into bank 1.
You can't have it load a program that overwrites either of the bank 0 areas.

IMPORTANT: this also uses IOT_32, the timesharing clock, for the 1 minute interrupt.
If not present, it will fail.

NOTE: this is the am1 version of the mac version in IOTs/Type23Drum. See how easy multibank is with am1?
