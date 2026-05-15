These test various features. They are am1 programs and used the include file that defines the 340 opertions.
They test:

vec1 - param, point, vector, should draw an isocoles triangle
vec2 - vector continue, should draw a line from the lower left corner to the upper right corner
subjmp - subroutine mode JUMP, draw a triangle while main code increments AC in a loop
subsave - subroutine mode SAVE, draws 3 triangles side-by-side using a subroutine
subdeposit - subroutine mode DEPOSIT, draws 3 triangles vertically using nested subroutines
quadrant - draws a dot in the center and one in each quadrant
character - displays a message, should be obvious
increment - draws a box, under it a plus, under it a line using increment mode
increment2 - draws a diamond, point up
increment3 - draws 4 diamonds at different sizes with their leftmost points coincident
intensity - draws a line at each of the 8 intensities, dimmest at the top
type340AlignmentTest - draws a border at the extreme edges, 0,0 to 0,1023 to 1023,1023 to 1023,0 to 0,0
    then draws a plus in the center at 512,512 and 2 corner-to-corner diagonals, all should intersect;
    PF1 and PF2 shoudl be on to indicate edge violations from the diagonals
type340Stress - draw 19 full-width horizontal vectors using vcontinue at scale 0, 19,456 total points
