# PiDP-1 Emulator -- Project Notes for Claude

## Hard Constraints
- When `pdp1.h` changes, rebuild BOTH the emulator (`make` in `src/blincolnlights/pdp1/`) AND all IOT plugins (`make` in `IOTs/Type30Display/` and other IOT subdirs). The user's normal workflow only runs `make` in the pdp1 directory; IOT rebuilds must be done explicitly.
- After any session involving non-trivial findings, fixes, or design decisions, update this file (and anything else under `Claude/` that's relevant) with the new information before the session ends, without waiting to be asked first. Treat "keep the knowledge base current" as standing, ongoing scope for every session, not a one-off request from 22-Jun-2026 when it was first stated explicitly.
- Files under `Claude/` (this file and everything in `Claude/skill-updates/`) must be plain ASCII only. No em-dashes, smart/curly quotes, arrows, fraction characters, box-drawing characters, checkmarks, or other non-ASCII symbols. The project owner's editor cannot handle non-ASCII encodings in these specific files. Use `--` for an em-dash, `->` for an arrow, `1/3`/`1/4` for fractions, plain `-` repeated for a divider line, `!=`/`~` for math symbols, and so on. This rule is scoped to `Claude/`'s own files; it does not apply to emulator source code, other project documentation, or anywhere else, unless told otherwise.
- Hardware testbed note (02-Jul-26): the Pi 4 testbed has no physical LED panel attached, so
  no panel-hardware-dependent symptom can be observed OR ruled out there -- do not treat "only
  reproduces on Pi 5" as a confirmed platform-specific finding for any panel-related bug unless
  it's been actually tested on Pi 4 hardware with a panel attached. See the panel-flicker
  investigation summarized in `src/blincolnlights/panel/driver/CLAUDE.md` for the case this
  came up in.
- Proactively flag possible context-degradation ("context rot") during a session, rather than silently continuing: if it becomes apparent that something already established earlier in the same session is being asked for again, that new output contradicts an earlier decision or finding without acknowledging the conflict, or track of the actual file/codebase structure has been lost, say so explicitly to the project owner rather than pushing forward as if nothing happened. There is no usage indicator available in this product surface to check directly (confirmed 22-Jun-2026 -- no context-window percentage is exposed anywhere in Cowork as of this writing), so these behavioral signs are the only available signal.
- Standing rule (added 03-Jul-26): clean up build/test artifacts created while verifying a
  fix -- `.o`, `.d`, `.so`, and any compiled executables (test harnesses, the `pdp1` binary,
  `newpanel`, `t30dpy`, etc.) -- once verification is done, rather than leaving them sitting
  in the working tree as clutter. This applies every session that builds/links anything to
  verify a change, not just when asked.
  Sandbox limitation found the same day: `rm`, `mv`, and Python's `os.remove()` on such a
  file all fail with "Operation not permitted" on this Cowork sandbox's mount, the same
  undeletable-artifact behavior already documented below under the stale-bash-read gotcha,
  but here it reproduced on ordinary build output, not just leftovers from a failed
  build/link. `lsattr` again reports "Operation not supported" (not a chattr immutable
  flag). Unlink is blocked, but a plain overwrite is not: truncating the file in place
  (`: > path/to/file`, or any open()-with-O_TRUNC-style write) succeeds and zeroes it out.
  Since true deletion isn't available, treat "truncate every build artifact to 0 bytes" as
  the working definition of "clean up" on this sandbox until a real fix is found -- it
  removes the actual content (the concern) even though the directory entry and name remain.

- Workflow convention (added 09-Jul-26): for interactive/graphical verification (loading a
  program into the emulator and checking the display/lightpen behavior), the project owner runs
  that themselves rather than having Claude spin up the emulator -- stated as "I'm inexpensive,
  you are not." Claude's normal scope stays at assembling/building and static checks (clean
  assembly, listing/symbol-table spot checks, code review); don't attempt to launch the
  graphical emulator for runtime verification unless specifically asked.
  Clarified same day: this is specifically about the *graphical* path (running t30dpy/t30dpy3
  and looking at rendered output). A separate, explicitly-requested automated test harness now
  exists under `Claude/SupportCode/` (see its own `CLAUDE.md`) that drives a headless pidp1
  via the `ad1` text debugger and reads/writes the raw Type 340 display socket directly --
  no SDL, no GUI, no pixels. That harness is fine to use freely when asked for automated
  testing/verification; this convention only gates launching the *graphical* emulator.

## Design Convention -- Shared Struct Field Ordering
New fields added to shared data structures (`PDP1` in `pdp1.h`, `Panel` in `panel_pidp1.h`) go at the **END** of the struct. This preserves ABI compatibility: IOT plugins compiled against an older `pdp1.h` continue to access existing fields at the correct offsets. Inserting fields anywhere else shifts subsequent field offsets and silently breaks all IOT plugins that weren't rebuilt.

## Architecture

### IOT Instruction Field Layout

From `pdp1.c`: `IR = MB[bits 0-4]` (top 5 bits). `IR_IOT = (IR == 035)`.

- `dev = MB & 077` -- low 6 bits of MB (device number, 0-63)
- `ch  = (MB >> 6) & 077` -- bits 6-11 of MB (sub-channel / argument)
- B5  = 010000 -- i bit (synchronous I/O wait)
- B6  = 004000 -- C bit (asynchronous completion)
- `nac = ((MB & (B5|B6)) == B5) || ((MB & (B5|B6)) == B6)` -- true if exactly ONE of B5/B6 set

Dynamic IOT plugins (IOT_nn.so) receive `nac` as their `completion` parameter. They signal
completion to the emulator via `IOCOMPLETE(pdp1P)` (sets `pdp1P->ios = 1`), defined in
`IOTs/iotHandler.h`. The machine clears `ioh` at TP8 when `ios=1`.

**B6 embedding in high-numbered ch codes:** B6 (004000) is bit 6 of MB, which is also
the MSB of the ch field (bits 6-11). Any command code in the range ch=040-057 has bit 5
of ch set, which means B6 is inherently present in MB for that command regardless of any
programmer-specified modifier. Adding the i modifier (B5=010000) then sets BOTH B5 and
B6 -> nac=0 -> completion=0 in iotHandler. This affects every DCS2 extended command
(ch 040-054: TCB, SSB, SCB, RLE, RPC, RCI, RIC, RCS, RWE, ROC, RES, RXL). None of
these can use `i` for ioh blocking. The correct blocking pattern is the command followed
by `ioh` (730000), which sets ioh=1 and waits for IOCOMPLETE fired by iotPoll.

**IOT completion call convention -- common bug:**
iotHandler is called TWICE for blocking IOTs:
- `completion=0` (initial call): set up the blocking condition (e.g., set a flag that
  iotPoll checks). Do NOT call IOCOMPLETE. Return and let the machine spin in ioh.
- `completion=1` (second call, after iotPoll fires IOCOMPLETE): the condition is now
  met. Do the actual work and return.

The mistake is putting the setup code inside `if( !completion )` instead of
`if( completion )`. For an IOT that uses the C bit (B6), nac=1 and completion=1
on the INITIAL call. So the setup code belongs in `if( completion )`.
If `if( !completion )` is used instead, need_general_completion is never set,
the end-of-handler `if(completion && !need_general_completion)` fires IOCOMPLETE
immediately, and the IOT becomes a no-op. See devices-additions.md (RWE) for the
concrete case.

Historical confirmation (11-Jul-26): DEC's own F25 "PDP-1 Input-Output Systems Manual"
Table I defines the completion-pulse-enable signal as exactly the XOR of bits 5 and 6 --
the same formula as `nac` above -- and documents that setting both bits (wait requested,
pulse disabled) is a hang hazard on real hardware too. See the F25 research summary in
`src/blincolnlights/pdp1/CLAUDE.md` for the full comparison, including confirmation that
the already-fixed `tyo C` and DCS2 `rwe i` hangs match this documented hardware behavior.

### The `ioh` Instruction (730000)

A standalone "I/O halt/wait" instruction. B5=1 in 730000 -> sets `ioh=1` at TP7. If `ios` is
already 1 (from a prior async completion): TP8 clears `ioh` immediately and execution continues.
If `ios=0`: machine spins until a device calls `IOCOMPLETE()`. Used to synchronise with a
previously issued `tyo C` or similar async IOT.

### Skip instruction direction rule

When writing `skip_instr; jmp target`, think about **when the `jmp` executes** (the no-skip case),
not when the skip fires.

- `sza; jmp bad` -> jmp executes when AC != 0 (sza skips over jmp when AC = 0)
- `sza i; jmp bad` -> jmp executes when AC = 0 (i bit inverts: sza i skips when AC != 0)

Mental model: choose the skip instruction that describes the **"don't branch"** condition. The jmp
fires whenever that condition is NOT met.

Example -- test for error (error flag is non-zero):
```
    and [dserr]     // isolate error bit
    sza             // skip if AC = 0 (no error) -- the "don't branch" condition is AC=0
    jmp halterr     // jmp fires when AC != 0 (error present)
```

Example -- loop until connected (dsfcon flag is non-zero when connected):
```
    and [dsfcon]    // isolate connected flag
    sza i           // skip if AC != 0 (connected) -- "don't branch when connected"
    jmp waitcon     // jmp fires when AC = 0 (not yet connected)
```

### Sandbox Note -- Getting Build Dependencies Without Root (21-Jun-2026)

The Cowork sandbox has no root/sudo. `apt-get install` fails; the workaround for any missing
package is `apt-get -o Dir::Cache::Archives=/tmp/debs download <pkg>` then
`dpkg-deb -x <pkg>.deb /tmp/local` -- no root at any step. Compile/link against the unpacked
prefix. SDL2 was confirmed working this way. SDL3 (`libsdl3-dev`) does not exist in the sandbox's
Ubuntu 22.04 apt index at all; build from source if needed (github.com is reachable).

For am1 from source: needs bison, flex, and m4 (none preinstalled); get all three via the same
no-root download trick. bison additionally needs `BISON_PKGDATADIR` and `M4` env vars pointed at
the unpacked locations.

**Sandbox gotcha: backgrounded processes (`nohup ... &`) do NOT survive between separate
shell-tool calls (found 07-Jul-26, sandbox-libs build session).** Each shell-tool invocation
runs inside its own fresh `bwrap --unshare-pid` namespace (confirmed via `ps aux`, PID 1 is a
fresh `bwrap` per call); when that call ends, the whole namespace and everything in it is torn
down, killing any backgrounded job started during that call. This CONTRADICTS the
"long builds must be backgrounded (nohup make ... &) and polled from subsequent calls" line
below (dated 02-Jul-26) -- that guidance does not hold for this shell tool as currently
provisioned; a `cmake --build .` started with `nohup ... &` was silently killed partway
through (19 of ~240 object files done) the moment the call that launched it returned, with no
error surfaced. Disk state DOES persist across calls (same underlying filesystem mount), so the
correct pattern for a build too long for one call's wall-clock budget is repeated foreground
`timeout <N> <build command>` calls relying on the build tool's own incremental/resumable state
(make and ninja both skip already-built objects on the next invocation) -- not backgrounding.
If a future session finds backgrounding DOES survive (e.g. because the tool's sandboxing
changed), re-test before trusting either claim blindly; until then, prefer the
timeout-and-repeat pattern.

**Sandbox note: a persistent, cross-session vendored library cache exists at
`sandbox-libs/ubuntu22-x86_64/` (added 07-Jul-26).** Ubuntu 22.04 lacks apt packages for some
libraries the project's builds want (SDL3 confirmed absent; likely to recur for other
newer libraries in the future), and every Cowork task gets a brand-new sandbox with nothing
installed from source or apt carried over -- but the project's own file tree persists. Rather
than re-fetching/rebuilding from scratch every task, `sandbox-libs/ubuntu22-x86_64/` holds
already-built/already-fetched libraries (currently: sdl2, sdl2_image, sdl2_ttf, udev, sdl3),
and `sandbox-libs/bootstrap.sh` restores them into the current sandbox's own filesystem in
seconds. Usage, full rationale, and gotchas already worked through (Debian -dev packages
shipping dangling .so symlinks that need the separate runtime package too; static link
ordering; why the install prefix is `/tmp/sandbox-libs-install` and not `/usr/local`; why sdl3
is a deliberately headless-only build) are in `sandbox-libs/ubuntu22-x86_64/NOTES.txt` and the
root `CLAUDE.md`'s "Sandbox build-dependency cache" section -- read those before adding a new
vendored library or troubleshooting a build that unexpectedly can't find one of these.

**Sandbox capability survey (confirmed 02-Jul-26):** Ubuntu 22.04.5, x86_64 only, gcc 11.4,
make and perf present; NO root/sudo (apt-get install fails, use the download trick above),
NO cmake, NO valgrind, NO ARM cross-compilers, NO qemu. github.com is reachable, so
building from source is viable. If cmake is needed (e.g. for SDL3 from source), install it
via `pip install cmake --break-system-packages` -- goes to the user site, no root needed.
Shell commands are capped at 45 seconds per call with no cwd/env carryover between calls;
long builds must be backgrounded (`nohup make ... &`) and polled from subsequent calls.

**Running the emulator headless for verification:** the sandbox has no display, but SDL's
dummy video driver (`SDL_VIDEODRIVER=dummy`) lets the emulator run without one. This enables
a real build-run-verify loop on x86: build against a user-prefix SDL (point PKG_CONFIG_PATH
at it), load an assembled tape, run, and inspect machine state / typewriter output / dumped
framebuffers. Hard limits: the panel driver (newpanel.c) compile-checks only -- no GPIO
hardware; nothing ARM-specific can execute; and perf numbers are x86-in-a-container --
useful for ranking algorithmic wins, not for Pi cycle counts. Pi-specific timing and
panel behavior must still be validated on real hardware.

**Sandbox gotcha: this Cowork sandbox's bash mount can serve stale, truncated reads of a file
just edited via the Read/Write/Edit tools, capped at the file's PRE-EDIT byte size.
It is specific to *paths bash had already looked up before the edit*;
a brand-new filename written via the file tools is read correctly by bash immediately.
Root cause looks like a stale cached file-size (`st_size`) attribute on the
bash side's mount of the shared folder that does not invalidate when the file tools (a separate
access path into the same underlying file) modify it, while the underlying page data itself
does refresh on access -- consistent with reads silently truncating at the old size, not serving
old content past that point.
**The reliable workaround:** write the corrected full
file content to a *new* filename in the same directory via the file tools (Write), then `mv` that
new file over the original from bash. The `mv` makes bash's view of the destination path inherit
the freshly-read source inode, which reads correctly immediately afterward.
If a from-source build mysteriously reports a
syntax error or unexpected EOF immediately after editing a source file via the file tools in a
Cowork session, suspect this before suspecting the edit itself.

**Sandbox gotcha: `sed -i` on the Windows filesystem mount zero-pads the file.**
The mount's in-place replacement rounds the output up to the next block boundary and
fills the gap with null bytes. The null bytes appear after the last real byte of content
and cause warnings like "null character(s) ignored" at compile time. Fix: strip with
`python3 -c "open(f,'wb').write(open(f,'rb').read().rstrip(b'\x00'))"` or avoid `sed -i`
entirely -- write to a temp file and `cp` back, or use the python `str.replace` approach.

**Sandbox gotcha: the stale-bash-mount-read issue above recurred and was confirmed again
(02-Jul-26, HSC conformance fix session)** on two files (`highSpeedChannels.c`,
`type340emu.c`) edited via the Edit tool, then compiled via bash. `wc -l`/`stat` on both files
kept reporting the exact pre-session byte count even minutes later and across multiple bash
calls (not just immediately after the edit) -- `sleep 2; sync` did not clear it either, so this
is not a short-lived cache that self-invalidates; treat it as sticky for the rest of the
session once a path has been looked up by bash. The documented workaround (Write the full
corrected content to a new filename, `mv` over the original from bash) resolved it cleanly
both times; both files then compiled without errors.

**Related new gotcha found in the same session: stale/partial build artifacts left behind by a
compile or link that failed mid-way (e.g. because of the stale-read issue above) can become
undeletable.** `rm` on the leftover `.o`/`.so` file returned `Operation not permitted`, and
`lsattr` on the same file returned `Operation not supported` -- even though `ls -la` showed
normal ownership and `-rwx------` permissions for the current user. Don't fight it with `rm`:
either let the compiler/linker overwrite the path directly (`gcc ... -o existing_file` succeeds
fine even when `rm` on that same path does not), or if `make` thinks a target is already up to
date because of a bad cached `.o`, `touch` the `.o` to force `make` to consider it stale and
relink, again writing over the existing output path rather than deleting it first.

**Undeletable-artifact gotcha confirmed again, more broadly, 03-Jul-26 (Phase 2 batch A/B
cleanup session):** asked to clean up ordinary (not failed-build) `.o`/`.so`/`.d` files and
compiled executables (`pdp1`, `newpanel`, `t30dpy`, `mkptyfio`, `mkptyfl`, a stray test
binary in `bin/`) after a normal successful verification pass. `rm`, `mv`, and Python's
`os.remove()` all failed identically with "Operation not permitted" on every one of them --
this is not limited to leftovers from a failed build, it is unlink() itself that does not
work on this mount for files the sandbox created, full stop. Even `git status` printed a
stray `warning: unable to unlink '.git/index.lock': Operation not permitted` in the same
session, suggesting this is a general property of the mount, not something specific to
build tooling. `lsattr` again reported "Operation not supported" (ruling out a chattr
immutable flag as the mechanism). **The only workaround found: truncate in place**
(`: > path/to/file` from bash, or any open()-with-O_TRUNC write) -- this succeeds and zeroes
the file's content, which is the actual concern (leftover binary/object content), even
though the directory entry and filename remain (true deletion is not available). Treat
"truncate every build/test artifact to 0 bytes" as the standing definition of "clean up"
on this sandbox until a real fix is found. See the "Standing rule" bullet under Hard
Constraints above for the policy this gotcha backs.

**Stale-bash-mount-read gotcha (see above) confirmed to recur MUCH more broadly than
originally scoped, 03-Jul-26 (Phase 2 batch C session):** the original write-up said this
was specific to *paths bash had already looked up before the edit*. That precondition does
NOT reliably predict when it happens. This session, four separate files
(`IOTs/Lib/filenames.c`, `IOTs/DCS2/IOT_22.c`, `IOTs/Type62and64Printer/IOT_45.c`,
`src/blincolnlights/pdp1/typtelnet.c`) all showed truncated/corrupted content under `gcc`
or `tail` immediately after being edited via the Edit/Write tools, and at least one of them
had no obvious prior bash lookup in the session before the edit that triggered it. One
instance was especially telling: `wc -c` and `wc -l` reported the CORRECT current size
and line count for a file, while `tail` (and separately `gcc`) read back content that cut
off mid-token partway through, with the tail output running directly into the next shell
command's output with no trailing newline -- i.e. the stat-level metadata (size) was fresh
but some read()s into the file still returned stale/incomplete page data. This means
`wc -c`/`stat` size checks are NOT a reliable way to confirm a file is fresh; do not treat
"the byte count looks right" as clearance to skip the workaround. **Also newly observed:
the staleness can affect a file's cached mtime, not just its content.** `make` (without
`-B`) silently skipped recompiling several changed `.c` files in this same session because
their stale cached mtime looked older than the (separately stale-zeroed, from the artifact
cleanup above) `.o` files already on disk, so nothing appeared to fail -- it just quietly
linked a binary from old objects. **Updated standing practice:** continue applying the
write-new-filename + `mv`-over-original workaround proactively after every edit to a file
a subsequent bash command will read, per the existing rule below, and do not treat a
clean `wc -l`/`wc -c`/`grep` check on that same path as sufficient proof of freshness --
those checks can themselves be fooled. When kicking off a rebuild after a multi-file edit
session, prefer `make -B` (or equivalent force-rebuild) over plain `make` at least once, to
route around the mtime-staleness variant without having to first prove which specific
files are affected.

**New variant found 03-Jul-26 (Phase 2 batch D session): `mv` itself can fail on a plain
source file, not just leftover build artifacts.** Moving a corrected `IOT_45.c` into place
via `mv newfile.c IOT_45.c` failed with `mv: inter-device move failed: ... unable to
remove target: Operation not permitted` -- the same "unlink doesn't work on this mount"
symptom documented above for `.o`/`.so` files, but this time on an ordinary tracked
source file being legitimately overwritten as part of the write-new-file+`mv` workaround
itself. **Workaround: use `cp` instead of `mv`** to land the corrected content (`cp
newfile.c IOT_45.c`) -- `cp` opens the destination and truncates/overwrites it rather than
unlinking it first, so it succeeds where `mv` does not, and a subsequent `ls -la
--time-style=full-iso` confirmed a fresh mtime. Treat `cp`, not `mv`, as the default final
step of the write-new-file workaround from now on; only fall back to `mv` if `cp` itself
is somehow unavailable.

**Sandbox gotcha: every file on this Windows-host mount reports a fake, uniform `0700`
permission (`rwx------`), regardless of file type (04-Jul-26).** Confirmed via `mount`
(the mount is `type fuse`, bridging the Win 11 host folder into the Linux sandbox) and
`stat` on several files of different types (`.c`, `.md`) -- all came back identically
`0700`. This is not a translation of any real Windows ACL; NTFS has no POSIX mode bits to
translate in the first place, so the FUSE bridge just returns a constant synthetic mode
for every regular file it exposes, execute bit included. This matters for `tar`: `tar`
faithfully preserves whatever `stat()` reports, so a `.tar` built directly from this mount
bakes in `rwx------` (no group/other access at all) for every file, including plain source
files that should be ordinary `0644`. On extraction elsewhere (e.g. the owner's Windows
test machine) files come out execute-bit-set with group/other access lost. **Workaround:
override the mode at tar-creation time rather than trusting this mount's `stat()` --
`tar --mode='0644' -cvf archive.tar <files...>` (confirmed working) forces a sane mode into
the archive regardless of what the bridge reports.** Use this whenever building a `.tar`
of files from this mount that the owner will extract on another machine.

**Sandbox gotcha: a file written from the Linux sandbox side (via the bash tool, e.g. `cp`)
can show its OLD, pre-write mtime on the Windows host side, even though the content is correct
and the Linux side's own `stat()` already reports the fresh mtime (04-Jul-26).** Found after
patching `src/blincolnlights/art/pdp1_panel.png` and regenerating `art/pdp1art.inc` via bash
`cp` (no `-p`, so a fresh mtime should apply). Checked from the sandbox side immediately after:
`stat` on both files correctly showed the actual write time, while a third, untouched file in
the same directory (`panelart.inc`) correctly still showed its original (months-old) mtime --
i.e. the Linux-side view was entirely correct and consistent. The project owner, checking the
same file on their actual Windows machine afterward, saw the OPPOSITE problem: `pdp1art.inc`'s
timestamp still showed the old pre-edit date despite the content being confirmed correct (the
new panel label rendered properly), while `panelart.inc`, which the owner touched directly on
Windows, correctly showed "now". This is a new, more expansive variant of the stale-metadata
family of gotchas already documented above (stale bash-side content reads, stale bash-side
mtimes causing `make` to skip rebuilds) -- this is the first confirmed case of the staleness
crossing all the way out to the owner's real Windows filesystem view, specifically for mtime,
after a write made from the Linux sandbox side. Cosmetic only (content is unaffected), but
worth remembering: don't trust a Windows-side timestamp alone to confirm a sandbox-made edit
landed, and warn the owner that a future hand-edit relying on `make`'s mtime-based rebuild
decision could be silently skipped if the timestamp is stuck old -- `touch` the file (confirmed
by the owner to fix the displayed timestamp) or use `make -B` as a safety net.
The owner has filed a bug report upstream about this whole family of staleness issues
(content, mtime, undeletable artifacts) -- if a fix lands, re-test before assuming these
workarounds are still needed. Until then, don't rely on Windows-side file dates to confirm
which files changed in a session -- state it explicitly instead (see standing practice note
directly below).

**Standing practice (04-Jul-26): always state explicitly, in plain text at the end of a
session, exactly which files were created/modified/deleted.** The owner normally identifies
changed files by sorting the folder by date, and the mtime-staleness gotcha above breaks that
workflow silently -- a changed file can appear to have an untouched, months-old date. Do not
rely on the owner being able to spot changes via file dates or folder sorting; call out changed
paths by name every time, regardless of whether this gotcha seems to be in play that session.


## C Coding Standard (established by project owner, recorded 22-Jun-2026)
Governs all C source written for this project -- emulator core, IOT plugins, and any other
C support code -- distinct from the "Claude/ files must be plain ASCII" rule above, which is
scoped only to this documentation hierarchy, not to project source.

### Target environment and stability priorities
- Language/toolchain: ISO C matching modern GCC default behavior. Procedural/structural hygiene.
- Architecture/OS: cross-platform Linux (Intel/AMD x86_64 and ARM, Pi 4 and above).
- Optimization vector: prioritize correctness, strict predictability, and structural hygiene
  over micro-optimizations.

### Lifecycle and traceability
- Iterative design: step-by-step logic expansion with clear validation checkpoints.
- Design mapping: all code must explicitly map to conceptual modules, state machines, or
  design requirements.

### Commentary mandate
- No "self-documenting" assumptions: comprehensive textual explanations are mandatory.
- File/module headers must include purpose, architectural scope, dependencies, and execution
  model.
- Function contracts: block comments defining preconditions, postconditions, arguments, return
  behaviors, and edge-case handling.
- Function comment style (added 19-Jun-2026): every function definition must be preceded by a
  comment block using `//` line comments (not `/* */`), giving a summary of what the function
  does. If the function returns a value, the comment must explicitly describe what the return
  value means (including the meaning of each distinct return value, e.g. 0 vs. non-zero, or
  what a returned pointer refers to and when it can be NULL/0). Void functions need the
  summary only.
- do not use -- or em-dash in comments, use a comma or start a new sentence.
- break in comments at sentence ends.
- Inline logic: granular commentary detailing the "why" and "how" of branches, state changes,
  math steps, and pointer manipulation.

### Defensive syntax and formatting
- Indent with 4 spaces, no tabs in text.
- Grouping operators: mandatory parenthetical containment `( )` for arithmetic, logical, and
  bitwise operations to completely isolate precedence bugs. This extends to address-of on
  struct members: write `&(ptr->field)` not `&ptr->field` -- the parentheses make the
  member access explicit and independent of implied operator precedence.
- Control flow blocks: explicit braces `{ }` for all control constructs (`if`, `while`, `for`,
  `do`), with braces positioned on separate lines from the body.
- Function calls in conditionals are permitted for common variable assignments where the
  result can be tested in an obvious, explicit manner (e.g., `if( !(fP = getFile(...)) )`).
- No space between a directive/control keyword and its opening parenthesis (e.g., `if(`,
  `while(`, `switch(`).
- Variable scope declarations: globals declared strictly at the beginning of the file; locals
  declared strictly at the beginning of the function body; explicit separation of declaration
  and initialization in most cases.
- Naming: camel-hump naming convention.
- Pointers: C-style pointer declarations (`char *ptrP`) with a trailing Systems Hungarian
  Notation suffix matching the pointer type (`P` or `ptr`).

### File layout (loose scoping order)
1. Comprehensive file block header
2. Include files (`#include`)
3. Preprocessor defines (`#define`)
4. Global variables (grouped cleanly by data type)
5. Function declarations/definitions

## Deployment note: symbol-exports.ldr must travel with any dlopen'd-plugin-visible C change
`src/blincolnlights/pdp1/symbol-exports.ldr` is the `-Wl,--dynamic-list` file that controls
which of the emulator binary's own symbols are visible for IOT plugins (dlopen'd .so files)
to resolve against. If a plugin calls a function in the emulator binary (e.g.
`HSCfreeChannel()` in `highSpeedChannels.c`, called from `IOTs/TestGateway/IOT_44.c`) that
isn't listed in `symbol-exports.ldr`, the plugin still `dlopen()`s fine (RTLD_LAZY), and any
symbol that IS listed still resolves fine -- but the FIRST actual call to the missing symbol
kills the process with "undefined symbol" at that call site, not at load time. This is easy
to source-scope out of a narrow file-list deliverable (03-Jul-26: a tar built from `git
status` filtered to `.c`/`.h`/`.am1`/Makefile changes only omitted `symbol-exports.ldr`,
which was also modified but doesn't match that extension filter, and the receiving machine's
pdp1 binary died the first time a test program actually called `HSCfreeChannel()` --
initially looked exactly like a hang/heap-corruption bug in the HSC code, wasted a
diagnostic-instrumentation round before the real cause -- a missing export, not a code bug
-- was found). Rule of thumb: any deliverable/tar of changes involving a NEW function a
dynamic IOT plugin calls into the main binary must include `symbol-exports.ldr` even when
scoping to "source files only" -- check `git diff --stat` for it explicitly, don't rely on
an extension filter to catch it.

## Key Files
| File | Role |
|------|------|
| `src/blincolnlights/pdp1/pdp1.h` | PDP1 struct definition; shared by emulator and all IOT plugins |
| `src/blincolnlights/pdp1/pdp1.c` | Core emulator: `cycle()`, `cycle0()`, `defer()`, `cycle1()`, `brkcycle()` |
| `src/blincolnlights/pdp1/panel1.c` | `updatelights()` / `updatelights_pwm()` -- panel register snapshot and PWM tally |
| `src/blincolnlights/pdp1/main.c` | Main loop; halt-state and DMA-steal must each call `updatelights()` + `updatelights_pwm(panel, 1)` |
| `src/blincolnlights/pdp1/display.c` | Type 30 display worker thread; `setDisplayData()` / `getDisplayData()` |
| `src/blincolnlights/panel/driver/newpanel.c` | Standalone panel driver process (GPIO row scan + PWM) |
| `src/blincolnlights/panel/gpio/gpiochip_rp1.c` | Pi 5 (RP1) GPIO register access, incl. the atomic SET/CLR fix |
| `IOTs/iotHandler.h` | IOT plugin interface; includes `pdp1.h` -- ABI-sensitive |
| `IOTs/Type30Display/IOT_7.c` | dpy IOT handler |
| `IOTs/Type30Display/Makefile` | Builds `IOT_7.so`; must be run after any `pdp1.h` change |

## Nested CLAUDE.md files (established pattern, 02-Jul-26)
For deep, subsystem-specific knowledge that's only needed once you're actually working in that
part of the tree, this project uses nested `CLAUDE.md` files colocated with the code they
describe, instead of growing this file without bound. Claude Code loads a nested `CLAUDE.md`
automatically the moment it reads any file in that directory -- no manual pointer needed. Keep
using this pattern going forward: when adding non-trivial subsystem-specific findings, prefer
a nested `CLAUDE.md` in the relevant code directory over appending to this file, unless the
fact is genuinely cross-cutting (needed before you'd even know to look in that directory, or
spanning multiple unrelated directories) or is a hard constraint -- those stay here.

Existing nested files:
- `src/blincolnlights/pdp1/CLAUDE.md` -- pdp1.c/panel1.c/display.c/main.c/highSpeedChannels.c
  internals: display IOT coordinate source, the panel driver interface and the opaque-`Panel*`-
  in-pdp1.c gotcha, machine cycle structure, typewriter (tyi/tyo/szf/clf) behavior, the
  sho/shro pipeline, the PDP-1D extended-architecture research, the BBN timesharing research
  summary, and the HSCwait() usleep()-is-a-scheduler-jitter-source finding (full analysis in
  the Type340Display nested file below).
- `src/blincolnlights/panel/driver/CLAUDE.md` -- newpanel.c internals: the panelthread/pwmthread
  architecture, the `-r`/`-t` flags and why `-r` is recommended, the CPU-affinity mitigation
  that was tried and removed, and RP1/Pi 5 GPIO hardware context (PCIe latency, why PIO was
  investigated and rejected).
- `IOTs/Type340Display/CLAUDE.md` -- type340emu.c/IOT_15-17.c internals: root-cause analysis
  of Type 340 display jitter (uncached `getWord()` -> `HSCexecute()`+`HSCwait()` ->
  `usleep()` -> Linux scheduler jitter), why the `t340cachesize` instruction cache was added
  specifically to work around it, the empirical (not derived) cache-size threshold found for
  `type340demo.am1`, the hard constraint that the simulated 5us HSC delay must stay as
  historically accurate as reasonable, and the `LOG_HSCTIMING` instrumentation added to
  measure actual per-word fetch latency.
- `Claude/SupportCode/CLAUDE.md` -- the automated headless test harness (added 09-Jul-26):
  ad1_driver.py/display_client.py/pdp1_harness.py, the run_in_opt.sh /opt/pidp1-mods
  mount-namespace workaround, the makepanel.c panel-segment-reset requirement, and every
  gotcha found getting a from-scratch pidp1+ad1 run working in this sandbox (leftover
  coremem/panel state between runs, power_sw gating the whole cycle loop, ad1's stdout
  buffering quirk, the Type 340 display wire protocol, and a Python __pycache__ staleness
  variant of the sandbox's stale-mount-read gotcha).

This is in addition to, not instead of, the root `CLAUDE.md`'s indirection to this `Claude/`
directory -- that indirection is intentional and stays as-is (confirmed 02-Jul-26). The
three-level structure is: root `CLAUDE.md` (always loaded) -> `Claude/CLAUDE.md` (read on the
root file's instruction) -> nested `CLAUDE.md` files in code directories (auto-loaded by
Claude Code when that code is touched, no instruction needed).
