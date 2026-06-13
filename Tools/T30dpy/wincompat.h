/*
 * wincompat.h - Windows 11 (MSYS2/UCRT64) portability shims for t30dpy/t30dpy3.
 *
 * PURPOSE:
 * t30dpy.c and t30dpy3.c are written against the POSIX/BSD APIs available on Linux
 * (sockets, signals, clock_gettime/nanosleep, pwd.h home-directory lookup).
 * This header lets those same source files build unchanged on both platforms by
 * routing the few non-portable calls through small macros/wrappers, selected at
 * compile time via the standard predefined macro _WIN32 (defined automatically
 * by mingw-w64/UCRT64, MSVC, etc - no special -D flag is required).
 *
 * ARCHITECTURAL SCOPE:
 * Included once near the top of each .c file, after the standard headers.
 * On Linux this header is almost entirely a no-op (the native calls are used directly).
 * On Windows it:
 *   - pulls in winsock2.h/ws2tcpip.h for the socket API,
 *   - maps SOCKREAD/SOCKWRITE/SOCKCLOSE onto recv/send/closesocket,
 *   - maps SIGHUP onto SIGBREAK (closest Windows analog - delivered on Ctrl-Break),
 *   - pulls in <getopt.h> for getopt()/optarg/optind (not declared by <unistd.h> here),
 *   - provides winGetHomeDir() to stand in for the pwd.h-based home directory lookup.
 * Note: clock_gettime(CLOCK_MONOTONIC, ...) and nanosleep() are provided natively by
 * the UCRT64 mingw-w64 headers (via pthread_time.h), so no shim is needed for those.
 *
 * EXECUTION MODEL:
 * winSockStartup()/winSockCleanup() must be called once each, at program start and
 * exit respectively, on Windows only (Winsock requires WSAStartup before any socket
 * call). On Linux these are no-ops and may be called unconditionally.
 *
 * 13-Jun-2026 wje (Claude) initial version, part of the Windows 11 port.
 */

#ifndef WINCOMPAT_H
#define WINCOMPAT_H

#include <time.h>

#ifdef _WIN32

// Winsock2 must be included before windows.h (which SDL headers may pull in)
// to avoid the classic winsock.h/winsock2.h conflict.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

// <unistd.h> on mingw-w64 does not declare getopt()/optarg/optind; <getopt.h> does.
#include <getopt.h>

// ---- Socket I/O -----------------------------------------------------------
// Winsock has no read()/write()/close() for sockets - it uses recv()/send()/
// closesocket(). The cast on send() matches the (const char *) buffer type
// Winsock expects; the underlying buffers here are uint32_t arrays.
#define SOCKREAD(fd, bufP, len)  recv((fd), (char *)(bufP), (int)(len), 0)
#define SOCKWRITE(fd, bufP, len) send((fd), (const char *)(bufP), (int)(len), 0)
#define SOCKCLOSE(fd)            closesocket(fd)

// ---- Signals ----------------------------------------------------------------
// Windows has no SIGHUP. SIGBREAK (delivered for Ctrl-Break) is the closest
// available analog for "please reload your configuration".
#ifndef SIGHUP
#define SIGHUP SIGBREAK
#endif

// ---- Home directory lookup --------------------------------------------------
// pwd.h/getpwuid()/getpwnam() do not exist on Windows. winGetHomeDir() returns
// the user's profile directory (from %USERPROFILE%, falling back to
// %HOMEDRIVE%%HOMEPATH%), used by getFile() for "~/..." path expansion.
const char *winGetHomeDir(void);

// ---- Winsock lifecycle -------------------------------------------------------
int winSockStartup(void);
void winSockCleanup(void);

#else /* !_WIN32 */

// On Linux, winSockStartup()/winSockCleanup() are harmless no-ops so callers
// can invoke them unconditionally without #ifdef clutter at the call site.
#define winSockStartup() (0)
#define winSockCleanup() ((void)0)

#define SOCKREAD(fd, bufP, len)  read((fd), (bufP), (len))
#define SOCKWRITE(fd, bufP, len) write((fd), (bufP), (len))
#define SOCKCLOSE(fd)            close(fd)

#endif /* _WIN32 */

#endif /* WINCOMPAT_H */
