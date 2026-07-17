# tapevis/ -- Tape Visualizer Notes for Claude

Scope: `tapevis.c` -- a standalone SDL2 utility for visualizing paper tape contents. This
file loads automatically whenever Claude reads a file in this directory; it does not replace
`Claude/CLAUDE.md` (project-wide rules, coding standard, sandbox gotchas) -- read that too, it
is not repeated here.

## Source-tree refactor history (17-Jul-2026)
See `REFACTOR-CHANGES.txt` at the repo root for the full record. Relevant to this directory:
- `Makefile` rebuilt to match `pdp1/Makefile`'s dependency-tracking pattern (`-MMD`/`-MP`,
  real per-file `.o`/`.d` objects for `tapevis.c` and `../common.c`), completing phase 3
  (Makefile modernization) for all five targets alongside the same pass applied to
  `vpanel_pdp1/Makefile`. Previously both source files were fed straight into a single
  hand-written link recipe (`cc -o $@ $^ -I.. \`sdl2-config --cflags --libs\``) with no
  header dependency tracking, so a change to a shared header (`common.h`) did not force a
  rebuild. `sdl2-config`'s `--cflags`/`--libs` output is now split into separate
  `SDL2_CFLAGS`/`SDL2_LIBS` variables since `--cflags` belongs on the per-file compile rules
  and `--libs` only on the final link. Verified: touching `../common.h` correctly forces both
  `tapevis.o` and `common.o` to recompile; an unrelated rebuild with nothing touched is a
  no-op.
