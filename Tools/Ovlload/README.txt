Ovlload puts a program's drum overlays, from the .ovl file am1 creates, onto the Type 23 drum,
then loads the program's tape into the pdp-1 as fastload does.

By default, the drum image is the file '/opt/pidp1-mods/pdp23drum'.
Each overlay goes on the track am1 chose for it at the drum addresses of its core window.
Nothing else on the drum is touched.
The program's stamp on its tape must match the .ovl's, so ovlload never loads a drum and a program
from different builds.  With -n no tape is read, so the stamp is not checked here.

A track holding another program's data, if that program has a recognized drum signature
(currently a drumupdater program, Adventure's save, a Z-machine story), is written regardless,
and ovlload warns of it after the fact.

-c only compares the drum with the .ovl, so a script can ask whether the drum is current.
-n writes the drum only and loads no program; load the program yourself.
If that program is from another build, the overlay manager's stamp check catches it at its
first overlay load.

-p tapefile punches a paper tape instead, in rim format.
This can then be loaded via Read In, as usual.
It contains a drum loader that is itself loaded.
That runs, loads the drum with the overlays that are also on the tape, then loads the program and starts it.
The writer warns of other program data as above on the typewriter,
checking each overlay's checksum while loading.
A halt with AC 1 is a tape checksum error, should never happen.
A halt with AC 2 is a memory write error, continue will retry it.
A halt with AC 3 is a drum transfer that failed 4 times.
Each continue will try it 4 more times, again shouldn't happen.
