// pdp1central's color schemes: the built-in tables, the reader for pdp1central.config (which
// scheme, and overrides of single colors), and building Nuklear's style from the result.
// Nuklear draws everything from its 32-color table (nk_style_from_table); pdp1central's own
// colors (lamps, notes, disabled widgets) follow it in the same array, so a scheme is one table
// and a change of scheme is one call between frames.
// Disabled widgets: Nuklear draws them by multiplying their colors by a factor of 0.5, which
// always darkens, so on a light scheme a disabled button would look heavier than a live one.
// Here the factor is 1.0 and a disabled widget is drawn in the scheme's disabled colors instead.

#include <stdio.h>
#include <string.h>

#include "style.h"

#define RGB(r, g, b) { (r), (g), (b), 255 }

typedef struct
{
    const char *nameP;
    struct nk_color colors[STYLE_COUNT];
} ColorScheme;

// The dark scheme is Nuklear's default table with two changes. Button hover and press are
// lighter, not darker, so a live state does not read as disabled; and the check mark is bright,
// since Nuklear's draws a checked box darker than an unchecked one, which reads backwards. Its
// disabled colors are what Nuklear's factor of 0.5 gave.
static const ColorScheme schemes[] =
{
    {
        "dark",
        {
            [NK_COLOR_TEXT] = RGB(175, 175, 175),
            [NK_COLOR_WINDOW] = RGB(45, 45, 45),
            [NK_COLOR_HEADER] = RGB(40, 40, 40),
            [NK_COLOR_BORDER] = RGB(65, 65, 65),
            [NK_COLOR_BUTTON] = RGB(50, 50, 50),
            [NK_COLOR_BUTTON_HOVER] = RGB(72, 72, 72),
            [NK_COLOR_BUTTON_ACTIVE] = RGB(95, 95, 95),
            [NK_COLOR_TOGGLE] = RGB(100, 100, 100),
            [NK_COLOR_TOGGLE_HOVER] = RGB(120, 120, 120),
            [NK_COLOR_TOGGLE_CURSOR] = RGB(225, 225, 225),
            [NK_COLOR_SELECT] = RGB(45, 45, 45),
            [NK_COLOR_SELECT_ACTIVE] = RGB(35, 35, 35),
            [NK_COLOR_SLIDER] = RGB(38, 38, 38),
            [NK_COLOR_SLIDER_CURSOR] = RGB(100, 100, 100),
            [NK_COLOR_SLIDER_CURSOR_HOVER] = RGB(120, 120, 120),
            [NK_COLOR_SLIDER_CURSOR_ACTIVE] = RGB(150, 150, 150),
            [NK_COLOR_PROPERTY] = RGB(38, 38, 38),
            [NK_COLOR_EDIT] = RGB(38, 38, 38),
            [NK_COLOR_EDIT_CURSOR] = RGB(175, 175, 175),
            [NK_COLOR_COMBO] = RGB(45, 45, 45),
            [NK_COLOR_CHART] = RGB(120, 120, 120),
            [NK_COLOR_CHART_COLOR] = RGB(45, 45, 45),
            [NK_COLOR_CHART_COLOR_HIGHLIGHT] = RGB(255, 0, 0),
            [NK_COLOR_SCROLLBAR] = RGB(40, 40, 40),
            [NK_COLOR_SCROLLBAR_CURSOR] = RGB(100, 100, 100),
            [NK_COLOR_SCROLLBAR_CURSOR_HOVER] = RGB(120, 120, 120),
            [NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = RGB(150, 150, 150),
            [NK_COLOR_TAB_HEADER] = RGB(40, 40, 40),
            [NK_COLOR_KNOB] = RGB(38, 38, 38),
            [NK_COLOR_KNOB_CURSOR] = RGB(100, 100, 100),
            [NK_COLOR_KNOB_CURSOR_HOVER] = RGB(120, 120, 120),
            [NK_COLOR_KNOB_CURSOR_ACTIVE] = RGB(150, 150, 150),
            [STYLE_LAMP_ON] = RGB(60, 200, 80),
            [STYLE_LAMP_OFF] = RGB(90, 90, 90),
            [STYLE_WARNING] = RGB(230, 150, 60),
            [STYLE_CHANGED] = RGB(240, 200, 80),
            [STYLE_TOGGLE_CURSOR_HOVER] = RGB(255, 255, 255),
            [STYLE_BUTTON_TEXT] = RGB(175, 175, 175),
            [STYLE_BUTTON_TEXT_HOVER] = RGB(235, 235, 235),
            [STYLE_BUTTON_TEXT_ACTIVE] = RGB(255, 255, 255),
            [STYLE_DISABLED_BUTTON] = RGB(25, 25, 25),
            [STYLE_DISABLED_TEXT] = RGB(87, 87, 87),
            [STYLE_DISABLED_BORDER] = RGB(32, 32, 32)
        }
    },
    {
        "light",
        {
            [NK_COLOR_TEXT] = RGB(28, 28, 28),
            [NK_COLOR_WINDOW] = RGB(236, 236, 232),
            [NK_COLOR_HEADER] = RGB(220, 220, 216),
            [NK_COLOR_BORDER] = RGB(150, 150, 146),
            [NK_COLOR_BUTTON] = RGB(214, 214, 210),
            [NK_COLOR_BUTTON_HOVER] = RGB(196, 196, 192),
            [NK_COLOR_BUTTON_ACTIVE] = RGB(176, 176, 172),
            [NK_COLOR_TOGGLE] = RGB(200, 200, 196),
            [NK_COLOR_TOGGLE_HOVER] = RGB(180, 180, 176),
            [NK_COLOR_TOGGLE_CURSOR] = RGB(28, 28, 28),
            [NK_COLOR_SELECT] = RGB(236, 236, 232),
            [NK_COLOR_SELECT_ACTIVE] = RGB(208, 208, 204),
            [NK_COLOR_SLIDER] = RGB(220, 220, 216),
            [NK_COLOR_SLIDER_CURSOR] = RGB(150, 150, 146),
            [NK_COLOR_SLIDER_CURSOR_HOVER] = RGB(130, 130, 126),
            [NK_COLOR_SLIDER_CURSOR_ACTIVE] = RGB(110, 110, 106),
            [NK_COLOR_PROPERTY] = RGB(250, 250, 248),
            [NK_COLOR_EDIT] = RGB(252, 252, 250),
            [NK_COLOR_EDIT_CURSOR] = RGB(28, 28, 28),
            [NK_COLOR_COMBO] = RGB(250, 250, 248),
            [NK_COLOR_CHART] = RGB(160, 160, 156),
            [NK_COLOR_CHART_COLOR] = RGB(236, 236, 232),
            [NK_COLOR_CHART_COLOR_HIGHLIGHT] = RGB(200, 0, 0),
            [NK_COLOR_SCROLLBAR] = RGB(225, 225, 221),
            [NK_COLOR_SCROLLBAR_CURSOR] = RGB(170, 170, 166),
            [NK_COLOR_SCROLLBAR_CURSOR_HOVER] = RGB(150, 150, 146),
            [NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = RGB(130, 130, 126),
            [NK_COLOR_TAB_HEADER] = RGB(220, 220, 216),
            [NK_COLOR_KNOB] = RGB(220, 220, 216),
            [NK_COLOR_KNOB_CURSOR] = RGB(150, 150, 146),
            [NK_COLOR_KNOB_CURSOR_HOVER] = RGB(130, 130, 126),
            [NK_COLOR_KNOB_CURSOR_ACTIVE] = RGB(110, 110, 106),
            [STYLE_LAMP_ON] = RGB(30, 190, 60),
            [STYLE_LAMP_OFF] = RGB(80, 80, 80),
            [STYLE_WARNING] = RGB(160, 70, 0),
            [STYLE_CHANGED] = RGB(20, 80, 180),
            [STYLE_TOGGLE_CURSOR_HOVER] = RGB(0, 0, 0),
            [STYLE_BUTTON_TEXT] = RGB(28, 28, 28),
            [STYLE_BUTTON_TEXT_HOVER] = RGB(0, 0, 0),
            [STYLE_BUTTON_TEXT_ACTIVE] = RGB(0, 0, 0),
            [STYLE_DISABLED_BUTTON] = RGB(226, 226, 222),
            [STYLE_DISABLED_TEXT] = RGB(150, 150, 146),
            [STYLE_DISABLED_BORDER] = RGB(200, 200, 196)
        }
    },
    {
        "gray",
        {
            [NK_COLOR_TEXT] = RGB(10, 10, 10),
            [NK_COLOR_WINDOW] = RGB(160, 160, 160),
            [NK_COLOR_HEADER] = RGB(150, 150, 150),
            [NK_COLOR_BORDER] = RGB(100, 100, 100),
            [NK_COLOR_BUTTON] = RGB(180, 180, 180),
            [NK_COLOR_BUTTON_HOVER] = RGB(200, 200, 200),
            [NK_COLOR_BUTTON_ACTIVE] = RGB(220, 220, 220),
            [NK_COLOR_TOGGLE] = RGB(205, 205, 205),
            [NK_COLOR_TOGGLE_HOVER] = RGB(225, 225, 225),
            [NK_COLOR_TOGGLE_CURSOR] = RGB(10, 10, 10),
            [NK_COLOR_SELECT] = RGB(160, 160, 160),
            [NK_COLOR_SELECT_ACTIVE] = RGB(135, 135, 135),
            [NK_COLOR_SLIDER] = RGB(150, 150, 150),
            [NK_COLOR_SLIDER_CURSOR] = RGB(110, 110, 110),
            [NK_COLOR_SLIDER_CURSOR_HOVER] = RGB(95, 95, 95),
            [NK_COLOR_SLIDER_CURSOR_ACTIVE] = RGB(80, 80, 80),
            [NK_COLOR_PROPERTY] = RGB(200, 200, 200),
            [NK_COLOR_EDIT] = RGB(215, 215, 215),
            [NK_COLOR_EDIT_CURSOR] = RGB(10, 10, 10),
            [NK_COLOR_COMBO] = RGB(185, 185, 185),
            [NK_COLOR_CHART] = RGB(110, 110, 110),
            [NK_COLOR_CHART_COLOR] = RGB(160, 160, 160),
            [NK_COLOR_CHART_COLOR_HIGHLIGHT] = RGB(150, 0, 0),
            [NK_COLOR_SCROLLBAR] = RGB(150, 150, 150),
            [NK_COLOR_SCROLLBAR_CURSOR] = RGB(110, 110, 110),
            [NK_COLOR_SCROLLBAR_CURSOR_HOVER] = RGB(95, 95, 95),
            [NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = RGB(80, 80, 80),
            [NK_COLOR_TAB_HEADER] = RGB(150, 150, 150),
            [NK_COLOR_KNOB] = RGB(150, 150, 150),
            [NK_COLOR_KNOB_CURSOR] = RGB(110, 110, 110),
            [NK_COLOR_KNOB_CURSOR_HOVER] = RGB(95, 95, 95),
            [NK_COLOR_KNOB_CURSOR_ACTIVE] = RGB(80, 80, 80),
            [STYLE_LAMP_ON] = RGB(40, 220, 60),
            [STYLE_LAMP_OFF] = RGB(90, 90, 90),
            [STYLE_WARNING] = RGB(110, 0, 0),
            [STYLE_CHANGED] = RGB(0, 0, 140),
            [STYLE_TOGGLE_CURSOR_HOVER] = RGB(0, 0, 0),
            [STYLE_BUTTON_TEXT] = RGB(10, 10, 10),
            [STYLE_BUTTON_TEXT_HOVER] = RGB(0, 0, 0),
            [STYLE_BUTTON_TEXT_ACTIVE] = RGB(0, 0, 0),
            [STYLE_DISABLED_BUTTON] = RGB(168, 168, 168),
            [STYLE_DISABLED_TEXT] = RGB(105, 105, 105),
            [STYLE_DISABLED_BORDER] = RGB(140, 140, 140)
        }
    },
    {
        "contrast",
        {
            [NK_COLOR_TEXT] = RGB(255, 255, 255),
            [NK_COLOR_WINDOW] = RGB(0, 0, 0),
            [NK_COLOR_HEADER] = RGB(0, 0, 0),
            [NK_COLOR_BORDER] = RGB(255, 255, 255),
            [NK_COLOR_BUTTON] = RGB(0, 0, 0),
            [NK_COLOR_BUTTON_HOVER] = RGB(70, 70, 70),
            [NK_COLOR_BUTTON_ACTIVE] = RGB(110, 110, 110),
            [NK_COLOR_TOGGLE] = RGB(90, 90, 90),
            [NK_COLOR_TOGGLE_HOVER] = RGB(140, 140, 140),
            [NK_COLOR_TOGGLE_CURSOR] = RGB(255, 255, 255),
            [NK_COLOR_SELECT] = RGB(0, 0, 0),
            [NK_COLOR_SELECT_ACTIVE] = RGB(80, 80, 80),
            [NK_COLOR_SLIDER] = RGB(30, 30, 30),
            [NK_COLOR_SLIDER_CURSOR] = RGB(160, 160, 160),
            [NK_COLOR_SLIDER_CURSOR_HOVER] = RGB(200, 200, 200),
            [NK_COLOR_SLIDER_CURSOR_ACTIVE] = RGB(255, 255, 255),
            [NK_COLOR_PROPERTY] = RGB(0, 0, 0),
            [NK_COLOR_EDIT] = RGB(0, 0, 0),
            [NK_COLOR_EDIT_CURSOR] = RGB(255, 255, 255),
            [NK_COLOR_COMBO] = RGB(0, 0, 0),
            [NK_COLOR_CHART] = RGB(160, 160, 160),
            [NK_COLOR_CHART_COLOR] = RGB(255, 255, 255),
            [NK_COLOR_CHART_COLOR_HIGHLIGHT] = RGB(255, 0, 0),
            [NK_COLOR_SCROLLBAR] = RGB(30, 30, 30),
            [NK_COLOR_SCROLLBAR_CURSOR] = RGB(160, 160, 160),
            [NK_COLOR_SCROLLBAR_CURSOR_HOVER] = RGB(200, 200, 200),
            [NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = RGB(255, 255, 255),
            [NK_COLOR_TAB_HEADER] = RGB(0, 0, 0),
            [NK_COLOR_KNOB] = RGB(30, 30, 30),
            [NK_COLOR_KNOB_CURSOR] = RGB(160, 160, 160),
            [NK_COLOR_KNOB_CURSOR_HOVER] = RGB(200, 200, 200),
            [NK_COLOR_KNOB_CURSOR_ACTIVE] = RGB(255, 255, 255),
            [STYLE_LAMP_ON] = RGB(0, 255, 0),
            [STYLE_LAMP_OFF] = RGB(70, 70, 70),
            [STYLE_WARNING] = RGB(255, 170, 0),
            [STYLE_CHANGED] = RGB(255, 255, 0),
            [STYLE_TOGGLE_CURSOR_HOVER] = RGB(255, 255, 0),
            [STYLE_BUTTON_TEXT] = RGB(255, 255, 255),
            [STYLE_BUTTON_TEXT_HOVER] = RGB(255, 255, 0),
            [STYLE_BUTTON_TEXT_ACTIVE] = RGB(255, 255, 0),
            [STYLE_DISABLED_BUTTON] = RGB(0, 0, 0),
            [STYLE_DISABLED_TEXT] = RGB(128, 128, 128),
            [STYLE_DISABLED_BORDER] = RGB(90, 90, 90)
        }
    },
    // The PDP-1 cabinet of the installed desktop picture (install/wallpaper.png): its light gray
    // panels, its blue console and trim, and unlit lamps near black. Colors sampled from the picture.
    {
        "pdp1",
        {
            [NK_COLOR_TEXT] = RGB(20, 20, 24),
            [NK_COLOR_WINDOW] = RGB(218, 219, 218),
            [NK_COLOR_HEADER] = RGB(199, 198, 194),
            [NK_COLOR_BORDER] = RGB(0, 85, 122),
            [NK_COLOR_BUTTON] = RGB(0, 103, 142),
            [NK_COLOR_BUTTON_HOVER] = RGB(30, 124, 165),
            [NK_COLOR_BUTTON_ACTIVE] = RGB(60, 145, 185),
            [NK_COLOR_TOGGLE] = RGB(250, 250, 250),
            [NK_COLOR_TOGGLE_HOVER] = RGB(236, 241, 244),
            [NK_COLOR_TOGGLE_CURSOR] = RGB(0, 103, 142),
            [NK_COLOR_SELECT] = RGB(218, 219, 218),
            [NK_COLOR_SELECT_ACTIVE] = RGB(190, 200, 206),
            [NK_COLOR_SLIDER] = RGB(199, 198, 194),
            [NK_COLOR_SLIDER_CURSOR] = RGB(0, 103, 142),
            [NK_COLOR_SLIDER_CURSOR_HOVER] = RGB(30, 124, 165),
            [NK_COLOR_SLIDER_CURSOR_ACTIVE] = RGB(0, 85, 122),
            [NK_COLOR_PROPERTY] = RGB(250, 250, 250),
            [NK_COLOR_EDIT] = RGB(250, 250, 250),
            [NK_COLOR_EDIT_CURSOR] = RGB(20, 20, 24),
            [NK_COLOR_COMBO] = RGB(250, 250, 250),
            [NK_COLOR_CHART] = RGB(199, 198, 194),
            [NK_COLOR_CHART_COLOR] = RGB(0, 103, 142),
            [NK_COLOR_CHART_COLOR_HIGHLIGHT] = RGB(200, 0, 0),
            [NK_COLOR_SCROLLBAR] = RGB(199, 198, 194),
            [NK_COLOR_SCROLLBAR_CURSOR] = RGB(0, 103, 142),
            [NK_COLOR_SCROLLBAR_CURSOR_HOVER] = RGB(30, 124, 165),
            [NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = RGB(0, 85, 122),
            [NK_COLOR_TAB_HEADER] = RGB(199, 198, 194),
            [NK_COLOR_KNOB] = RGB(199, 198, 194),
            [NK_COLOR_KNOB_CURSOR] = RGB(0, 103, 142),
            [NK_COLOR_KNOB_CURSOR_HOVER] = RGB(30, 124, 165),
            [NK_COLOR_KNOB_CURSOR_ACTIVE] = RGB(0, 85, 122),
            [STYLE_LAMP_ON] = RGB(30, 190, 60),
            [STYLE_LAMP_OFF] = RGB(40, 50, 58),
            [STYLE_WARNING] = RGB(150, 60, 0),
            [STYLE_CHANGED] = RGB(0, 85, 122),
            [STYLE_TOGGLE_CURSOR_HOVER] = RGB(30, 124, 165),
            [STYLE_BUTTON_TEXT] = RGB(255, 255, 255),
            [STYLE_BUTTON_TEXT_HOVER] = RGB(255, 255, 255),
            [STYLE_BUTTON_TEXT_ACTIVE] = RGB(255, 255, 255),
            [STYLE_DISABLED_BUTTON] = RGB(190, 196, 200),
            [STYLE_DISABLED_TEXT] = RGB(120, 130, 138),
            [STYLE_DISABLED_BORDER] = RGB(160, 170, 176)
        }
    }
};
#define SCHEME_COUNT ((int)(sizeof(schemes) / sizeof(schemes[0])))

struct nk_color styleColors[STYLE_COUNT];

static struct nk_style_button savedButton;
static struct nk_style_toggle savedCheckbox;
static int currentScheme;       // the built-in scheme styleColors started from

// The name of built-in scheme index, counting from 0.
// Returns NULL past the last one.
const char *
styleSchemeName(int index)
{
    return( ((index >= 0) && (index < SCHEME_COUNT)) ? schemes[index].nameP : NULL );
}

// The index of the built-in scheme in use, for styleSchemeName.
int
styleCurrent(void)
{
    return(currentScheme);
}

// The override name of a scheme entry: Nuklear's NK_COLOR_BUTTON_HOVER or our
// STYLE_BUTTON_TEXT_HOVER becomes colorButtonHover or colorButtonTextHover. Settings names are
// letters and digits only, as in pidp1.config, so the file's own parser reads them.
// Returns bufP, or "" for an entry out of range.
const char *
styleEntryName(int entry, char *bufP, size_t bufLen)
{
static const char *ownNames[] = { "LAMP_ON", "LAMP_OFF", "WARNING", "CHANGED", "TOGGLE_CURSOR_HOVER",
    "BUTTON_TEXT", "BUTTON_TEXT_HOVER", "BUTTON_TEXT_ACTIVE", "DISABLED_BUTTON", "DISABLED_TEXT", "DISABLED_BORDER" };
const char *upperP;
size_t len;
bool capital;

    if( (entry < 0) || (entry >= STYLE_COUNT) || (bufLen < 6) )
    {
        return("");
    }

    upperP = ((entry < NK_COLOR_COUNT) ? (nk_style_get_color_by_name(entry) + strlen("NK_COLOR_")) :
        ownNames[entry - NK_COLOR_COUNT]);
    strcpy(bufP, "color");
    len = strlen(bufP);
    for( capital = true; *upperP && (len < (bufLen - 1)); upperP++ )
    {
        if( *upperP == '_' )
        {
            capital = true;
            continue;
        }

        bufP[len++] = (capital ? *upperP : (char)(*upperP - 'A' + 'a'));
        capital = false;
    }

    bufP[len] = '\0';
    return(bufP);
}

// The index of a built-in scheme. Returns -1 if none has that name.
static int
findScheme(const char *nameP)
{
int i;

    for( i = 0; i < SCHEME_COUNT; i++ )
    {
        if( !strcmp(schemes[i].nameP, nameP) )
        {
            return(i);
        }
    }

    return(-1);
}

// Rebuild Nuklear's style from styleColors.
static void
applyColors(struct nk_context *ctxP)
{
struct nk_style *styleP;

    nk_style_from_table(ctxP, styleColors);

    // What the table has no entry for.
    styleP = &ctxP->style;
    styleP->button.text_normal = styleColors[STYLE_BUTTON_TEXT];
    styleP->button.text_hover = styleColors[STYLE_BUTTON_TEXT_HOVER];
    styleP->button.text_active = styleColors[STYLE_BUTTON_TEXT_ACTIVE];
    styleP->checkbox.cursor_hover = nk_style_item_color(styleColors[STYLE_TOGGLE_CURSOR_HOVER]);
    styleP->option.cursor_hover = nk_style_item_color(styleColors[STYLE_TOGGLE_CURSOR_HOVER]);
    styleP->button.disabled_factor = 1.0f;
    styleP->checkbox.disabled_factor = 1.0f;
}

// Make a built-in scheme the one in use, with no overrides, and rebuild Nuklear's style from it.
// pdp1central uses it before the file is read, so a file that cannot be read leaves the default.
// Returns false, with the default scheme used instead, if no scheme has that name.
bool
styleUse(struct nk_context *ctxP, const char *nameP)
{
int i;

    i = findScheme(nameP);
    currentScheme = ((i >= 0) ? i : findScheme(STYLE_DEFAULT));
    memcpy(styleColors, schemes[currentScheme].colors, sizeof(styleColors));
    applyColors(ctxP);
    return(i >= 0);
}

// Parse "R,G,B", each 0 to 255 in decimal with no spaces.
// Returns false, with colorP unchanged, for anything else.
static bool
parseColor(const char *textP, struct nk_color *colorP)
{
int part[3];
int i, value;

    for( i = 0; i < 3; i++ )
    {
        if( (*textP < '0') || (*textP > '9') )
        {
            return(false);
        }

        for( value = 0; (*textP >= '0') && (*textP <= '9'); textP++ )
        {
            value = ((value * 10) + (*textP - '0'));
            if( value > 255 )
            {
                return(false);
            }
        }

        part[i] = value;
        if( (i < 2) && (*textP++ != ',') )
        {
            return(false);
        }
    }

    if( *textP )
    {
        return(false);
    }

    *colorP = nk_rgb(part[0], part[1], part[2]);
    return(true);
}

// Whether a line is blank, a comment, or what the settings parser takes as name=value: a name of
// letters and digits from the first column, then '=' and a value, with spaces allowed around '='.
// The parser passes over any other line without a word, so it is reported here instead.
static bool
lineUnderstood(const char *lineP)
{
const char *p;

    for( p = lineP; (*p == ' ') || (*p == '\t') || (*p == '\r'); p++ )
    {
    }

    if( !*p || (lineP[0] == '#') )
    {
        return(true);
    }

    for( p = lineP; ((*p >= 'a') && (*p <= 'z')) || ((*p >= 'A') && (*p <= 'Z')) || ((*p >= '0') && (*p <= '9')); p++ )
    {
    }

    if( p == lineP )
    {
        return(false);
    }

    for( ; (*p == ' ') || (*p == '\t'); p++ )
    {
    }

    if( *p++ != '=' )
    {
        return(false);
    }

    for( ; (*p == ' ') || (*p == '\t'); p++ )
    {
    }

    return( (*p != '\0') && (*p != '\r') );
}

// Use the scheme pdp1central.config names (the default when it names none), with the file's
// color overrides applied on top, and rebuild Nuklear's style. The last line for a name wins.
// Every problem is passed to logP, and the rest of the file still applies.
// Returns the number of override names in effect.
int
styleLoad(struct nk_context *ctxP, ConfFile *cfP, void (*logP)(const char *fmtP, ...))
{
char names[(STYLE_COUNT * 2)][CONF_MAX_VALUE + 1];
char schemeName[CONF_MAX_VALUE + 1];
char entryName[CONF_MAX_VALUE + 1];
const char *valueP;
struct nk_color color;
int count, applied, scheme, i, entry;

    for( i = 0; i < cfP->lineCount; i++ )
    {
        if( !lineUnderstood(cfP->linesP[i]) )
        {
            logP("%s line %d is not name=value; ignored", cfP->pathP, (i + 1));
        }
    }

    snprintf(schemeName, sizeof(schemeName), "%s", ((valueP = confGet(cfP, "scheme")) ? valueP : STYLE_DEFAULT));
    if( (scheme = findScheme(schemeName)) < 0 )
    {
        logP("%s: there is no scheme named %s; using %s", cfP->pathP, schemeName, STYLE_DEFAULT);
        scheme = findScheme(STYLE_DEFAULT);
    }

    currentScheme = scheme;
    memcpy(styleColors, schemes[scheme].colors, sizeof(styleColors));

    applied = 0;
    count = confNames(cfP, names, (STYLE_COUNT * 2));
    for( i = 0; i < count; i++ )
    {
        if( !strcmp(names[i], "scheme") )
        {
            continue;
        }

        for( entry = 0; (entry < STYLE_COUNT) && strcmp(names[i], styleEntryName(entry, entryName, sizeof(entryName)));
            entry++ )
        {
        }

        if( entry == STYLE_COUNT )
        {
            logP("%s: %s is not a color name; ignored", cfP->pathP, names[i]);
        }
        else if( !parseColor((valueP = confGet(cfP, names[i])), &color) )
        {
            logP("%s: %s=%s is not R,G,B with each 0 to 255; ignored", cfP->pathP, names[i], valueP);
        }
        else
        {
            styleColors[entry] = color;
            applied++;
        }
    }

    applyColors(ctxP);
    return(applied);
}

// Start drawing buttons and check boxes that cannot be used, in the scheme's disabled colors.
// Every call is paired with styleDisableEnd before the next one.
void
styleDisableBegin(struct nk_context *ctxP)
{
struct nk_style_button *buttonP;
struct nk_style_toggle *toggleP;

    savedButton = ctxP->style.button;
    savedCheckbox = ctxP->style.checkbox;

    buttonP = &ctxP->style.button;
    buttonP->normal = nk_style_item_color(styleColors[STYLE_DISABLED_BUTTON]);
    buttonP->hover = buttonP->normal;
    buttonP->active = buttonP->normal;
    buttonP->border_color = styleColors[STYLE_DISABLED_BORDER];
    buttonP->text_normal = styleColors[STYLE_DISABLED_TEXT];
    buttonP->text_hover = buttonP->text_normal;
    buttonP->text_active = buttonP->text_normal;

    toggleP = &ctxP->style.checkbox;
    toggleP->normal = nk_style_item_color(styleColors[STYLE_DISABLED_BUTTON]);
    toggleP->hover = toggleP->normal;
    toggleP->active = toggleP->normal;
    toggleP->cursor_normal = nk_style_item_color(styleColors[STYLE_DISABLED_TEXT]);
    toggleP->cursor_hover = toggleP->cursor_normal;
    toggleP->text_normal = styleColors[STYLE_DISABLED_TEXT];
    toggleP->text_hover = toggleP->text_normal;
    toggleP->text_active = toggleP->text_normal;

    nk_widget_disable_begin(ctxP);
}

// Go back to drawing live widgets.
void
styleDisableEnd(struct nk_context *ctxP)
{
    nk_widget_disable_end(ctxP);
    ctxP->style.button = savedButton;
    ctxP->style.checkbox = savedCheckbox;
}
