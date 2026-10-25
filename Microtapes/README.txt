This directory is where microtapes mounted via the mtp command
using just a name or relative path instead of an absolute path go.
A name starting with ./ or ../ is the exception: mtp takes it from the
current directory instead.

A microtape image's extension is .mtp.
This directory is also the tape library that pdp1central's Microtape tab lists,
with an optional index.txt here naming tapes and folders included from other locations,
one path per line.
A relative file reference is relative from this directory.
