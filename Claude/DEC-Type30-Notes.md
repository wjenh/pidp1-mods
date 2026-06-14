# DEC Type 30 Emulation — Session Notes

Carried over from a Claude Project chat ("Pi5 display performance
observation"), prior to migrating to a Linux-based Claude client. Covers the
`Tools/T30dpy/` Windows port and a related `display.c` socket bug fix.

## Windows port of t30dpy / t30dpy3 (Tools/T30dpy/)

Goal: build native Windows .exe versions of the Type 30 display client.

- **New files**: `wincompat.h` / `wincompat.c` — Windows-only shims:
  - Winsock init/cleanup
  - `SOCKREAD`/`SOCKWRITE`/`SOCKCLOSE` macros (map to `recv`/`send`/`closesocket`
    on Windows, plain `read`/`write`/`close` no-ops on Linux)
  - `SIGHUP` → `SIGBREAK`
  - `winGetHomeDir()` for `~/...` config path resolution
  - (Initial `winNow()`/`winNanosleep()` based on QueryPerformanceCounter were
    later **removed** — UCRT64 provides `clock_gettime`/`nanosleep`/
    `CLOCK_MONOTONIC` natively; the custom shims conflicted.)

- **`t30dpy.c` / `t30dpy3.c`**: Single source files, Windows-specific code
  entirely behind `#ifdef _WIN32` / `#ifndef _WIN32`. On Linux, all macros
  collapse to the original calls — **no behavior change on Linux**. Covers
  includes, socket I/O, signal cleanup, `getFile()` pwd.h fallback, Winsock
  startup/cleanup, `now()`.

- **`Win/Makefile`** (new): builds both `.exe`s under MSYS2 UCRT64 via
  pkg-config for SDL2/SDL3, linking `-lws2_32`. Includes the pacman package
  list needed. Linux Makefile untouched.

### Build fixes applied (from error logs)

- Added `<getopt.h>` — mingw's `<unistd.h>` doesn't declare
  `getopt`/`optarg`/`optind`.
- Removed `CLOCK_MONOTONIC`/`clock_gettime`/`nanosleep` shims (conflicted with
  native UCRT64 support).
- Cast `setsockopt`'s `optval` to `(const char *)` for Winsock (both files).
- `%lu`/`uint64_t` format warnings are pre-existing, non-fatal, left alone.
- **Makefile gotcha**: running `make t30dpy` (no `.exe`, habit from Linux
  Makefile) fell through to make's builtin bare-`cc` link rule and produced
  missing-library errors, since only `t30dpy.exe` was defined as a target.
  Fix: added `t30dpy`/`t30dpy3` as `.PHONY` aliases to the `.exe` targets.
  If stray `t30dpy`/`t30dpy3` files exist from a failed link, `rm -f` them
  first.

### Result

Build succeeded under MSYS2 UCRT64; confirmed working by user.

### Static build (in progress / partially blocked)

- User wants a fully self-contained `.exe` (no MSYS2/DLLs needed) — Windows
  equivalent of static linking.
- Added `make static` target → `t30dpy-static.exe` / `t30dpy3-static.exe`,
  linked with `-static` + `pkg-config --static --libs sdl2/sdl3` (pulls in
  winmm, imm32, setupapi, version, ole32, etc.).
- **Caveat noted**: SDL3's static pkg-config support is newer/less mature than
  SDL2's — possible missing-symbol errors.
- **Blocked**: SDL3 is not available via MSYS2 packages, and user doesn't want
  to build SDL3 from source. So SDL3 static build is deferred until the
  package becomes available. `make` (dynamic) and `make t30dpy-static.exe`
  (SDL2) should both work without SDL3.

## display.c socket bug fix (src/blincolnlights/pdp1/display.c)

Found while testing the Windows t30dpy client against the remote display
server — connection terminated abruptly over a remote/slower link (also seen
on Linux clients, not just Windows).

- **Root cause**: server-side `display.c` socket is opened **blocking** via
  `serveN()` in `common.c` (original code, runs in its own thread). But
  `display.c` separately sets the fd non-blocking for the lightpen read
  (`fcntl(..., O_NONBLOCK)`). `flushDisplay()` treated *any* short `write()`
  return — including `-1`/EAGAIN (errno 11) from the non-blocking fd — as
  fatal and closed the connection. EAGAIN simply meant the send buffer was
  full because data was being written faster than the socket could drain it
  (only shows up on slower/remote links, never same-machine).

- **Fix applied** in `display.c`:
  - EAGAIN/EWOULDBLOCK/EINTR are now non-fatal: buffered commands stay queued
    and `flushDisplay()` retries later.
  - A genuine partial write shifts unsent commands to the front of `dpyBuf[]`
    instead of discarding them.
  - `addDpyCommand()` loops (bounded by `DPYFULLRETRIES`, ~30µs each) when the
    buffer is full and a flush doesn't drain it — instead of the old
    single-shot flush that could overflow `dpyBuf[]`.
  - If still stuck after ~3ms of retries, the command is dropped (logged) —
    not a connection close.
  - Only genuine `write()` errors (not EAGAIN) close the fd now.
  - A logger() line and revision history entry were added.

- **Result**: confirmed fixed by user ("Problem seems fixed").

- Considered and rejected as primary fix: increasing `READBUFSIZE` (512
  uint32_t = 2KB) on the t30dpy client side — would help marginally but is a
  band-aid; doesn't address the root EAGAIN-handling bug. A separate
  light-pen-read thread was also considered but deemed unnecessary — the
  non-blocking fd design was fine, only the error handling was wrong.

## Status at handoff

- Windows port working (dynamic build confirmed).
- display.c socket fix confirmed working.
- Static build for t30dpy (SDL2) available; t30dpy3 static (SDL3) deferred
  pending SDL3 MSYS2 package availability.
