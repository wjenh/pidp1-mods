Ironic Computer Space Simulator for the PDP-1
vers. 1.3, Norbert Landsteiner June 1, 2025
initial release: Nov 10, 2016


Requires a DEC PDP-1 with the automatic multiply/divide option (mul/div).

Source format: Unix new lines, tabs set to 8 spaces (like browsers, etc).


The program is in the public domain as long as the name of the author (me)
and the date of the program (2016, not 1962) are preserved.


Start address 4: control boxes (iot 11)
Start address 5: testword controls (lat)


Version 1.3 provides an update for more generally useful sense switch
options in anticipation of the PiDP-1. Recommended settings are now
all sense switches off (default). Moreover, display options for rocket and
saucer scores have been reworked.


Sense Switch Options (default: all off)

SSW 1 ...  Background effect
             off - Parallax, background stars move relatively to rocket ship.
             on  - Stars scroll continuously to the left.
SSW 2 ...  Torpedo Agility (steering)
             off - Normal.
             on  - Agile.
SSW 3 ...  Saucer Motion
             off - Diagonals are horizontally stretched.
                   (conforms more to the overall impression of the original CS)
             on  - Geometrical diagonals.
SSW 4 ...  Saucer Piloting, which saucer is shooting?
             off - Always the same one (as in original CS).
             on  - Random select.
SSW 5 ...  Score Display Mode: Rocket
             off - Original: decimal (normal).
             on  - Emulate the garbled hex display bug.
SSW 6 ...  Score Display Mode: Saucer
             off - Original: decimal (normal).
             on  - Emulate the garbled hex display bug.



Notes:

This program for the DEC PDP-1 attempts to provide a faithful recreation of the
1971 coin-op arcade game "Computer Space" by Nolan Bushnell and Ted Dabney
(Syzygy Engineering/Nutting Associates). However, there are few modifications,
owed to the platform. The original arcade uses reverse video to indicate
saucer explosions and overtime / extended play ("hyper space").
Since it is not possible to recreate this on the PDP-1, a special graphical
effect for saucer explosions has been added and overtime is indicated by a
scanning line effect at the very bottom of the display.

The original arcade game uses a 7448 BCD-to-7-segment decoder chip for its
score display. Due to a frequent hardware failure, the game logic may send full
4-bit values to the decoder, resulting in a garbled hex display for either the
rocket, the saucers, or both.
In this case, the 7448 decoder generates the following patterns:

  A (10)   B (11)   C (12)   D (13)   E (14)   F (15)

                               –-  
                     |  |     |        |
   --       --        --       –-       –-
  |        |                           |
   --       --                 –-       --       --
  
(The output of the 7448 for 0xF, dec. 15, is actually blank, but here a set
base line is used instead for unambiguity.)

Sense switches 5 and 6 allow to emulate this kind of faulty display for the
rocket and saucer scores, respectively. Mind that these options apply to the
display only and have no effect on the internal scoring.
Also mind that, like with the original game, only a single digit is displayed
and the score display is always truncated to the least significant digit.

Deviating from the original game, full scores are kept internally and players
are required to beat the saucers on each level in order to reach the next
overtime ("hyper space") and to continue.



Version History

Vers. 1.3, Jun 1, 2016: Consolidated sense switch options.
             Reworked alternative score display modes.
Vers. 1.2, Nov 23, 2016: Refined alternate scoring method.
             (No reset, evaluation of winner per play.)
Vers. 1.1, Nov 12, 2016: Added timeout for onscreen display to prevent burn-in.
             Minor adjustments to saucer positioning on respawn.
             Scores are now reset at the start of an extra play, when the
             optional scoring method (sense switch 6) is active.
Vers. 1.0, Nov 10, 2016: Release version.
             Adjusted pace and parameters (now 60 fps!).
             New option to disable the original scoring bug.
             Otherwise, player wraps at 16, saucers at 10.
             Torpedoes now always follow the turn of the ship.
Vers. 0.9.4/5, Nov 5, 2016: All new saucer explosion.
Vers. 0.9.3, Nov 3, 2016: Minor adaptations.
Vers. 0.9.2, Nov 3, 2016: Added a tiny animation as an indicator for
             'Hyperspace Mode' (extra play, in the original in reverse video).
Vers. 0.9.1, Nov 1, 2016: Minor adaptations.
Vers. 0.9.0, Oct 31, 2016: First release, beta
             (final version as for RetroChallenge 2016/10).
Vers. 0.0.1, Oct 12, 2016: Start of project.



Online emulation and latest code: http://www.masswerk.at/icss/



Norbert Landsteiner
Contact: see www.masswerk.at

Vienna, June 1, 2025