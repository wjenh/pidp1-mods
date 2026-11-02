// The scene: the world's objects, the hand, the hidden-line pass and the display.
//
// The picture is six groups of points. They are the red, green, blue and white
// objects (the box and the table), the held object, and the hand. Each edge of an
// object is clipped against the screen outlines of the opaque objects in front of
// it, Cyrus-Beck in the edge's steps; what is left is expanded to points as the
// 340 draws a vector. Where two objects share a line it is drawn once, by the
// brighter, so that the two do not fight on the same pixels.
//
// A thread of its own sends the points to the display client at the 340's pace,
// frame after frame. The program changes what it sends between frames, under one
// lock, and never waits for it, except that the hand's steps keep their period.

#include <arpa/inet.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "scene.h"

#define NOBJ 13
#define BOXOBJ 12
#define MAXOCC 12

#define KWALL 0
#define KBLOCK 1
#define KPYRAMID 2
#define KBOX 3

#define CRED 0
#define CGREEN 1
#define CBLUE 2
#define CWHITE 3

#define FTRANS 1
#define WALL 010

#define HANDX 040
#define HANDY 0
#define HANDZ 0
#define LIFTZ 01300
#define ARMTOP 01730
#define STEPX 020
#define STEPY 014
#define HBARTOP 24
#define HBARBOT 40
// Shorter than any frame, so the hand takes one step on every frame (frameWait);
// a period above a frame makes the steps fall some frames apart.
#define PERIOD_MICROS 1L

#define RASTERMAX 1023
#define INTENSITY_WHITE 7
#define VECTOR_MAX 127

// The 340's rate on the emulator, points a second, and how many go at a time.
#define POINT_RATE 473000L
#define CHUNK_POINTS 512
// A frame shorter than this waits out the rest.
#define MIN_FRAME_MICROS 20000L
// The longest a hand step waits for a frame to end, so a stopped refresh cannot hold a move.
#define FRAME_WAIT_MAX_MICROS 200000L

// The idle color labels (sceneLabelsOn): two letters of the 340's character set 1 at
// scale 1, so the dots of a letter's 5 by 7 matrix are LABEL_STEP raster units apart
// and the next letter starts LABEL_ADVANCE dots on. A label stands LABEL_DX and
// LABEL_DY right of and above the middle of its object's longest visible piece, and
// is kept where both letters fit on the raster.
#define LABEL_INTENSITY 7
#define LABEL_STEP 2
#define LABEL_ADVANCE 7
#define LABEL_DX 4
#define LABEL_DY 4
#define LABEL_MAXX (RASTERMAX - (15 * LABEL_STEP))
#define LABEL_MAXY (RASTERMAX - (8 * LABEL_STEP))
#define GLYPH_ROWS 7
#define GLYPH_COLS 5

#define MAX_INTERVALS 64
#define MAX_VERTS 14

#define MILLION 1000000L

typedef struct Object
{
    int x;
    int y;
    int z;
    int sx;
    int sy;
    int sz;
    int dsx;
    int dsy;
    int dsz;
    int kind;
    int color;
    int flags;
} Object;

// An occluder, with its screen outline, counterclockwise, and its box in the world.
typedef struct Occluder
{
    int sminx;
    int smaxx;
    int sminy;
    int smaxy;
    int owner;
    int wall;
    int lo[3];
    int hi[3];
    int n;
    int px[6];
    int py[6];
} Occluder;

// A kind's shape. Each vertex is three octal digits, the x, y and z choices of
// coordOf, and each edge is two vertex numbers as octal pairs. An edge starts where the
// last ended wherever the shape allows, which a vector list needs and a point
// list does not, but the choice of each edge's first end decides its points.
typedef struct Shape
{
    int nVert;
    const int *vertP;
    int nVisible;
    int nHidden;
    const int *edgeP;
} Shape;

// A growing list of point words.
typedef struct Points
{
    uint32_t *wordP;
    size_t n;
    size_t cap;
} Points;

static const Object objStart[NOBJ] =
{
    {0110, 0100, 0, 0100, 0100, 0100, 0100, 0100, 0100, KBLOCK, CRED, 0},
    {0110, 0100, 0100, 0100, 0100, 0100, 0100, 0100, 0100, KPYRAMID, CGREEN, 0},
    {0400, 0, 0, 0200, 0200, 0200, 0200, 0200, 0200, KBLOCK, CGREEN, 0},
    {0640, 0640, 1, 0200, 0200, 0200, 0200, 0200, 0200, KPYRAMID, CBLUE, 0},
    {0500, 0100, 0200, 0100, 0100, 0300, 0100, 0100, 0300, KPYRAMID, CRED, 0},
    {0, 0300, 0, 0200, 0300, 0300, 0200, 0300, 0300, KBLOCK, CRED, 0},
    {0, 0240, 0300, 0200, 0200, 0200, 0200, 0200, 0200, KBLOCK, CGREEN, 0},
    {0300, 0640, 0, 0200, 0100, 0400, 0200, 0100, 0400, KBLOCK, CBLUE, 0},
    {0570, 0570, 0, 010, 0400, 0300, 0, 0, 0, KWALL, CWHITE, 0},
    {0570, 0570, 0, 0400, 010, 0300, 0, 0, 0, KWALL, CWHITE, 0},
    {0570, 01200, 0, 0400, 010, 0300, 0, 0, 0, KWALL, CWHITE, 0},
    {01200, 0570, 0, 010, 0400, 0300, 0, 0, 0, KWALL, CWHITE, 0},
    {0600, 0600, 0, 0400, 0400, 1, 0376, 0376, 0300, KBOX, CWHITE, 0}
};

static const char *const objNames[NOBJ] =
{
    "B1", "B2", "B3", "B4", "B5", "B6", "B7", "B10", "BW1", "BW2", "BW3", "BW4", "BOX"
};
static const char *const colorWords[] = {"RED", "GREEN", "BLUE", "WHITE"};
static const char *const kindWords[] = {"WALL", "BLOCK", "PYRAMID", "BOX"};
static const int intensityOf[] = {5, 3, 1, 7};

// A letter of the 342 Character Generator's table (the Type 340 Programming Manual,
// Table 2). Its seven rows from the top have five dots each, the leftmost the high bit.
// The letter begins at its lower left dot.
typedef struct Glyph
{
    char letter;
    unsigned char row[GLYPH_ROWS];
} Glyph;

static const Glyph glyphs[] =
{
    {'B', {036, 021, 021, 036, 021, 021, 036}},
    {'D', {036, 021, 021, 021, 021, 021, 036}},
    {'G', {016, 021, 020, 027, 021, 021, 016}},
    {'L', {020, 020, 020, 020, 020, 020, 037}},
    {'R', {036, 021, 021, 036, 024, 022, 021}}
};

// The two letters of each color's label.
static const char *const labelWords[] = {"RD", "GR", "BL"};

static const int blockVerts[] = {0000, 0100, 0110, 0111, 0011, 0001, 0101, 0010};
static const int blockEdges[] =
{
    0201, 0106, 0605, 0504, 0403, 0302, 0207, 0407, 0706,
    0110, 01003, 01005
};
static const int pyramidVerts[] = {0000, 0100, 0110, 0221, 0010};
static const int pyramidEdges[] = {0201, 0104, 0403, 0302, 0204, 0405, 0501, 0503};
static const int boxVerts[] =
{
    0000, 0100, 0110, 0111, 0011, 0001, 0101, 0010, 0331, 0431, 0441, 0341, 0340
};
static const int boxEdges[] =
{
    0201, 0106, 0605, 0504, 0403, 0302, 0207,
    0407, 0706, 0611, 01112, 01213, 01314, 01415,
    01114,
    0110, 01003, 01005
};
static const Shape shapes[] =
{
    {8, blockVerts, 9, 3, blockEdges},
    {8, blockVerts, 9, 3, blockEdges},
    {5, pyramidVerts, 5, 3, pyramidEdges},
    {13, boxVerts, 15, 3, boxEdges}
};

// The table's edges on the screen: x0 y0 x1 y1 each.
static const int tableEdges[4][4] =
{
    {0, 0, 582, 1},
    {582, 1, 1022, 254},
    {1022, 254, 437, 254},
    {437, 254, 0, 0}
};

// The world.
static Object objs[NOBJ];
static int handX;
static int handY;
static int handZ;
static int held;
static int fastHand;

// The vertices of the object being worked on, from index 1.
static int gvx[MAX_VERTS];
static int gvy[MAX_VERTS];
static int gvz[MAX_VERTS];
static int gsx[MAX_VERTS];
static int gsy[MAX_VERTS];
static const Shape *shapeP;
static int gClamp;
static int gLoc[3];
static int gDraw[3];

// The edge being clipped.
static int cOwner;
static int cEmit;
static int cWhole;
static unsigned cMask;
static int cInBox;
static int cDx;
static int cDy;
static int cAdx;
static int cAdy;
static int cL;
static int cKlo;
static int cKhi;
static int e0x;
static int e0y;
static int e1x;
static int e1y;
static int eLo[3];
static int eHi[3];
static int intLo[MAX_INTERVALS];
static int intHi[MAX_INTERVALS];
static int nInt;

static Occluder occTab[MAXOCC];
static int occN;

// The objects that can draw part of the working object's edges.
static const Object *shareCand[NOBJ];
static int shareN;
static int shareBlo[3];
static int shareBhi[3];

// The group being built.
static Points *groupP;
static int dlIntensity;
static int dlRun;
static int dlRel;
static int dlCx;
static int dlCy;

// What the display shows, under showLock.
static pthread_mutex_t showLock = PTHREAD_MUTEX_INITIALIZER;
static Points shownScene;
static Points shownHeld;
static int heldOffX;
static int heldOffY;
static int heldFirst;
static int shownX;
static int shownY;
// The labels' dots while they are on, none while they are off.
static Points shownLabels;

// Each object's longest visible piece in the last rebuild, as its length (the sum of its
// two extents) and its middle. The first of equals wins.
static int bestLen[NOBJ];
static int bestX[NOBJ];
static int bestY[NOBJ];
static int labelsOn;
static int noLabels;

// What STATUS reports.
static volatile long frames;
static long clamps;
static long rebuildMicros;

static long handWaitMicros;
static long handDue;
static int detached;

static void rebuild(void);

// Microseconds on a clock that does not stop.
static long
nowMicros(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((long)ts.tv_sec * MILLION) + (ts.tv_nsec / 1000L);
}

static void
sleepUntil(long micros)
{
    struct timespec ts;

    ts.tv_sec = micros / MILLION;
    ts.tv_nsec = (micros % MILLION) * 1000L;
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}

// One coordinate of the projection, in the machine's integers. The three
// products are each divided by 1000 and their remainders summed, which equals
// the original's floating formula for every coordinate that occurs.
static int
projOne(int a, int y, int p, int q)
{
    int w;

    w = (y * q) / 1000;
    return ((a * 900) / 1000) + ((y * p) / 1000)
           + ((((a * 900) % 1000) + ((y * p) % 1000) + 2700 + w) / 1000);
}

void
sceneProject(int x, int y, int z, int *sxP, int *syP)
{
    *sxP = projOne(x, y, 675, 999);
    *syP = projOne(z, y, 389, 718);
}

// A screen coordinate clamped to the raster; a clamped one is counted if asked.
static int
clampTo(int v, int count)
{
    if(v < 0)
    {
        clamps += count;
        return 0;
    }
    if(v > RASTERMAX)
    {
        clamps += count;
        return RASTERMAX;
    }
    return v;
}

// One coordinate of a vertex. 0 is the low side, 1 the high side, 2 the middle, 3
// one wall in from the low side, 4 one wall in from the high side.
static int
coordOf(int choice, int lo, int size)
{
    switch(choice)
    {
    case 0:
        return lo;
    case 1:
        return lo + size;
    case 2:
        return (size >> 1) + lo;
    case 3:
        return lo + WALL;
    default:
        return (lo + size) - WALL;
    }
}

// Makes the vertices of a shape at (lx, ly, lz), in the world and on the screen.
static void
geomBuild(int kind, const int *sizeP, int lx, int ly, int lz)
{
    int k;
    int spec;

    shapeP = &shapes[kind];
    gLoc[0] = lx;
    gLoc[1] = ly;
    gLoc[2] = lz;
    gDraw[0] = sizeP[0];
    gDraw[1] = sizeP[1];
    gDraw[2] = sizeP[2];
    for(k = 1; k <= shapeP->nVert; k++)
    {
        spec = shapeP->vertP[k - 1];
        gvx[k] = coordOf(spec >> 6, lx, sizeP[0]);
        gvy[k] = coordOf((spec >> 3) & 7, ly, sizeP[1]);
        gvz[k] = coordOf(spec & 7, lz, sizeP[2]);
        sceneProject(gvx[k], gvy[k], gvz[k], &gsx[k], &gsy[k]);
        if(gClamp)
        {
            gsx[k] = clampTo(gsx[k], 1);
            gsy[k] = clampTo(gsy[k], 1);
        }
    }
}

// Adds the outline geomBuild made as an occluder. The outline is a block's first
// six vertices, or a pyramid's base corners, apex and, when it shows, the back left
// corner.
static void
occAdd(int owner, int wall)
{
    Occluder *qP;
    int i;
    long cross;

    qP = &occTab[occN];
    occN++;
    qP->owner = owner;
    qP->wall = wall;
    for(i = 0; i < 3; i++)
    {
        qP->lo[i] = gLoc[i];
        qP->hi[i] = gLoc[i] + gDraw[i];
    }
    qP->n = 6;
    if(shapeP == &shapes[KPYRAMID])
    {
        cross = ((long)(gsx[1] - gsx[4]) * (gsy[5] - gsy[4]))
                - ((long)(gsy[1] - gsy[4]) * (gsx[5] - gsx[4]));
        qP->n = (cross < 0) ? 5 : 4;
    }
    qP->sminx = gsx[1];
    qP->smaxx = gsx[1];
    qP->sminy = gsy[1];
    qP->smaxy = gsy[1];
    for(i = 0; i < qP->n; i++)
    {
        qP->px[i] = gsx[i + 1];
        qP->py[i] = gsy[i + 1];
        qP->sminx = (qP->px[i] < qP->sminx) ? qP->px[i] : qP->sminx;
        qP->smaxx = (qP->px[i] > qP->smaxx) ? qP->px[i] : qP->smaxx;
        qP->sminy = (qP->py[i] < qP->sminy) ? qP->py[i] : qP->sminy;
        qP->smaxy = (qP->py[i] > qP->smaxy) ? qP->py[i] : qP->smaxy;
    }
}

// Lists the occluders. They are every object drawn, opaque and not held, and the
// box as its four walls, each a block.
static void
occBuild(void)
{
    int o;
    int size[3];
    const Object *oP;

    occN = 0;
    gClamp = 0;
    for(o = 0; o < NOBJ; o++)
    {
        oP = &objs[o];
        if((oP->kind == KWALL) || (o == held) || (oP->flags & FTRANS))
        {
            continue;
        }
        size[0] = oP->dsx;
        size[1] = oP->dsy;
        size[2] = oP->dsz;
        if(oP->kind != KBOX)
        {
            geomBuild(oP->kind, size, oP->x, oP->y, oP->z);
            occAdd(o, 0);
            continue;
        }
        size[1] = WALL;
        geomBuild(KBLOCK, size, oP->x, oP->y, oP->z);
        occAdd(o, 1);
        geomBuild(KBLOCK, size, oP->x, (oP->y + oP->dsy) - WALL, oP->z);
        occAdd(o, 1);
        size[0] = WALL;
        size[1] = oP->dsy;
        geomBuild(KBLOCK, size, oP->x, oP->y, oP->z);
        occAdd(o, 1);
        geomBuild(KBLOCK, size, (oP->x + oP->dsx) - WALL, oP->y, oP->z);
        occAdd(o, 1);
    }
}

// True if the working object is not the box and its center is strictly inside
// the box's outline. The box hides nothing that is in it.
static int
inBox(void)
{
    const Object *boxP;
    int c;

    if((cOwner < 0) || (cOwner == BOXOBJ))
    {
        return 0;
    }
    boxP = &objs[BOXOBJ];
    c = (gLoc[0] * 2) + gDraw[0];
    if(((boxP->x * 2) >= c) || (c >= ((boxP->x * 2) + (boxP->dsx * 2))))
    {
        return 0;
    }
    c = (gLoc[1] * 2) + gDraw[1];
    if(((boxP->y * 2) >= c) || (c >= ((boxP->y * 2) + (boxP->dsy * 2))))
    {
        return 0;
    }
    return 1;
}

// True if the occluder is on the viewer's side (+x, -y or +z) of the edge. Boxes
// that overlap go by their centers' depth. Against the box's edges an object
// in it is taken WALL in from each side, since what is in the box can overlap a
// wall by the wall's thickness; behind then finds what lies in front of it.
static int
inFront(Occluder *qP)
{
    long depth;

    if((cOwner == BOXOBJ) && !qP->wall)
    {
        qP->lo[0] += WALL;
        qP->lo[1] += WALL;
        qP->hi[0] -= WALL;
        qP->hi[1] -= WALL;
    }
    if((qP->lo[0] >= eHi[0]) || (eLo[1] >= qP->hi[1]) || (qP->lo[2] >= eHi[2]))
    {
        return 1;
    }
    if((eLo[0] >= qP->hi[0]) || (qP->lo[1] >= eHi[1]) || (eLo[2] >= qP->hi[2]))
    {
        return 0;
    }
    depth = (3L * ((qP->lo[0] + qP->hi[0]) - eLo[0] - eHi[0]))
            - (4L * ((qP->lo[1] + qP->hi[1]) - eLo[1] - eHi[1]))
            + (2L * ((qP->lo[2] + qP->hi[2]) - eLo[2] - eHi[2]));
    return depth > 0;
}

// The step of the edge at distance d of n along it, rounded.
static int
stepRound(int d, int n)
{
    int q;
    int r;

    q = (cL * d) / n;
    r = (cL * d) % n;
    return ((r * 2) >= n) ? (q + 1) : q;
}

// Narrows the hidden interval of one of the box's edges to the part behind an
// object in the box, after inFront took the object WALL in from each side. Where
// the object stands in a wall, part of the edge can lie nearer the viewer than
// the object's side. The box's edges each lie along one axis; on the screen sx
// grows with x and y, sy with z, which tells which end the hidden part starts
// from. True if some of the interval is left.
static int
behind(const Occluder *qP)
{
    int n;
    int d;
    int s;

    if((cOwner != BOXOBJ) || qP->wall)
    {
        return 1;
    }
    if(eHi[0] != eLo[0])
    {
        n = eHi[0] - eLo[0];
        d = (qP->hi[0] + WALL) - eLo[0];
        s = cDx;
    }
    else if(eHi[1] != eLo[1])
    {
        n = eHi[1] - eLo[1];
        d = (eHi[1] - qP->lo[1]) + WALL;
        s = -cDx;
    }
    else if(eHi[2] != eLo[2])
    {
        n = eHi[2] - eLo[2];
        d = qP->hi[2] - eLo[2];
        s = cDy;
    }
    else
    {
        return 1;
    }
    if(d < 0)
    {
        return 0;
    }
    if(d >= n)
    {
        return 1;
    }
    d = stepRound(d, n);
    if(s >= 0)
    {
        cKhi = (d < cKhi) ? d : cKhi;
    }
    else
    {
        d = cL - d;
        cKlo = (d >= cKlo) ? d : cKlo;
    }
    return cKlo < cKhi;
}

static long
absLong(long v)
{
    return (v < 0) ? -v : v;
}

// The crossing of one side, the step L (-F) / G, rounded. A crossing before the
// edge is -1 and one at or past its end is L. Both magnitudes are halved until G
// is under 2^17 first, as the machine must, which decides some roundings.
static int
ratio(long f, long g)
{
    long nw;
    long dw;
    long q;
    long r;

    if(f == 0)
    {
        return 0;
    }
    if((f < 0) == (g < 0))
    {
        return -1;
    }
    nw = absLong(f);
    dw = absLong(g);
    if(nw >= dw)
    {
        return cL;
    }
    while(dw >= (1L << 17))
    {
        nw >>= 1;
        dw >>= 1;
    }
    q = ((long)cL * nw) / dw;
    r = ((long)cL * nw) % dw;
    if(q >= (1L << 17))
    {
        return cL;
    }
    return (int)((r >= (dw - r)) ? (q + 1) : q);
}

static int
sign(int v)
{
    return (v > 0) - (v < 0);
}

// Finds, by Cyrus-Beck, where the edge is strictly inside the occluder's outline.
// An edge lying along a side, both its ends within about two raster units of the
// side's line, is on the outline, not inside it. Their ends were rounded separately.
// True with the interval in cKlo..cKhi.
static int
cyrus(const Occluder *qP)
{
    int j;
    int jn;
    int ex;
    int ey;
    int m;
    int kk;
    long f;
    long g;

    cKlo = 0;
    cKhi = cL;
    for(j = 0; j < qP->n; j++)
    {
        jn = ((j + 1) == qP->n) ? 0 : (j + 1);
        ex = qP->px[jn] - qP->px[j];
        ey = qP->py[jn] - qP->py[j];
        if((ex == 0) && (ey == 0))
        {
            continue;
        }
        if(ey == 0)
        {
            ex = sign(ex);
        }
        if(ex == 0)
        {
            ey = sign(ey);
        }
        m = abs(ex);
        m = (abs(ey) >= m) ? abs(ey) : m;
        m *= 2;
        f = ((long)ex * (e0y - qP->py[j])) - ((long)ey * (e0x - qP->px[j]));
        g = ((long)ex * cDy) - ((long)ey * cDx);
        if((absLong(f) <= m) && (absLong(f + g) <= m))
        {
            return 0;
        }
        if(g == 0)
        {
            if(f <= 0)
            {
                return 0;
            }
            continue;
        }
        kk = ratio(f, g);
        if(g > 0)
        {
            cKlo = (cKlo < kk) ? kk : cKlo;
        }
        else
        {
            cKhi = (kk < cKhi) ? kk : cKhi;
        }
        if(cKlo >= cKhi)
        {
            return 0;
        }
    }
    return 1;
}

// Adds the stretch of the edge inside lo..hi along its axis, in world units from
// lowEnd, to the hidden intervals, if it has length.
static void
shareOver(int ax, int lowEnd, int len, int lo, int hi)
{
    int kl;
    int kh;
    int t;
    int dir;

    if(hi <= lo)
    {
        return;
    }
    kl = stepRound(lo - lowEnd, len);
    kh = stepRound(hi - lowEnd, len);
    dir = (ax == 2) ? cDy : cDx;
    if(dir < 0)
    {
        t = cL - kh;
        kh = cL - kl;
        kl = t;
    }
    if(kh > kl)
    {
        intLo[nInt] = kl;
        intHi[nInt] = kh;
        nInt++;
    }
}

// An object's box along an axis.
static void
boxSpan(const Object *oP, int ax, int *loP, int *hiP)
{
    switch(ax)
    {
    case 0:
        *loP = oP->x;
        *hiP = oP->x + oP->dsx;
        break;
    case 1:
        *loP = oP->y;
        *hiP = oP->y + oP->dsy;
        break;
    default:
        *loP = oP->z;
        *hiP = oP->z + oP->dsz;
        break;
    }
}

// Lists the objects that can draw part of an edge of the working object. Those
// that outrank it, are neither held nor a wall, and have a box that touches the
// working object's.
static void
shareObj(void)
{
    int o;
    int f;
    int rank;
    int rown;
    int lo;
    int hi;
    int touching;
    const Object *oP;

    shareN = 0;
    if(!cEmit || (cOwner < 0))
    {
        return;
    }
    for(f = 0; f < 3; f++)
    {
        shareBlo[f] = gLoc[f];
        shareBhi[f] = gLoc[f] + gDraw[f];
    }
    rown = intensityOf[objs[cOwner].color];
    for(o = 0; o < NOBJ; o++)
    {
        oP = &objs[o];
        rank = intensityOf[oP->color];
        if((rank < rown) || ((rank == rown) && (o >= cOwner)))
        {
            continue;
        }
        if((o == held) || (oP->kind == KWALL))
        {
            continue;
        }
        touching = 1;
        for(f = 0; f < 3; f++)
        {
            boxSpan(oP, f, &lo, &hi);
            if((lo > shareBhi[f]) || (shareBlo[f] > hi))
            {
                touching = 0;
            }
        }
        if(touching)
        {
            shareCand[shareN] = oP;
            shareN++;
        }
    }
}

// Adds to the edge's hidden intervals the stretches another object draws. Such
// edges lie along an axis, on both objects' boxes. Of the other's box the lines
// drawn are all twelve of a wireframe, all but the three through the back corner
// of a solid, and of a pyramid only its base by the same rule.
static void
shareClip(void)
{
    int f;
    int k;
    int ax;
    int len;
    int lo;
    int hi;
    int back;
    int zHigh;
    int on;
    const Object *oP;

    if(!cEmit || (cOwner < 0) || !shareN)
    {
        return;
    }
    ax = -1;
    len = 0;
    for(f = 0; f < 3; f++)
    {
        if(eHi[f] != eLo[f])
        {
            if(ax >= 0)
            {
                return;
            }
            ax = f;
            len = eHi[f] - eLo[f];
        }
    }
    if(ax < 0)
    {
        return;
    }
    for(k = 0; k < shareN; k++)
    {
        oP = shareCand[k];
        back = 0;
        zHigh = 0;
        on = 1;
        for(f = 0; f < 3; f++)
        {
            if(f == ax)
            {
                continue;
            }
            boxSpan(oP, f, &lo, &hi);
            if(eLo[f] == lo)
            {
                back += (f != 1);
            }
            else if(eLo[f] == hi)
            {
                zHigh |= (f == 2);
                back += (f == 1);
            }
            else
            {
                on = 0;
            }
        }
        if(!on)
        {
            continue;
        }
        if((oP->kind == KPYRAMID) && ((ax == 2) || zHigh))
        {
            continue;
        }
        if((back == 2) && !(oP->flags & FTRANS))
        {
            continue;
        }
        boxSpan(oP, ax, &lo, &hi);
        shareOver(ax, eLo[ax], len, (eLo[ax] > lo) ? eLo[ax] : lo,
                  (eHi[ax] < hi) ? eHi[ax] : hi);
    }
}

static void
pointAdd(Points *pP, int x, int y, int intensity)
{
    if((x < 0) || (x > RASTERMAX) || (y < 0) || (y > RASTERMAX))
    {
        return;
    }
    if(pP->n == pP->cap)
    {
        pP->cap = pP->cap ? (pP->cap * 2) : 4096;
        pP->wordP = realloc(pP->wordP, pP->cap * sizeof(uint32_t));
    }
    pP->wordP[pP->n] = (uint32_t)x | ((uint32_t)y << 10) | ((uint32_t)intensity << 20);
    pP->n++;
}

// The points of one vector from (x, y), as the 340 steps it. Each axis follows the
// straight line on its own, stepping when the line passes the half way to the next
// unit, and a step of either lights a point, so a diagonal goes round its corners.
// The 340 does not light the point it starts from.
static void
vectorPoints(Points *pP, int x, int y, int dx, int dy, int intensity)
{
    int adx;
    int ady;
    int sx;
    int sy;
    int i;
    int j;
    int xTime;
    int yTime;

    adx = abs(dx);
    ady = abs(dy);
    sx = (dx < 0) ? -1 : 1;
    sy = (dy < 0) ? -1 : 1;
    i = 0;
    j = 0;
    while((i < adx) || (j < ady))
    {
        // The i-th step of x comes at (2i + 1) / 2adx of the way.
        // The two are compared across.
        xTime = (i < adx) ? (((2 * i) + 1) * ady) : INT_MAX;
        yTime = (j < ady) ? (((2 * j) + 1) * adx) : INT_MAX;
        if(xTime <= yTime)
        {
            x += sx;
            i++;
        }
        if(yTime <= xTime)
        {
            y += sy;
            j++;
        }
        pointAdd(pP, x, y, intensity);
    }
}

// Moves the beam to (tx, ty) in as few vectors as the 340's delta allows, equal
// parts but for rounding, drawing them if visible. The split decides the points.
static void
dlMove(int tx, int ty, int visible)
{
    int dx;
    int dy;
    int adx;
    int ady;
    int m;
    int n;
    int k;
    int px;
    int py;
    int nx;
    int ny;

    dx = tx - dlCx;
    dy = ty - dlCy;
    adx = abs(dx);
    ady = abs(dy);
    m = (ady >= adx) ? ady : adx;
    if(m == 0)
    {
        return;
    }
    n = (m + (VECTOR_MAX - 1)) / VECTOR_MAX;
    px = dlCx;
    py = dlCy;
    for(k = 1; k <= n; k++)
    {
        nx = dlCx + ((dx < 0) ? -((adx * k) / n) : ((adx * k) / n));
        ny = dlCy + ((dy < 0) ? -((ady * k) / n) : ((ady * k) / n));
        if(visible)
        {
            vectorPoints(groupP, px, py, nx - px, ny - py, dlIntensity);
        }
        px = nx;
        py = ny;
    }
    dlCx = tx;
    dlCy = ty;
}

// Adds a piece to the group, from where the beam is if it can start there.
static void
dlSeg(int x0, int y0, int x1, int y1)
{
    if((x0 == x1) && (y0 == y1))
    {
        return;
    }
    if(dlRun && dlRel)
    {
        dlMove(x0, y0, 0);
    }
    else if(!dlRun || (y0 != dlCy) || (x0 != dlCx))
    {
        dlCx = x0;
        dlCy = y0;
        dlRun = 1;
    }
    dlMove(x1, y1, 1);
}

static void
dlBegin(Points *pP, int intensity)
{
    groupP = pP;
    dlIntensity = intensity;
    dlRun = 0;
    dlRel = 0;
}

// round(m k / L) with the sign of s, which is one axis of the point at step k.
static int
stepAxis(int m, int s, int k)
{
    int q;
    int r;

    q = (m * k) / cL;
    r = (m * k) % cL;
    if(r >= (cL - r))
    {
        q++;
    }
    return (s < 0) ? -q : q;
}

// One visible piece of the working edge, steps a to b: drawn, and kept if it is the
// longest the object has shown so far, for its label.
static void
piece(int a, int b)
{
    int x0;
    int y0;
    int x1;
    int y1;
    int len;

    x0 = e0x + stepAxis(cAdx, cDx, a);
    y0 = e0y + stepAxis(cAdy, cDy, a);
    x1 = e0x + stepAxis(cAdx, cDx, b);
    y1 = e0y + stepAxis(cAdy, cDy, b);
    len = abs(x1 - x0) + abs(y1 - y0);
    if((cOwner >= 0) && (len > bestLen[cOwner]))
    {
        bestLen[cOwner] = len;
        bestX[cOwner] = (x0 + x1) >> 1;
        bestY[cOwner] = (y0 + y1) >> 1;
    }
    dlSeg(x0, y0, x1, y1);
}

// Turns the hidden intervals into visible pieces. Sorted by their starts, each
// gap between them is a piece, and so are the ends.
static void
pieces(void)
{
    int k;
    int m;
    int a;
    int b;
    int pos;

    for(k = 1; k < nInt; k++)
    {
        a = intLo[k];
        b = intHi[k];
        for(m = k; (m > 0) && (a < intLo[m - 1]); m--)
        {
            intLo[m] = intLo[m - 1];
            intHi[m] = intHi[m - 1];
        }
        intLo[m] = a;
        intHi[m] = b;
    }
    pos = 0;
    for(k = 0; k < nInt; k++)
    {
        if(pos < intLo[k])
        {
            piece(pos, intLo[k]);
        }
        if(pos < intHi[k])
        {
            pos = intHi[k];
        }
    }
    if(pos < cL)
    {
        piece(pos, cL);
    }
}

// Clips one edge, on the screen with its world box, against the occluders.
// The hiders go into cMask, and with cEmit set the visible pieces are drawn.
static void
clipOne(void)
{
    int i;
    int minx;
    int maxx;
    int miny;
    int maxy;
    Occluder q;

    cDx = e1x - e0x;
    cDy = e1y - e0y;
    cAdx = abs(cDx);
    cAdy = abs(cDy);
    cL = (cAdy >= cAdx) ? cAdy : cAdx;
    if(cL == 0)
    {
        return;
    }
    minx = (e1x < e0x) ? e1x : e0x;
    maxx = (e1x < e0x) ? e0x : e1x;
    miny = (e1y < e0y) ? e1y : e0y;
    maxy = (e1y < e0y) ? e0y : e1y;
    nInt = 0;
    for(i = 0; i < occN; i++)
    {
        q = occTab[i];
        if((q.sminx >= maxx) || (minx >= q.smaxx) || (q.sminy >= maxy) || (miny >= q.smaxy))
        {
            continue;
        }
        if((q.owner == cOwner) && (cOwner != BOXOBJ))
        {
            continue;
        }
        if(q.wall && cInBox)
        {
            continue;
        }
        if(!inFront(&q) || !cyrus(&q) || !behind(&q))
        {
            continue;
        }
        intLo[nInt] = cKlo;
        intHi[nInt] = cKhi;
        nInt++;
        cMask |= 1u << q.owner;
    }
    shareClip();
    if(cEmit)
    {
        pieces();
    }
}

// Clips the edges of the object geomBuild made last: all of them for a
// wireframe, else the ones that can be seen.
static void
clipEdges(void)
{
    int n;
    int k;
    int w;
    int vi;
    int vj;
    int f;
    int a[3];
    int b[3];

    cInBox = inBox();
    shareObj();
    n = shapeP->nVisible + (cWhole ? shapeP->nHidden : 0);
    for(k = 0; k < n; k++)
    {
        w = shapeP->edgeP[k];
        vi = w >> 6;
        vj = w & 077;
        e0x = gsx[vi];
        e0y = gsy[vi];
        e1x = gsx[vj];
        e1y = gsy[vj];
        a[0] = gvx[vi];
        a[1] = gvy[vi];
        a[2] = gvz[vi];
        b[0] = gvx[vj];
        b[1] = gvy[vj];
        b[2] = gvz[vj];
        for(f = 0; f < 3; f++)
        {
            eLo[f] = (a[f] < b[f]) ? a[f] : b[f];
            eHi[f] = (a[f] < b[f]) ? b[f] : a[f];
        }
        clipOne();
    }
}

// The table's four edges. Everything stands on it, so everything is in front.
static void
clipTable(void)
{
    int k;

    cOwner = -1;
    cInBox = 0;
    shareN = 0;
    memset(eLo, 0, sizeof(eLo));
    memset(eHi, 0, sizeof(eHi));
    for(k = 0; k < 4; k++)
    {
        e0x = tableEdges[k][0];
        e0y = tableEdges[k][1];
        e1x = tableEdges[k][2];
        e1y = tableEdges[k][3];
        clipOne();
    }
}

// The held object, every edge, unclipped, drawn relative to its first point,
// which follows the hand.
static void
heldBuild(Points *pP)
{
    const Object *oP;
    int size[3];
    int n;
    int k;
    int w;
    int hx;
    int hy;

    pP->n = 0;
    heldFirst = 0;
    if(held < 0)
    {
        return;
    }
    oP = &objs[held];
    size[0] = oP->dsx;
    size[1] = oP->dsy;
    size[2] = oP->dsz;
    geomBuild(oP->kind, size, handX - (oP->dsx >> 1), handY - (oP->dsy >> 1), handZ - oP->dsz);
    dlBegin(pP, intensityOf[oP->color]);
    dlRel = 1;
    sceneProject(handX, handY, handZ, &hx, &hy);
    heldOffX = gsx[2] - hx;
    heldOffY = gsy[2] - hy;
    heldFirst = gsx[2] | (gsy[2] << 10);
    n = shapeP->nVisible + shapeP->nHidden;
    for(k = 0; k < n; k++)
    {
        w = shapeP->edgeP[k];
        dlSeg(gsx[w >> 6], gsy[w >> 6], gsx[w & 077], gsy[w & 077]);
    }
}

// Rebuilds the scene. What changed is drawn again, which here is all of it.
static void
rebuild(void)
{
    static Points building;
    static Points buildingHeld;
    Points t;
    long t0;
    int c;
    int o;
    int size[3];
    const Object *oP;

    if(detached)
    {
        return;
    }
    t0 = nowMicros();
    occBuild();
    gClamp = 1;
    cEmit = 1;
    building.n = 0;
    memset(bestLen, 0, sizeof(bestLen));
    for(c = CRED; c <= CWHITE; c++)
    {
        dlBegin(&building, intensityOf[c]);
        for(o = 0; o < NOBJ; o++)
        {
            oP = &objs[o];
            if((oP->kind == KWALL) || (o == held) || (oP->color != c))
            {
                continue;
            }
            size[0] = oP->dsx;
            size[1] = oP->dsy;
            size[2] = oP->dsz;
            cOwner = o;
            cWhole = oP->flags & FTRANS;
            geomBuild(oP->kind, size, oP->x, oP->y, oP->z);
            clipEdges();
        }
        if(c == CWHITE)
        {
            clipTable();
        }
    }
    heldBuild(&buildingHeld);
    pthread_mutex_lock(&showLock);
    t = shownScene;
    shownScene = building;
    building = t;
    t = shownHeld;
    shownHeld = buildingHeld;
    buildingHeld = t;
    pthread_mutex_unlock(&showLock);
    rebuildMicros = nowMicros() - t0;
}

// The objects that hide part of an object's drawn edges were it at (lx, ly, lz).
// Returns their bits, the object's bit not among them.
static unsigned
hidersOf(int o, int lx, int ly, int lz)
{
    const Object *oP;
    int size[3];

    occBuild();
    oP = &objs[o];
    size[0] = oP->dsx;
    size[1] = oP->dsy;
    size[2] = oP->dsz;
    gClamp = 0;
    cEmit = 0;
    cMask = 0;
    cOwner = o;
    cWhole = oP->flags & FTRANS;
    geomBuild(oP->kind, size, lx, ly, lz);
    clipEdges();
    return cMask & ~(1u << o);
}

// Makes wireframes of the objects whose bits are set; says whether any changed.
static int
setTrans(unsigned mask)
{
    int o;
    int changed;

    changed = 0;
    for(o = 0; o < NOBJ; o++)
    {
        if((mask & (1u << o)) && !(objs[o].flags & FTRANS))
        {
            objs[o].flags |= FTRANS;
            changed = 1;
        }
    }
    return changed;
}

// Waits for the frame being drawn to end, so that a step of the hand is applied
// between frames, as the PDP-1 program's is, and two never fall in one frame.
static void
frameWait(void)
{
    long f0;
    long t0;

    f0 = frames;
    t0 = nowMicros();
    while((frames == f0) && ((nowMicros() - t0) < FRAME_WAIT_MAX_MICROS))
    {
        usleep(200);
    }
}

// Shows the hand at a screen point when it is due. A step of a move is applied
// at the first frame's end after it is due, and sets the next due time one
// period after this one's, so that steps keep the period as their mean, and
// never bursts to catch up.
static void
handShow(int x, int y, int paced)
{
    long t0;
    long now;

    if(detached)
    {
        return;
    }
    t0 = nowMicros();
    sleepUntil(handDue);
    if(paced)
    {
        frameWait();
    }
    pthread_mutex_lock(&showLock);
    shownX = x;
    shownY = y;
    pthread_mutex_unlock(&showLock);
    now = nowMicros();
    handWaitMicros += now - t0;
    if(paced)
    {
        handDue += PERIOD_MICROS;
        if(handDue <= now)
        {
            handDue = now + PERIOD_MICROS;
        }
    }
}

static void
handNow(void)
{
    handDue = nowMicros();
}

// Puts the hand on the screen at its location at once.
static void
handJump(void)
{
    int sx;
    int sy;

    sceneProject(handX, handY, handZ, &sx, &sy);
    handNow();
    handShow(sx, sy, 0);
}

// One leg of a move. Each pass shows the hand, then steps each axis still moving
// or puts it at its end; an axis stops when within a step of its end.
static void
leg(int x, int y, int z)
{
    int nx;
    int ny;
    int ix;
    int iy;
    int sx;
    int sy;
    int fx;
    int fy;

    sceneProject(x, y, z, &nx, &ny);
    ix = shownX;
    iy = shownY;
    sx = (nx >= ix) ? STEPX : -STEPX;
    sy = (ny >= iy) ? STEPY : -STEPY;
    fx = 1;
    fy = 1;
    while(fx || fy)
    {
        if(abs(nx - ix) < STEPX)
        {
            fx = 0;
        }
        if(abs(ny - iy) < STEPY)
        {
            fy = 0;
        }
        handShow(ix, iy, 1);
        ix = fx ? (ix + sx) : nx;
        iy = fy ? (iy + sy) : ny;
    }
    handShow(nx, ny, 0);
}

// The hand from where it is to the point given, by up, across in y, across in x,
// down, or by a jump if it is to go fast.
static void
moveHand(int x, int y, int z)
{
    if((x == handX) && (y == handY) && (z == handZ))
    {
        return;
    }
    if(fastHand || detached)
    {
        handX = x;
        handY = y;
        handZ = z;
        handJump();
        return;
    }
    handNow();
    leg(handX, handY, LIFTZ);
    leg(handX, y, LIFTZ);
    leg(x, y, LIFTZ);
    leg(x, y, z);
    handX = x;
    handY = y;
    handZ = z;
}

void
sceneMoveto(int x, int y, int z)
{
    const Object *oP;

    if(held >= 0)
    {
        oP = &objs[held];
        if(setTrans(hidersOf(held, x - (oP->dsx >> 1), y - (oP->dsy >> 1), z - oP->dsz)))
        {
            rebuild();
        }
    }
    moveHand(x, y, z);
}

// Lets go of the held object. It stays a wireframe, centered below the hand.
static void
letGo(void)
{
    Object *oP;

    oP = &objs[held];
    oP->x = handX - (oP->dsx >> 1);
    oP->y = handY - (oP->dsy >> 1);
    oP->z = handZ - oP->dsz;
    oP->flags |= FTRANS;
    held = -1;
}

void
sceneGrasp(int obj)
{
    Object *oP;
    unsigned mask;

    oP = &objs[obj];
    if((oP->kind != KBLOCK) && (oP->kind != KPYRAMID))
    {
        termText("NOT AN OBJECT\n");
        return;
    }
    if(held >= 0)
    {
        termText("I AM ALREADY GRASPING SOMETHING\n");
        letGo();
    }
    handX = (oP->dsx >> 1) + oP->x;
    handY = (oP->dsy >> 1) + oP->y;
    handZ = oP->dsz + oP->z;
    handJump();
    mask = hidersOf(obj, oP->x, oP->y, oP->z);
    setTrans(mask | (1u << obj));
    held = obj;
    rebuild();
}

void
sceneUngrasp(void)
{
    if(held < 0)
    {
        termText("NOT GRASPING ANYTHING\n");
        return;
    }
    letGo();
    rebuild();
}

// Every object solid but the held one. A question's empty plan comes here too, and
// with no flag to change the list is right as it stands.
void
sceneRefresh(void)
{
    int o;
    int flags;
    int changed;

    changed = 0;
    for(o = 0; o < NOBJ; o++)
    {
        flags = (o == held) ? FTRANS : 0;
        changed |= objs[o].flags != flags;
        objs[o].flags = flags;
    }
    if(changed)
    {
        rebuild();
    }
}

void
sceneReset(void)
{
    memcpy(objs, objStart, sizeof(objs));
    held = -1;
    handX = HANDX;
    handY = HANDY;
    handZ = HANDZ;
    if(!detached)
    {
        handJump();
    }
    rebuild();
}

int
sceneObject(const char *nameP)
{
    int o;

    if(nameP[0] != ':')
    {
        return -1;
    }
    for(o = 0; o < NOBJ; o++)
    {
        if(!strcmp(nameP + 1, objNames[o]))
        {
            return o;
        }
    }
    return -1;
}

// OCCUPIER finds the first object, in table order, whose open box holds the point.
// Returns -1 for none, -2 below the table.
static int
occupier(int x, int y, int z, int skip)
{
    int o;
    const Object *oP;

    if(z < 0)
    {
        return -2;
    }
    for(o = 0; o < NOBJ; o++)
    {
        oP = &objs[o];
        if((o != skip) && (x >= oP->x) && (x < (oP->x + oP->sx))
           && (y >= oP->y) && (y < (oP->y + oP->sy))
           && (z >= oP->z) && (z < (oP->z + oP->sz)))
        {
            return o;
        }
    }
    return -1;
}

// SUPPORT finds what holds an object up. Returns -2 for the table, -1 for nothing.
static int
support(int o)
{
    const Object *oP;
    int under;

    if(o == held)
    {
        return -1;
    }
    oP = &objs[o];
    if(oP->z == 0)
    {
        return -2;
    }
    under = occupier((oP->sx >> 1) + oP->x, (oP->sy >> 1) + oP->y, oP->z - 1, held);
    return (under == o) ? -1 : under;
}

void
sceneOut(void)
{
    int o;
    int j;
    int any;
    char buf[160];
    const Object *oP;

    termText("CURRENT SCENE\n");
    for(o = 0; o < NOBJ; o++)
    {
        oP = &objs[o];
        if(oP->kind == KWALL)
        {
            continue;
        }
        snprintf(buf, sizeof(buf), ":%s --> A %s %s %s AT (%o %o %o)", objNames[o],
                 ((oP->sx + oP->sy + oP->sz) >= 384) ? "LARGE" : "SMALL", colorWords[oP->color],
                 kindWords[oP->kind], (unsigned)oP->x, (unsigned)oP->y, (unsigned)oP->z);
        termText(buf);
        any = 0;
        for(j = 0; j < NOBJ; j++)
        {
            if((j != o) && (objs[j].kind != KWALL) && (support(j) == o))
            {
                snprintf(buf, sizeof(buf), "%s:%s", any ? " " : " SUPPORTS (", objNames[j]);
                termText(buf);
                any = 1;
            }
        }
        termText(any ? ")\n" : "\n");
    }
    snprintf(buf, sizeof(buf), "THE HAND IS GRASPING %s%s\n", (held >= 0) ? ":" : "",
             (held >= 0) ? objNames[held] : "NOTHING");
    termText(buf);
}

void
sceneStatusOut(void)
{
    char buf[120];

    snprintf(buf, sizeof(buf), "FRAMES %lo EDGES 0 CLAMPS %lo OVER 0 MS %lo\n",
             (unsigned long)frames & 0377777ul, (unsigned long)clamps,
             (unsigned long)(rebuildMicros / 1000L));
    termText(buf);
}

long
sceneHandMicros(void)
{
    return handWaitMicros;
}

void
sceneDetach(void)
{
    detached = 1;
}

// The hand's group at (hsx, hsy): its point, the arm up to ARMTOP in pieces of
// the 340's delta, and the two crossbars of the original's film, a short one at
// the arm's top and a longer one at the hand's point.
static void
handPoints(Points *pP, int hsx, int hsy)
{
    int hx;
    int hy;
    int y;
    int s;
    int left;
    int right;

    hx = clampTo(hsx, 0);
    hy = clampTo(hsy, 0);
    pointAdd(pP, hx, hy, INTENSITY_WHITE);
    y = hy;
    while(y < ARMTOP)
    {
        s = ((ARMTOP - y) > VECTOR_MAX) ? VECTOR_MAX : (ARMTOP - y);
        vectorPoints(pP, hx, y, 0, s, INTENSITY_WHITE);
        y += s;
    }
    left = ((hx - (HBARTOP >> 1)) < 0) ? 0 : (hx - (HBARTOP >> 1));
    right = ((hx + (HBARTOP >> 1)) > RASTERMAX) ? RASTERMAX : (hx + (HBARTOP >> 1));
    vectorPoints(pP, left, y, right - left, 0, INTENSITY_WHITE);
    left = ((hx - (HBARBOT >> 1)) < 0) ? 0 : (hx - (HBARBOT >> 1));
    right = ((hx + (HBARBOT >> 1)) > RASTERMAX) ? RASTERMAX : (hx + (HBARBOT >> 1));
    vectorPoints(pP, left, hy, right - left, 0, INTENSITY_WHITE);
}

// A letter's dots from (x, y), its lower left dot. Says the x the next letter begins at.
static int
glyphDots(Points *pP, int x, int y, char letter)
{
    size_t k;
    int r;
    int c;

    for(k = 0; k < (sizeof(glyphs) / sizeof(glyphs[0])); k++)
    {
        if(glyphs[k].letter != letter)
        {
            continue;
        }
        for(r = 0; r < GLYPH_ROWS; r++)
        {
            for(c = 0; c < GLYPH_COLS; c++)
            {
                if(glyphs[k].row[r] & (1 << ((GLYPH_COLS - 1) - c)))
                {
                    pointAdd(pP, x + (c * LABEL_STEP), y + (((GLYPH_ROWS - 1) - r) * LABEL_STEP),
                             LABEL_INTENSITY);
                }
            }
        }
    }
    return x + (LABEL_ADVANCE * LABEL_STEP);
}

// Puts the color labels up. Each colored object's two letters go beside the middle of its
// longest visible piece in the last rebuild, the held object's beside the point its
// drawing hangs from the hand by (an object with no visible piece has none). They are
// made from the scene as it stands, and stay until sceneLabelsOff. Nothing
// happens if they are up already, or switched off by SHRDLU_NOLABELS.
void
sceneLabelsOn(void)
{
    static Points building;
    Points t;
    const Object *oP;
    const char *wordP;
    int o;
    int x;
    int y;

    if(labelsOn || noLabels || detached)
    {
        return;
    }
    building.n = 0;
    for(o = 0; o < NOBJ; o++)
    {
        oP = &objs[o];
        if((oP->kind == KWALL) || (oP->color == CWHITE))
        {
            continue;
        }
        if(o == held)
        {
            x = shownX + heldOffX;
            y = shownY + heldOffY;
            x = (x < 0) ? 0 : x;
            y = (y < 0) ? 0 : y;
        }
        else if(bestLen[o] > 0)
        {
            x = bestX[o];
            y = bestY[o];
        }
        else
        {
            continue;
        }
        x += LABEL_DX;
        y += LABEL_DY;
        x = (x > LABEL_MAXX) ? LABEL_MAXX : x;
        y = (y > LABEL_MAXY) ? LABEL_MAXY : y;
        for(wordP = labelWords[oP->color]; *wordP; wordP++)
        {
            x = glyphDots(&building, x, y, *wordP);
        }
    }
    pthread_mutex_lock(&showLock);
    t = shownLabels;
    shownLabels = building;
    building = t;
    pthread_mutex_unlock(&showLock);
    labelsOn = 1;
}

void
sceneLabelsOff(void)
{
    if(!labelsOn || detached)
    {
        return;
    }
    pthread_mutex_lock(&showLock);
    shownLabels.n = 0;
    pthread_mutex_unlock(&showLock);
    labelsOn = 0;
}

// A whole frame's points, made from what is shown now.
static void
frameBuild(Points *pP)
{
    size_t i;
    int ax;
    int ay;
    int fx;
    int fy;
    int x;
    int y;
    uint32_t w;

    pP->n = 0;
    pthread_mutex_lock(&showLock);
    for(i = 0; i < shownScene.n; i++)
    {
        pointAdd(pP, (int)(shownScene.wordP[i] & 01777u), (int)((shownScene.wordP[i] >> 10) & 01777u),
                 (int)(shownScene.wordP[i] >> 20));
    }
    if(shownHeld.n)
    {
        ax = clampTo(shownX + heldOffX, 0);
        ay = clampTo(shownY + heldOffY, 0);
        fx = heldFirst & 01777;
        fy = (heldFirst >> 10) & 01777;
        for(i = 0; i < shownHeld.n; i++)
        {
            w = shownHeld.wordP[i];
            x = ((int)(w & 01777u) - fx) + ax;
            y = ((int)((w >> 10) & 01777u) - fy) + ay;
            pointAdd(pP, x, y, (int)(w >> 20));
        }
    }
    for(i = 0; i < shownLabels.n; i++)
    {
        pointAdd(pP, (int)(shownLabels.wordP[i] & 01777u), (int)((shownLabels.wordP[i] >> 10) & 01777u),
                 (int)(shownLabels.wordP[i] >> 20));
    }
    handPoints(pP, shownX, shownY);
    pthread_mutex_unlock(&showLock);
}

// Sends a frame's words at the 340's pace. Says whether the client is still there.
static int
frameSend(int fd, const Points *pP)
{
    size_t at;
    size_t k;
    size_t off;
    ssize_t r;
    long start;
    const char *byteP;

    start = nowMicros();
    at = 0;
    while(at < pP->n)
    {
        k = pP->n - at;
        k = (k > CHUNK_POINTS) ? CHUNK_POINTS : k;
        byteP = (const char *)&pP->wordP[at];
        off = 0;
        while(off < (k * sizeof(uint32_t)))
        {
            r = send(fd, byteP + off, (k * sizeof(uint32_t)) - off, MSG_NOSIGNAL);
            if(r <= 0)
            {
                return 0;
            }
            off += (size_t)r;
        }
        at += k;
        sleepUntil(start + (((long)at * MILLION) / POINT_RATE));
    }
    return 1;
}

// Drops what a client sends, the lightpen's words. Says whether it is still there.
static int
clientDrain(int fd)
{
    char buf[256];
    ssize_t r;

    for(;;)
    {
        r = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if(r > 0)
        {
            continue;
        }
        if(r == 0)
        {
            return 0;
        }
        return 1;
    }
}

// The refresh: frame after frame, to the client if there is one. A new client
// takes over from the last, which may be a dead window.
static void *
displayThread(void *argP)
{
    int listenFd;
    int client;
    int fd;
    int one;
    long t0;
    struct pollfd pfd;
    Points frame;

    listenFd = *(int *)argP;
    client = -1;
    memset(&frame, 0, sizeof(frame));
    for(;;)
    {
        t0 = nowMicros();
        pfd.fd = listenFd;
        pfd.events = POLLIN;
        if(poll(&pfd, 1, 0) > 0)
        {
            fd = accept(listenFd, NULL, NULL);
            if(fd >= 0)
            {
                if(client >= 0)
                {
                    close(client);
                }
                client = fd;
                one = 1;
                setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            }
        }
        frameBuild(&frame);
        if((client >= 0) && (!clientDrain(client) || !frameSend(client, &frame)))
        {
            close(client);
            client = -1;
        }
        frames++;
        sleepUntil(t0 + MIN_FRAME_MICROS);
    }
    return NULL;
}

void
sceneInit(int port, int fast)
{
    static int listenFd;
    struct sockaddr_in addr;
    pthread_t thread;
    const char *envP;
    int one;

    envP = getenv("SHRDLU_NOLABELS");
    noLabels = (envP != NULL) && (envP[0] == '1');
    fastHand = fast;
    memcpy(objs, objStart, sizeof(objs));
    held = -1;
    handX = HANDX;
    handY = HANDY;
    handZ = HANDZ;
    sceneProject(handX, handY, handZ, &shownX, &shownY);
    rebuild();
    listenFd = socket(AF_INET, SOCK_STREAM, 0);
    one = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if((bind(listenFd, (struct sockaddr *)&addr, sizeof(addr)) < 0) || (listen(listenFd, 4) < 0))
    {
        perror("display port");
        exit(1);
    }
    pthread_create(&thread, NULL, displayThread, &listenFd);
}
