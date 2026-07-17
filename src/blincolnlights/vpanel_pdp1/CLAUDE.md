# vpanel_pdp1/ -- Virtual (SDL) Front Panel Notes for Claude

Scope: `main.c`, `elements.inc`, `Makefile` -- the SDL2 desktop application that renders a
clickable bitmap PDP-1 console and drives it off the same `/tmp/pdp1_panel` shared segment
used by the emulator and the hardware panel driver (`panel_pidp1/newpanel.c`). This file loads
automatically whenever Claude reads a file in this directory; it does not replace
`Claude/CLAUDE.md` (project-wide rules, coding standard, sandbox gotchas) -- read that too, it
is not repeated here.

## Source-tree refactor history (17-Jul-2026)
See `REFACTOR-CHANGES.txt` at the repo root for the full record. Relevant to this directory:
- `Makefile` rebuilt to match `pdp1/Makefile`'s dependency-tracking pattern (`-MMD`/`-MP`,
  real per-file `.o`/`.d` objects for `main.c` and `../common.c`), completing phase 3
  (Makefile modernization) alongside the same pass applied to `tapevis/Makefile`. Previously
  both source files were fed straight into a single hand-written link recipe with no header
  dependency tracking, so a change to a shared header (`common.h`, `panel_pidp1.h`) did not
  force a rebuild. `elements.inc` is still picked up correctly despite being `#include`'d
  directly into `main.c` rather than compiled separately: it is a real `#include`, so gcc's
  `-MMD`/`-MP` dependency scan sees it like any other header. Verified: touching `../common.h`
  correctly forces both `main.o` and `common.o` to recompile; an unrelated rebuild with
  nothing touched is a no-op. The built binary is still named `panel_pdp1` (matching the
  original recipe), not `vpanel_pdp1`, despite the directory name.
- **17-Jul-2026, phase 4:** the sibling `art/` directory (the original `.png`/`.svg` source
  images that `panelart.inc`/`pdp1art.inc`'s baked-in C byte arrays were generated from --
  not used at build or runtime, kept only as source reference) folded into this directory.
  `main.c`'s two `#include "../art/panelart.inc"` / `"../art/pdp1art.inc"` lines dropped their
  `../art/` prefix accordingly. This directory itself was NOT renamed (stays `vpanel_pdp1/`,
  by explicit decision -- only `art/`'s contents moved, not the directory name).

**Known, accepted concurrency caveat -- confirmed with the project owner, not defended
against in code:** `panel->pwmcount[][]`'s existing contract (see its own comment in
`panel_pidp1.h`) assumes a single reader that periodically reads and resets it;
`panel_pidp1/newpanel.c`'s `pwmthread()` already does this destructively, and this change
makes `vpanel_pdp1` do the same against the *same* shared array. If both front ends were
attached to `/tmp/pdp1_panel` at the same time, each one's reset would zero out the other's
in-flight integration window, silently corrupting whichever one reads second (not a crash).
Raised this during the analysis pass that preceded this change; the project owner confirmed
(05-Jul-26) that `newpanel` and `vpanel_pdp1` are not supposed to run concurrently against the
same segment, that nothing in the inherited connection logic currently blocks it, and that
this will be called out in user-facing documentation rather than enforced here. A future
session should not add code-level guards against concurrent attachment based on this
conversation alone -- that would be a product decision, not a bug fix, and the owner has
already made the call for now.


- **DEP key is intentionally right-click-only (state==2), by design -- not a bug.** Every other
  single-mapped key in `updatepanel()` (STOP, CONT, EXAM, READIN, FEED) fires on state==1
  (left-click/"down"). DEP is the sole exception, firing only on state==2 ("up"/right-click),
  with no state==1 case. Initially flagged as a likely copy/paste bug during this review;
  confirmed by the project owner (04-Jul-26) that this matches real PDP-1 hardware: the deposit
  switch had to be lifted up, not pressed down, as a safety interlock against accidentally
  depositing into memory. Left-clicking the on-screen DEP key doing nothing is correct.
- **Shared-segment truncation risk (not yet fixed, just documented):** `initpanel()` calls
  `createseg()` (`common.c`), which unconditionally `ftruncate()`s `/tmp/pdp1_panel` to
  `sizeof(Panel)` on every open, even if the file already exists and is mapped by another live
  process. The emulator (`pdp1/panel1.c:getpanel()`) only `attachseg()`s (no truncate), so it
  relies on a panel front end -- this program or `panel_pidp1/newpanel.c`, both of which
  `createseg()` -- having created the file first. If two of these binaries are built against
  different versions of `panel_pidp1.h` and therefore disagree on `sizeof(Panel)`, whichever
  opens the segment second silently resizes it out from under whichever process already has it
  mmap'd. Neither `createseg()` nor any caller checks the existing file's size before
  truncating. Not observed to have actually happened; flagged as a latent risk given the
  project's own append-only struct-field convention exists precisely to guard against this
  class of problem elsewhere (`pdp1.h`/`PDP1`), but nothing here enforces or checks it for
  `Panel`.
- **`elements.inc`'s "run, single step, single inst" comment on the three grid2 horizontal
  switches is stale/copy-pasted** from the identical comment on the lights-table group at the
  same grid position (which legitimately are run/sstep/sinst status lamps). The first of the
  three switches (`misc_sw[0]`) is actually bound to `SW_POWER` in `main.c`, not to any "run"
  control -- there is no run switch in the `panel_pidp1.h` bit enum, only the `L5_RUN`
  status-light bit. Not a functional bug, just a misleading label; left as-is pending direction.
- **I/O panel lights (`panel->lights7`-`lights9`) have no corresponding Elements at all** in
  `elements.inc` -- this front end only ever renders/updates `lights0`-`lights6`. This is intentional.
- `drawgrid()` is dead debug code (alignment-check helper from panel-art development), reachable
  only via a commented-out call in `draw()`.
- `SDL_Init()`/`IMG_Init()` return values are not checked in `main()`; failures would currently
  surface later and less specifically via `loadtex()`'s own `panic()` call.
- No cleanup on exit (no `SDL_Quit`/`IMG_Quit`/texture destruction/`munmap` of the shared
  segment) -- process just `exit()`s and relies on the OS to reclaim everything.
