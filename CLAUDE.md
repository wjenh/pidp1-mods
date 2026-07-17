# PiDP-1 Emulator -- Project Notes for Claude
## Language
Use American English spelling throughout all project files -- "behavior" not "behaviour",
"color" not "colour", "recognize" not "recognise", and so on.

Do not use em-dash, --, as a separator. Start a new sentence.

## Action Rule
**Read but take no other action until specifically directed to.**
Analyse, plan, ask clarifying questions but make no file modifications, run no builds,
and execute no shell commands until specifically directed to do so.

**Do not assume that the pdp-1 architecture or the am1 assembler operate in the same way
as modern architectures or languages.
Specifically, variables and locations initialized in source files are load-time initializations;
pdp-1 memory is non-volatile and programs can be rerun without reloading.
Any value that needs to be initialized when a program is started must be explicitly initialized in the program.

**Exception -- Claude-related documentation (clarified 02-Jul-26):** this rule governs the
emulator codebase itself (source, build files, configs -- anything that runs or ships). It does
NOT cover documentation under the `Claude/` directory (`Claude/CLAUDE.md`,
`Claude/skill-updates/*`, `Claude/README.txt`) or any nested `CLAUDE.md` file colocated with
code (e.g. `src/blincolnlights/pdp1/CLAUDE.md`, `IOTs/Type340Display/CLAUDE.md`). The project
owner does not consider updates to these knowledge-base files to be "code changes." Claude may
create, edit, or extend any of these documents at any time, including mid-session and without
being asked, per the standing knowledge-base-currency instruction in `Claude/CLAUDE.md`.

## Reference Files -- Read These Now
All project context, architecture notes, coding standards, and device behaviour documentation
is maintained in the repository under `Claude/`. Read the CLAUDE.md document and relevant skill files first.
This indirection is intentional -- keep it.

pdp1-emulator.skill -- coding standards, hadware architecture, device descriptions

am1-pdp1-assembler.skill -- am1 assembler usage, syntax, examples

Additional subdirectories might exist with skill data not yet incorporated into the skill documents, check
that also if present.

In addition to the above: subsystem-specific knowledge (device internals, implementation
history, tuning rationale) lives in nested `CLAUDE.md` files colocated with the code they
describe (e.g. `src/blincolnlights/pdp1/CLAUDE.md`, `src/blincolnlights/panel/driver/CLAUDE.md`).
These load automatically when a file in that directory is read -- no separate instruction
needed to find them, just be aware they exist and keep adding to them going forward rather
than growing `Claude/CLAUDE.md` without bound. See `Claude/CLAUDE.md`'s own "Nested CLAUDE.md
files" section for the full pattern and which topics currently live where.

## Hard Constraints (summary, full detail in Claude/CLAUDE.md)
- New fields in `PDP1` / `Panel` structs go at the **END** (ABI compatibility).
- When `pdp1.h` changes, rebuild emulator AND all IOT plugins explicitly.
- All files to be saved in ascii, not utf-8.
- Do not modify files in the Docs directory, they are primary references only
- Never run `git commit`, `git push`, or any other command that would write to git history
  or to a remote from this checkout (clarified 04-Jul-26). This working copy is a disposable
  Cowork sandbox checkout, not the project of record -- the true master lives on another
  machine, and this repo is expected to sit with uncommitted changes indefinitely. Read-only
  git commands (`git status`, `git diff`, `git log`, etc.) are fine for figuring out what has
  changed; anything that mutates the repo's committed state or talks to `origin` is not,
  regardless of how routine or low-risk it seems.
- Do not modify or delete cleanup.sh in the root directory unless specifically told to.

## Sandbox build-dependency cache -- use this before building anything needing SDL2/SDL3/etc.
Some libraries the build needs (SDL3 today; more may follow) have no Ubuntu 22.04 apt package,
and every Cowork task starts a brand-new sandbox with nothing installed from source or apt
carried over. Rather than rebuilding from scratch each task, already-built/fetched libraries are
cached in the project tree at `sandbox-libs/ubuntu22-x86_64/` (currently: sdl2, sdl2_image,
sdl2_ttf, udev, sdl3).

Before building any target that needs one of these, source the bootstrap script and run `make`
in the SAME shell command (env vars do not persist between separate shell-tool calls):

    . sandbox-libs/bootstrap.sh && make -C src/blincolnlights/pdp1 LIBS="-lpthread -lSDL3 -lm"

(The explicit `LIBS=` override is only needed for `src/blincolnlights/pdp1`'s own Makefile,
whose default `-lm` ordering breaks static linking against the vendored libSDL3.a -- see
`sandbox-libs/ubuntu22-x86_64/NOTES.txt` for why. Other targets build with a plain `make`.)

This requires zero Makefile changes -- `sandbox-libs/bootstrap.sh` only exports environment
variables (`PKG_CONFIG_PATH`, `CPATH`, `LIBRARY_PATH`, `PATH`, `LD_LIBRARY_PATH`) and copies
files into `/tmp/sandbox-libs-install`, a writable, session-stable path (the sandbox has no
root, and `$HOME`/the project mount path both change every session, so neither is usable as a
stable cache-install target). Full rationale, gotchas already solved (dangling .so symlinks in
Debian -dev packages, static link ordering, why sdl3 is a deliberately headless-only build) and
how to rebuild/extend the cache are in `sandbox-libs/ubuntu22-x86_64/NOTES.txt` and
`Claude/CLAUDE.md`. Read those before adding a new vendored library.

## Sandbox stale-read gotcha -- always use the write-new-file+mv workaround
This Cowork sandbox's bash mount pervasively serves stale/truncated reads of a file just
edited or written via the Read/Write/Edit tools -- confirmed recurring across many files and
sessions, not a one-off (full technical detail and history in `Claude/CLAUDE.md`). Treat this
as certain to happen, not something to detect first: as a standing default, after any
Write/Edit to a file that a subsequent bash command (compile, `cat`, `wc`, `tar`, etc.) will
read, write the corrected content to a new filename and `mv` it over the original from bash
before running that bash command -- don't wait for a build error or a suspiciously-small
`wc -l` to confirm the staleness first, just apply the workaround proactively. This applies
project-wide, not to any one file or subsystem.
