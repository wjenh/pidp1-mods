GCC=gcc -O3 -march=native -Wall -Wextra -Wno-unused-result -Wno-implicit-fallthrough -Wno-switch \
    -Wno-unused-parameter
#GCC=gcc -g -Og -march=native -Wall -Wextra -Wno-unused-result -Wno-implicit-fallthrough -Wno-switch \
    -fsanitize=address,undefined
#GCC=gcc -g -Og -march=native -Wall -Wextra -Wno-unused-result -Wno-implicit-fallthrough -Wno-switch

# Build only the versions whose SDL is present, and install the SDL3 one as t30dpy when it is.
# pkg-config is used, as in the emulator's Makefile. Without pkg-config SDL3 can't be detected,
# so this falls back to the SDL2 version, which is what the emulator does too.
# Either can be forced from the command line, e.g. "make HAVE_SDL3= install" installs SDL2's.
HAVE_PKGCONFIG := $(shell command -v pkg-config >/dev/null 2>&1 && echo 1)
ifeq ($(HAVE_PKGCONFIG),1)
HAVE_SDL2 := $(shell pkg-config --exists sdl2 2>/dev/null && echo 1)
HAVE_SDL3 := $(shell pkg-config --exists sdl3 2>/dev/null && echo 1)
else
HAVE_SDL2 := 1
endif

TARGETS=
ifeq ($(HAVE_SDL2),1)
TARGETS+=t30dpy
endif
ifeq ($(HAVE_SDL3),1)
TARGETS+=t30dpy3
endif

ifeq ($(strip $(TARGETS)),)
all install:
	@echo "Neither SDL2 nor SDL3 development files were found, t30dpy can't be built."
	@exit 1
else
all: $(TARGETS)

ifeq ($(HAVE_SDL3),1)
install: installdpy3
else
install: t30dpy
	cp t30dpy ../../bin
	strip ../../bin/t30dpy
endif
endif

t30dpy:	t30dpy.o
	$(GCC) $^ -o t30dpy -lSDL2 -lSDL2main -lm

t30dpy.o: t30dpy.c wincompat.h
	$(GCC) -c t30dpy.c

t30dpy3:	t30dpy3.o
	$(GCC) $^ -o t30dpy3 -lSDL3 -lm

t30dpy3.o: t30dpy3.c wincompat.h
	$(GCC) -c t30dpy3.c

dpytester:	dpytester.o
	$(GCC) $^ -o dpytester

dpytester.o: dpytester.c
	$(GCC) -c dpytester.c

clean:
	@rm *.o t30dpy t30dpy3 dpytester 2>/dev/null || true

# The SDL3 version, installed under the SDL2 version's name so everything that starts t30dpy gets it.
installdpy3: t30dpy3
	cp t30dpy3 ../../bin/t30dpy
	strip ../../bin/t30dpy

.PHONY: all install installdpy3 clean
