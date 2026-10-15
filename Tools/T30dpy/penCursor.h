/*
 * penCursor.h - the light-pen cursor art shared by t30dpy (SDL2) and t30dpy3 (SDL3).
 *
 * Inside the display window the mouse is the light pen, on the screen while the left button is
 * held. Each client makes two color cursors from this art at startup, one for pen up and one for
 * pen down, and shows the one matching the pen state it last sent to pdp1. Data only: each client
 * does its own SDL calls.
 *
 * The art is 32 by 32, one character per pixel, and is edited in place. penCursorColors gives each
 * character's color in the pen-up and pen-down cursors as ARGB, alpha in the top byte, 0 being
 * transparent. A character not in the table is transparent. '@' is the hot spot, the pixel the pen
 * aims at: the position SDL reports, and so the one sent to pdp1. Each cursor needs exactly one,
 * and both must have it in the same place, or the aim moves when the button is pressed. The hot
 * spot and its neighbors are left clear so the point aimed at stays visible; nothing marks it,
 * since programs do their own highlighting of what the pen sees.
 *
 * Pen up is a dim outline with a dark tip, pen down a bright filled pen with a lit tip: brightness
 * tells them apart even without color.
 */

#ifndef PENCURSOR_H
#define PENCURSOR_H

#include <stdint.h>

#define PENCURSORSIZE 32
#define PENCURSORHOT '@'

typedef struct PenCursorColor {
    char ch;
    uint32_t argb[2];           // [0] pen up, [1] pen down
} PenCursorColor;

static const PenCursorColor penCursorColors[] = {
    //  char    pen up        pen down
    { '.', { 0x00000000, 0x00000000 } },    // transparent
    { '@', { 0x00000000, 0x00000000 } },    // the hot spot, transparent
    { '#', { 0xFF8C8C8C, 0xFFFFFFFF } },    // outline: mid gray, white
    { '=', { 0x00000000, 0xFFC0C0C0 } },    // barrel fill: none, light gray
    { '^', { 0xFF303030, 0xFFDCE1FF } },    // tip: dark, blue-white
    { '~', { 0xFF6E6E6E, 0xFFB4B4B4 } }     // cable
};

static const char *const penCursorArt[2][PENCURSORSIZE] = {
    {   // pen up
        "...............................~",
        "..............................~.",
        ".............................~..",
        "...........................~~...",
        ".......................###~.....",
        "......................##.##.....",
        ".....................##...##....",
        "....................##.....##...",
        "...................##.....##....",
        "..................##.....##.....",
        ".................##.....##......",
        "................##.....##.......",
        "...............##.....##........",
        "..............##.....##.........",
        ".............##.....##..........",
        "............##.....##...........",
        "...........##.....##............",
        "..........##.....##.............",
        "........###.....##..............",
        "........##.....##...............",
        ".......#^##...##................",
        ".......#^^##.##.................",
        ".......#^^^###..................",
        ".......#^^^^##..................",
        "......#^####....................",
        "......##........................",
        "................................",
        "....@...........................",
        "................................",
        "................................",
        "................................",
        "................................"
    },
    {   // pen down
        "...............................~",
        "..............................~.",
        ".............................~..",
        "...........................~~...",
        ".......................###~.....",
        "......................##=##.....",
        ".....................##===##....",
        "....................##=====##...",
        "...................##=====##....",
        "..................##=====##.....",
        ".................##=====##......",
        "................##=====##.......",
        "...............##=====##........",
        "..............##=====##.........",
        ".............##=====##..........",
        "............##=====##...........",
        "...........##=====##............",
        "..........##=====##.............",
        "........###=====##..............",
        "........##=====##...............",
        ".......#^##===##................",
        ".......#^^##=##.................",
        ".......#^^^###..................",
        ".......#^^^^##..................",
        "......#^####....................",
        "......##........................",
        "................................",
        "....@...........................",
        "................................",
        "................................",
        "................................",
        "................................"
    }
};

#endif
