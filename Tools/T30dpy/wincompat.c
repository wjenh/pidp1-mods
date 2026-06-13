/*
 * wincompat.c - Windows 11 (MSYS2/UCRT64) portability shims for t30dpy/t30dpy3.
 *
 * PURPOSE:
 * Implements the Windows-only helper functions declared in wincompat.h:
 * winSockStartup()/winSockCleanup() (Winsock lifecycle), winNow()/winNanosleep()
 * (monotonic clock and sleep, replacing clock_gettime/nanosleep), and
 * winGetHomeDir() (home directory lookup, replacing pwd.h).
 *
 * This entire file compiles to nothing on Linux - it exists only so the Windows
 * build has a translation unit to hold these definitions. The Makefile in Win/
 * is expected to compile and link this file only for the Windows target.
 *
 * 13-Jun-2026 wje (Claude) initial version, part of the Windows 11 port.
 */

#include "wincompat.h"

#ifdef _WIN32

#include <stdio.h>

// ---------------------------------------------------------------------------
// winSockStartup()
// Preconditions: none. Must be called once before any socket call.
// Postconditions: Winsock 2.2 is initialized. Returns 0 on success, -1 on
// failure (matching the conventional Linux "no setup needed, can't fail" of 0
// for the no-op case while still allowing the Windows caller to detect errors).
// ---------------------------------------------------------------------------
int
winSockStartup(void)
{
WSADATA wsaData;

    if( WSAStartup(MAKEWORD(2, 2), &wsaData) != 0 )
    {
        return(-1);
    }
    return(0);
}

// ---------------------------------------------------------------------------
// winSockCleanup()
// Preconditions: winSockStartup() previously succeeded.
// Postconditions: releases Winsock resources. No return value, mirrors the
// fact that the Linux no-op version also returns nothing useful.
// ---------------------------------------------------------------------------
void
winSockCleanup(void)
{
    WSACleanup();
}

// ---------------------------------------------------------------------------
// winGetHomeDir()
// Preconditions: none.
// Postconditions: returns a pointer to a static buffer containing the user's
// home/profile directory path, or an empty string if none of the expected
// environment variables are set. The returned pointer remains valid until the
// next call (matches the simple "static buffer" style used by getenv()).
// ---------------------------------------------------------------------------
const char *
winGetHomeDir(void)
{
static char homeDir[MAX_PATH];
const char *userProfileP;
const char *homeDriveP;
const char *homePathP;

    if( (userProfileP = getenv("USERPROFILE")) )
    {
        snprintf(homeDir, sizeof(homeDir), "%s", userProfileP);
        return(homeDir);
    }

    // Fall back to HOMEDRIVE+HOMEPATH, the older convention some environments
    // (and some MSYS2 shells) still set instead of USERPROFILE.
    homeDriveP = getenv("HOMEDRIVE");
    homePathP = getenv("HOMEPATH");
    if( homeDriveP && homePathP )
    {
        snprintf(homeDir, sizeof(homeDir), "%s%s", homeDriveP, homePathP);
        return(homeDir);
    }

    homeDir[0] = '\0';
    return(homeDir);
}

#endif /* _WIN32 */
