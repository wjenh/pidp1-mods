// The one compile unit holding Nuklear's implementation and its SDL renderer's, so an edit to
// the front end does not recompile Nuklear. The NK_INCLUDE_* options come from the Makefile,
// since every unit that includes nuklear.h must see the same ones.

#define NK_IMPLEMENTATION
#define NK_SDL_RENDERER_IMPLEMENTATION

#include <SDL.h>

#include "nuklear.h"
#include "nuklear_sdl_renderer.h"
