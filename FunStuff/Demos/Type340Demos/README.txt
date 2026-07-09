Type340demo shows off some of the type340 capabilities, it's self-explanatory.

Cube is a classic retro graphics demo, a rotating 3D wireframe cube.

Cube2 is a deceptively simple-appearing program that uses the Type 340 display and the
lightpen to let you rotate a 3D wire-frame cube.
Why 'deceptively'? It is not using precomputed vectors like cube does, all the vertex calculations are done
in real-time
The cube's orientation is a 3×3 rotation matrix, updated incrementally on each drag step using
the small-angle sin/cos approximation applied to the cube's vertices to compute new screen positions.
Impressive that a PDP-1 can do this with cycles to spare.
Of course, the Type 340 offloads all the display drawing, freeing up all those cycles for the math calculations.

Bounce is another classic, a bouncing ball that uses real physics.
The lower 6 test switches can be used to vary gravity.
All off uses the built-in default.
They are treated as a 2 digit octal n.n fractional number, 7.7 being the max.
This allows gravity less than 1. Higher values get silly quickly.
The switches are read every bounce cycle so you can change gravity on the fly.

NewLines is a version of the classic Type 30 lines program using the Type 340 long vector mode.

Cg340demo and cg340full show off the character generator.

These would have run exactly the same way on a real PDP-1 with a Type 340 display with the same timing,
all valid examples of 60's era graphics capability.

Dual, however, is a special case.
The real PDP-1 could have both a Type 340 and a Type 30, but only as separate display devices.
While they are implemented as separate driver 'hardware' in the emulator, both write to the same display,
e.g.t30dpy, and that's not really authentic.
Useful, though.
