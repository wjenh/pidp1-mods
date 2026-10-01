#ifndef STYLE_H
#define STYLE_H
// pdp1central's color schemes. A scheme is Nuklear's color table plus the colors pdp1central
// draws with itself, kept in one array: Nuklear's entries first, in its own order, then ours.
// Every unit that includes this must be built with the Makefile's NK_INCLUDE_* options.

#include <stdbool.h>

#include "nuklear.h"

#include "core.h"

#define STYLE_DEFAULT "light"   // the scheme used when pdp1central.config names none

// pdp1central's own entries, numbered after Nuklear's table.
enum
{
    STYLE_LAMP_ON = NK_COLOR_COUNT, // a status lamp that is lit
    STYLE_LAMP_OFF,
    STYLE_WARNING,                  // notes and errors in the top strip, and other warnings
    STYLE_CHANGED,                  // an unsaved setting's name, and the "saved" note
    STYLE_TOGGLE_CURSOR_HOVER,      // a check mark or selected option under the mouse
    STYLE_BUTTON_TEXT,              // a button's label, where Nuklear uses the one text color
    STYLE_BUTTON_TEXT_HOVER,
    STYLE_BUTTON_TEXT_ACTIVE,
    STYLE_DISABLED_BUTTON,          // a widget that cannot be used: its face,
    STYLE_DISABLED_TEXT,            // its label and mark,
    STYLE_DISABLED_BORDER,          // and its edge
    STYLE_COUNT
};

// The scheme in use, with any overrides; read it, change it only through styleUse or styleLoad.
extern struct nk_color styleColors[STYLE_COUNT];

const char *styleSchemeName(int index);
int styleCurrent(void);
const char *styleEntryName(int entry, char *bufP, size_t bufLen);
bool styleUse(struct nk_context *ctxP, const char *nameP);
int styleLoad(struct nk_context *ctxP, ConfFile *cfP, void (*logP)(const char *fmtP, ...));
void styleDisableBegin(struct nk_context *ctxP);
void styleDisableEnd(struct nk_context *ctxP);

#endif
