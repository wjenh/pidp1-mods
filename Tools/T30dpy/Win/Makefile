# Makefile for building t30dpy and t30dpy3 on Windows 11 under MSYS2 UCRT64.
#
# NOTE that SDL3 libs don't seem to be available yet for UCRT64, so the t30dpy3 build is removed for now in
# the all target.
#
# This builds the SAME source files used on Linux (../t30dpy.c, ../t30dpy3.c),
# plus the Windows compatibility shim ../wincompat.c. The Windows-specific code
# in those files is selected automatically via the _WIN32 macro, which the
# UCRT64 gcc defines on its own - no extra -D flags are needed.
#
# Prerequisites (run once, from an MSYS2 UCRT64 shell - NOT mingw32/mingw64/msys):
#   pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-pkg-config \
#             mingw-w64-ucrt-x86_64-SDL2 make
# When SDL3 is available:
#   pacman -S mingw-w64-ucrt-x86_64-SDL3
#
# Build (from this Win/ directory, in the UCRT64 shell):
#   make
#
# This produces t30dpy.exe in this directory.
# These are statically linked and can be copied to any Windows 11 machine and
# run directly.

GCC=gcc -O2 -Wall -Wextra -Wno-unused-result -Wno-implicit-fallthrough -Wno-switch \
    -Wno-unused-parameter

SDL2_CFLAGS=$(shell pkg-config --cflags sdl2)
SDL2_LIBS=$(shell pkg-config --libs sdl2)
SDL2_STATIC_LIBS=$(shell pkg-config --static --libs sdl2)

SDL3_CFLAGS=$(shell pkg-config --cflags sdl3)
SDL3_LIBS=$(shell pkg-config --libs sdl3)
SDL3_STATIC_LIBS=$(shell pkg-config --static --libs sdl3)

# -lws2_32 supplies WSAStartup/WSACleanup/socket/recv/send/etc (Winsock2).
WINLIBS=-lws2_32

# -static links libgcc, winpthread and the UCRT runtime into the executable
# too, on top of pkg-config's static SDL2/SDL3 (which already pull in the
# extra Windows system libs like winmm/imm32/setupapi/version/ole32 they need).
STATICFLAGS=-static

all: t30dpy.exe

# same target names as the Linux version for convenience
t30dpy: t30dpy.exe
t30dpy3: t30dpy3.exe

t30dpy.o: ../t30dpy.c
	$(GCC) $(SDL2_CFLAGS) -I.. -c ../t30dpy.c -o t30dpy.o

t30dpy3.o: ../t30dpy3.c
	$(GCC) $(SDL3_CFLAGS) -I.. -c ../t30dpy3.c -o t30dpy3.o

wincompat.o: ../wincompat.c ../wincompat.h
	$(GCC) $(SDL2_CFLAGS) -I.. -c ../wincompat.c -o wincompat.o

t30dpy.exe: t30dpy.o wincompat.o
	$(GCC) $(STATICFLAGS) $^ -o t30dpy-static.exe $(SDL2_STATIC_LIBS) $(WINLIBS) -lm

t30dpy3.exe: t30dpy3.o wincompat.o
	$(GCC) $(STATICFLAGS) $^ -o t30dpy3-static.exe $(SDL3_STATIC_LIBS) $(WINLIBS) -lm

clean:
	@rm -f *.o t30dpy.exe t30dpy3.exe 2>/dev/null || true
