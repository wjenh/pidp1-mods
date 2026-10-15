/*
 * t30dpy3 - a replacement for p7sim that uses SDL3.
 * See t30dpy for an SDL2 version.
 * This is MUCH lighter weight than p7sim and simulates the Type 30 behavior better.
 * The major differences are:
 * The color p7sim displayed was not totally correct for a p7 phosphor, the initial spot was too white.
 * The yellow phosphor decay time was too long.
 * See initializeRgbas() below for more details.
 * The increase in dot size with higher intensities to mimic beam spread was far too much.
 * Finally, p7sim was complex and an immense cpu hog. This version generally uses a fraction as much cpu.
 * It is a pure SDL3 application.
 * While it generally does not always perform significantly faster than the SDL2 version,
 * it is much better at smooth rendering and scaling and is current, SDL2 is very dated.
 *
 * Neither this nor p7sim accurately replicate the blue color, modern displays can't duplicate
 * the very short high intensity burst only a few microseconds in duration especially since the
 * framerate imposed by modern raster displays is far longer.
 * Two things are done to handle this.
 * First, the duration is extended across multiple frames. This compensates for the lack of intensity.
 * Second, the first frame can use a different rgb value to make it brighter. Unfortunately, this shifts the
 * color, but following frames use the correct rgb value.
 * It uses a logical window size of 1024*1024 to match the Type 30 display and lets SDL do the scaling.
 *
 * The variable dot size is handled by drawing a matrix of dots for each intensity.
 *
 * For maximum efficiency, all of the possible SDL rgba values we need are precomputed in initializeRgbas()
 * which results in no floating point calculations being done at all (by this code, at least)
 * while running in the display loop.
 *
 * An array of the full 1024 x 1024 points is used, but to avoid iterating over the full size every display cycle
 * where most points will be empty, active points are linked in a list and only those are examined.
 *
 * Author - Bill Ezell, wje
 * This can be freely used, modified, whatever. Please just keep the attribution to me.
 *
 * 6-Jun-2026 wje initial version, cloned from t30dpy.c
 * 8-Jun-2026 wje general cleanup, some optimizations
 * 9-Jun-2026 wje major rework to use an active list instead of full iteration over all points,
 *    add workaround for SDL3 overwriting the meun bar if on the bottom of the screen
 * 10-Jun-2026 wje add workaround for totally broken wayland/labwc window control
 * 11-Jun-2026 wje CPU optimization pass for high active-point counts.
 *    - replaced the per-screen-position pointData[1024][1024] active list with a compact
 *      index-linked activePool[] (MAXACTIVEPOINTS) plus a pointIndex[][] lookup table,
 *      to keep active-list traversal cache-friendly.
 *    - drawPoint() now computes row base pointers once per point instead of recomputing
 *      full address arithmetic for every one of the up to 9 pixels it can touch.
 *    - rgbaValues[][] now stores premultiplied-alpha rgb, allowing
 *      SDL_BLENDMODE_NONE instead of SDL_BLENDMODE_BLEND and removing the per-frame
 *      SDL_RenderClear(), eliminating a full-screen blend and a full-screen clear
 *      every frame.
 * 11-Jun-2026 wje fix regression from above: in fullscreen with SDL_LOGICAL_PRESENTATION_LETTERBOX,
 *    removing the per-frame SDL_RenderClear() left the letterbox bars (and edge points'
 *    scaled bleed into them) uncleared from prior frames, showing as phantom points outside
 *    the 1024x1024 area. Restored SDL_RenderClear() before SDL_RenderTexture()
 * 12-Jun-2026 wje minor code cleanup, remove some unused vars
 * 13-Jun-2026 wje (Claude) switch pthread usage to SDL_Thread/_Mutex, preparation for win11 compatibility.
 *    No functional change on Linux.
 * 13-Jun-2026 wje (Claude) add Windows 11 (MSYS2/UCRT64) support via wincompat.h,
 *    guarded by #ifdef _WIN32. Sockets, signal handling, the monotonic clock and
 *    home-directory lookup are routed through small macros/wrappers so this
 *    single source file builds on both Linux and Windows. No functional change
 *    on Linux.
 * 15-Jun-2026 fix bit setting size error in lightpen commands
 * 15-Jun-2026 don't apply wayland/labwc fix if user explicitly set a size
 * 17-Jun-2026 wje (Claude) declare activeListHead, freeListHead, quit as volatile to prevent
 *    the optimizer from caching them in registers across frames; symptom was display going
 *    blank after many hours while the program continued running.
 * 17-Jun-2026 wje (Claude) right-mouse-button drag: SDL_BUTTON_RIGHT down/up sets dragging
 *    state; SDL_EVENT_MOUSE_MOTION while dragging calls SDL_SetWindowPosition with the
 *    delta from the button-down origin. Drag coords are captured before any render-space
 *    conversion so they remain in raw window space.
 * 18-Jun-2026 wje (Claude) reclaim SIGINT from SDL3: SDL_HINT_NO_SIGNAL_HANDLERS before
 *    SDL_Init() prevents SDL3 from installing its own SIGINT handler; sighandler()
 *    registered for SIGINT along with SIGHUP/SIGTERM. SDL_WaitThread() added before
 *    mutex/SDL_Quit() cleanup to prevent use-after-free if reader thread exits late.
 * 18-Jun-2026 wje extract reportTiming() function; call from sighandler() and reconfigure()
 *    so timing data is output on SIGINT and SIGHUP as well as normal exit. Move startTime,
 *    frameMisses, frameDelay to global scope so reportTiming() can reach them.
 * 18-Jun-2026 wje (Claude) hardcode SDL_PIXELFORMAT_RGBA8888 instead of querying the
 *    renderer's preferred format. The software renderer on rpi trixie returns ARGB8888;
 *    with the premultiplied-alpha scheme (alpha stored as 255, brightness in RGB) the
 *    0xFF alpha byte lands in a color channel, causing all points to appear at full
 *    brightness. RGBA8888 matches t30dpy (SDL2) and works on all backends.
 * 18-Jun-2026 wje (Claude) remove explicit SDL_DestroyRenderer/SDL_DestroyWindow before
 *    SDL_Quit(). On X11, those calls trigger XTranslateCoordinates on the window after X
 *    has invalidated the resource, producing a BadWindow error. SDL_Quit() sequences the
 *    teardown correctly on its own.
 * 18-Jun-2026 wje (Claude) replace SDL_CreateWindowAndRenderer with
 *    SDL_CreateWindowWithProperties + SDL_CreateRenderer so SDL_WINDOWPOS_CENTERED can be
 *    specified. X11 honors this; Wayland ignores it but centers new windows by default.
 * 19-Jun-2026 wje (Claude) fix the long-standing 12+ hour blank-display/unresponsive hang.
 *    Hold busyLockP for the entire active-point walk in main() instead of only inside
 *    removeActivePoint().
 * 20-Jun-2026 wje create the window initially hidden to avoid sdl window initialization jitter
 * 01-Jul-2026 wje (Claude) fix new points being overwritten by nearby aging points.
 *    DrawPoint() previously did a flat overwrite for every pixel, so whichever
 *    point happened to be walked last in the active list won.
 *    Adjust point pattern vs intensity for more realistic rendering.
 * 02-Jul-2026 wje SDL_RenderClear() is only needed when the visible window's aspect ratio
 *    differs from the 1024x1024 logical area.
 * 14-Jul-2026 wje Process mouse events in subframe intervals, faster lightpen updats.
 * 30-Sep-2026 Claude - exit on Escape even when the display is idle (shut the socket down to wake the reader),
 *    and the SIGINT/SIGTERM handler only sets quit, so a kill can no longer deadlock inside SDL.
 * 30-Sep-2026 Claude - the SIGHUP reload runs from the main loop, not inside the signal handler.
 * 02-Oct-2026 wje - pixel fading moved to the GPU, now the CPU handles only each frame's new points.
 * 03-Oct-2026 wje - work around SDL3 picking lavapipe when remotely connected, the renderer is
 *    actively probed to find opengl, opengles2 or software and uses the first that opens,
 *    unless the new renderer setting or -r gives one explicitly.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <signal.h>
#include <time.h>
#ifdef _WIN32
#include "wincompat.h"
#else
#include <unistd.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <pwd.h>
#include <sys/resource.h>       // setpriority(), PRIO_PROCESS -- voluntary nice-down
#include "wincompat.h"
#endif
#include <math.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#define READBUFSIZE 512         // size of the buffer we read from the server into, bytes

#define MINSIZE 256             // window can't be smaller than this
#define WAYLANDMARGIN 128       // to work around stupid wayland, the inital window must be physical size - this
#define LOGICALSIZE 1024        // really should not be changed, this is the authentic value
#define NUMPOINTS (LOGICALSIZE*LOGICALSIZE)   // total number of points on screen
#define MININTENSITY 16         // intensity 0 will be this, the rest are linearly scaled to 255 for level 7

// The alpha values for the color intensity over time are computed using a power law.
// Note that the alpha decay times are based on a lifetime of 255 frames.
#define BLUEDECAYALPHA  1.5     // intensity = time^-DECAYALPHA (time to the -alpha power) normalized to 0-255
#define BLUER 35
#define BLUEG 0
#define BLUEB 255
#define WHITEBIAS 180           // add this to both r and g for the blue phosphor in the first frame, makes it whiter

#define YELLOWDECAYALPHA  0.85
#define YELLOWRGB 179, 225, 0   // keep exactly this format, used as 3 args
#define MAXLIFETIME 255         // number of frames a point will exist unless its alptha falls below LOWCUTOFF

// After blending, the resulting alpha also has a power law applied to adjust for the nonlinear response of the eye.
// Smaller values enhance the brightness of low alphas, larger dims them.
#define GAMMA 0.4545             // default gamma, gamma applies after final alpha is calculated

#define LOWCUTOFF 5             // alphas less than this are not displayed
#define LINEAR false            // ask SDL renderer to use linear scaling

// MAXACTIVEPOINTS bounds the size of the active-point pool (see ActivePoint below).
// Worst-case observed live usage has been around 171000 simultaneously active points,
// so this is set with generous headroom above that.
// NOINDEX marks end of list or not active in the indexed pool.
#define MAXACTIVEPOINTS 400000
#define NOINDEX 0xFFFFFFFFu

// The GPU fade, the default, replaces the active-point walk; gpufade=false in the config file turns it
// off, and -f turns it on whatever the config file says.
// Each decay term is an 8-bit render target. Once a frame the renderer multiplies a whole target by
// n/255 and subtracts s/255, then draws that frame's new points into it, and the targets are added
// onto the screen at their weights. An 8-bit value multiplied alone stops falling once
// v * n / 255 rounds back to v; the subtraction keeps every term falling to black, so the fade
// ends in a short ramp instead of a step or a lasting glow.
// The renderer does exactly v = round(v * n / 255) - s, at least 0, when n and s are sent as
// n/255 and s/255 (measured on SDL 3.2.10 opengl, Pi 4, and SDL 3.4.2 opengl/opengles2/vulkan);
// initializeGpuFade() checks it on the renderer in use.
#define FADETERMS 3             // fast (it also carries the blue), mid and slow
#define FASTTERM 0
#define MAXNEWPOINTS 100000     // new points one frame takes in the GPU fade; more are dropped and counted
#define MAXSPREAD 9             // pixels one point covers at the highest intensities, see drawPoint()
#define DEFAULTTAIL 1.3f        // seconds until a full-intensity point has faded to black
#define MINWEIGHT 0.005f        // a term weighted less than this is not drawn at all
#define PACKPOINT(x, y, i) ((uint32_t)(x) | ((uint32_t)(y) << 10) | ((uint32_t)(i) << 20))
#define FADETABLEROWS ((int)(sizeof(fadeTable) / sizeof(fadeTable[0])))
#define FADETABLEFIRST 8        // fadeTable's first row is for a tail of 0.8 s, then every 0.1 s

// The following can be overridden by the config file and some also via the command line.
// For those that have a command line override, it takes precedence.
// See usage() below for those that have command line versions.
#define DEFAULTHOST "localhost"
#define DEFAULTPORT 3400
// Amount to raise our own nice value, lowerer our scheduling priority) so that when this
// client shares a CPU with the PDP-1 emulator, the emulator wins CPU contention and keeps
// feeding display points instead of stalling.
// 0 disables, overridable via the config file "nice=N".
#define DEFAULTNICE 5

#define NSECPERUSEC 1000
#define NSECPERMSEC 1000000
#define CURSORTIMEOUT 4000      // if no mouse motion after this many milliseconds, hide it

#define XYTOPTR(base, pitch, x, y) (uint32_t *)((base) + ((y) * (pitch)) + ((x) * sizeof(uint32_t)))
#define APPLYGAMMA(alpha, gamma) (int)(powf((float)(alpha) / 255.0f, (gamma)) * 255.0f);

#define CONSTRAIN(x) ((x) < 0?0:(((x) > 1023)?1023:(x)))    // keep a value in the range 0-1023
#define FRAMETIME 33333333L         // nanoseconds between frames, this is 30 fps
#define EVENTPOLLNS NSECPERMSEC     // Instead of waiting for each frame to update mouse events,
                                    // we update this often in the idle wait loop.
                                    // Matches the granularity of the poll time on the pdp-1 side.
#define DRAWIFBRIGHTER(pixP, brightP) \
    if(bright >= *(brightP)) {*(pixP) = rgba; *(brightP) = (uint8_t)bright;}

// Commands sent to emulator.
#define CMDBITS 0xFF000000
#define LPCMD   0xFF000000
#define PENBITS 0x00F00000
#define LPUP    0x00100000

typedef unsigned char byte;
typedef uint32_t Rgba;

// The description of a point to display.
// It contains the values to select the intensity over time for the two phospors,
// the fast-decay blue-purple and the slow-decay yellow-green.
// The lifetime field is used to select the alpha for the decay interval and ranges from 0-255.
// The phosphors decay in intensity by power-law decay, not exponential decay.
// The intensity field ranges from 0-7 and is used to select scaled alpha values, 0 being dimmest, 7 brightest.
// In the display loop, precomputed values are used and are selected by the combination of intensity and lifetime.
// The use of uint16_t here but int elsewhere is intentional and dome for rather abstract reasons dealing
// with compiler optimization and allowing it to use optimized cpu instructions.
// The benefit is minor, but we're potentially dealing with fairly low performance cpus, every little bit helps.
//
// ActivePoint records live in a small, contiguous pool, activePool[],
// and the list is threaded via array indices (nextIdx) rather than pointers.
// A separate, much smaller lookup table, pointIndex[][], maps a logical (x,y) screen
// position to its slot in the pool, or to NOINDEX if that position is not currently active.
// This keeps the working set during traversal limited to roughly
// (active point count * sizeof(ActivePoint)) instead of the full 25MB grid.
typedef struct ActivePoint {
    uint16_t x;                 // x and y could be computed, but this saves a few cycles
    uint16_t y;
    byte intensity;             // 0 - 7, PDP-1 intensity
    uint16_t lifetime;          // value is only 0-255, but use 16 bits because it would be padded to it anyway
    uint32_t nextIdx;           // index of the next entry in whichever list (active or free) this entry is on
} ActivePoint, *ActivePointP;

// One choice of the GPU fade's three terms. Each frame a term's value v (0-255) becomes
// round(v * n / 255) - s, at least 0; a point is drawn into every term at full scale for its
// intensity, and a term is added to the screen at its weight.
typedef struct FadeTerms {
    int life;                   // frames from 255 until the slow term, the last to go, reads 0
    int n[FADETERMS];           // per-frame factor, n/255
    int s[FADETERMS];           // per-frame subtraction, s/255
    float w[FADETERMS];         // weight on the screen
} FadeTerms;

// We keep the active points in a pool that effectively implements 2 linked lists.
// One is a list of free pool entries, the other a list of active points.
// As points come in, an entry is moved from the free list to the active list.
// When a point is no longer displayed, it is moved back to the free list.
// This way we don't have to check every point location to see if it's valid.
// Locking is not done between the reader and display threads.
// When a new item is added to the busy list, it is added to the head and the link atomically updated.
// When an item is taken from the free list, it is always taken from the head.
// When an item is moved from the busy list, it is added to the head of the free list.
ActivePoint activePool[MAXACTIVEPOINTS];        // compact pool of active-point records
uint32_t pointIndex[LOGICALSIZE][LOGICALSIZE];  // maps screen (y,x) -> pool index, or NOINDEX
volatile uint32_t activeListHead;               // head index of the active list, or NOINDEX if empty
volatile uint32_t freeListHead;                 // head index of the free list, or NOINDEX if exhausted
uint64_t droppedPoints;                         // counts points dropped because the pool was exhausted
SDL_Mutex *busyLockP;                           // for interlocking with the reader thread

Rgba rgbaValues[8][256];        // The rgba values for each possibe intensity and internal time step.

// Perceptual brightness for each possible intensity and lifetime.
// This is the value drawPoint() uses to decide whether a point should override a pixel some other point already
// wrote this frame.
uint8_t brightValues[8][256];

// Tracks the brightest value written to each screen pixel so far in the current frame.
uint8_t brightBuffer[LOGICALSIZE][LOGICALSIZE];

// The GPU fade's terms, one row per tail length from 0.8 s to 2.7 s in steps of 0.1 s.
// Each row's weights are a least-squares fit to the P7 yellow-green persistence curve of RCA
// TPM-1508A page 13, (1 + t / 16.9 ms)^-0.73 averaged over each 33 ms frame, as encoded values
// (gamma 0.4545), over 0-400 ms, the range the curve is published for. Past 400 ms a row is not
// fitted: it fades to black by its tail. Short tails fit 0-400 ms worse, as noted per row.
static const FadeTerms fadeTable[] = {
    { 25, {  30, 150, 250 }, { 0, 2, 8 }, { 0.244f, 0.001f, 0.759f } },  // 0.8 s, within 35.2% to 400 ms
    { 27, {  30, 234, 252 }, { 0, 4, 8 }, { 0.283f, 0.001f, 0.722f } },  // 0.9 s, within 22.5%
    { 30, {  80, 240, 254 }, { 0, 4, 8 }, { 0.333f, 0.011f, 0.665f } },  // 1.0 s, within 14.9%
    { 32, {  90, 240, 253 }, { 0, 3, 7 }, { 0.339f, 0.001f, 0.667f } },  // 1.1 s, within 10.8%
    { 37, { 100, 150, 253 }, { 0, 4, 6 }, { 0.211f, 0.153f, 0.634f } },  // 1.2 s, within 6.0%
    { 40, {  90, 150, 254 }, { 0, 4, 6 }, { 0.130f, 0.257f, 0.611f } },  // 1.3 s, within 3.6%
    { 43, {  60, 150, 253 }, { 0, 3, 5 }, { 0.096f, 0.295f, 0.609f } },  // 1.4 s, within 2.0%
    { 43, {  60, 150, 253 }, { 0, 3, 5 }, { 0.096f, 0.295f, 0.609f } },  // 1.5 s, within 2.0%
    { 47, {  50, 168, 254 }, { 0, 3, 5 }, { 0.126f, 0.295f, 0.578f } },  // 1.6 s, within 0.9%
    { 52, {  50, 168, 253 }, { 0, 2, 4 }, { 0.127f, 0.296f, 0.577f } },  // 1.7 s, within 0.5%
    { 52, {  50, 168, 253 }, { 0, 2, 4 }, { 0.127f, 0.296f, 0.577f } },  // 1.8 s, within 0.5%
    { 58, {  60, 180, 254 }, { 0, 2, 4 }, { 0.155f, 0.299f, 0.546f } },  // 1.9 s, within 0.3%
    { 58, {  60, 180, 254 }, { 0, 2, 4 }, { 0.155f, 0.299f, 0.546f } },  // 2.0 s, within 0.3%
    { 64, {  60, 168, 250 }, { 0, 3, 2 }, { 0.138f, 0.271f, 0.591f } },  // 2.1 s, within 1.0%
    { 66, {  60, 180, 253 }, { 0, 2, 3 }, { 0.155f, 0.299f, 0.546f } },  // 2.2 s, within 0.3%
    { 66, {  60, 180, 253 }, { 0, 2, 3 }, { 0.155f, 0.299f, 0.546f } },  // 2.3 s, within 0.3%
    { 71, {  60, 180, 251 }, { 0, 2, 2 }, { 0.163f, 0.274f, 0.563f } },  // 2.4 s, within 0.5%
    { 75, {  70, 192, 254 }, { 0, 1, 3 }, { 0.184f, 0.308f, 0.507f } },  // 2.5 s, within 0.4%
    { 75, {  70, 192, 254 }, { 0, 1, 3 }, { 0.184f, 0.308f, 0.507f } },  // 2.6 s, within 0.4%
    { 79, {  70, 186, 252 }, { 0, 1, 2 }, { 0.178f, 0.286f, 0.536f } }   // 2.7 s, within 0.7%
};

const FadeTerms *fadeTermsP;           // the row in use, see selectFadeTerms()
SDL_Texture *termTextures[FADETERMS];   // one 8-bit render target per term
SDL_BlendMode subtractMode;             // dst - src, for the per-frame subtraction
SDL_BlendMode maxMode;                  // max(dst, src), for drawing points into a term

// New points for the GPU fade, double buffered: the reader thread fills newPoints[newFill]
// under busyLockP, and the main loop takes it and switches newFill under the same lock.
uint32_t newPoints[2][MAXNEWPOINTS];
uint32_t newCount;
int newFill;

// One frame's new points spread to their pixels, grouped by intensity, see renderGpuFrame().
SDL_FPoint spreadPoints[MAXNEWPOINTS * MAXSPREAD];

float levelValues[8];           // each intensity's value drawn into a term, gamma applied
float fastColor[3];             // the fast term's color at full scale, see selectFadeTerms()

int pdp1FD;
int portNum;
char *hostNameP;
const char *driverNameP;
char *rendererSettingP;         // the renderer setting or -r, NULL or "auto" for createRenderer()'s order

int winSize;
int lowCutoff = LOWCUTOFF;
int whiteBias = WHITEBIAS;
int niceValue = DEFAULTNICE;    // see DEFAULTNICE; 0 disables, config "nice=N" overrides

bool allowLabwcFix = true;
bool usingLabwc = false;
volatile bool quit = false;
volatile bool reloadRequested = false;  // set by SIGHUP, acted on by the main loop, see sighup()
bool border;
bool doLinear = LINEAR;
bool mikecMode = false;
bool gpuFade = true;            // the GPU fade instead of the active-point walk, see FADETERMS
int gpuIdleFrames;              // frames since the GPU fade last had a new point

float gammaCorrection = GAMMA;
float fadeTail = DEFAULTTAIL;   // the GPU fade's tail, seconds

uint32_t blackPixel;                 // The value is numeric 0, but use the SDL generated version for consistency.

// These are global only so reportTiming() can see them.
uint64_t startTime;
uint64_t frameDelay;
uint32_t frameMisses;
uint64_t droppedNs;             // frame time the pacing dropped rather than catch up, see main()

// These values are used only for timing metrics.
uint64_t totalPoints;
uint64_t receivedPoints;
uint64_t totalFrames;
uint64_t maxActivePoints;
_Atomic  uint64_t activePoints;     // overly obsessive to get perfect counts, but only used for timing
uint64_t pacedFrames;        // every frame-paced main-loop pass, including idle passes with no active points
uint64_t renderTimeTotal;    // sum of per-rendered-frame times (ns), for the average
uint64_t renderTimeMax;      // worst single rendered-frame time (ns)
uint64_t renderCount;        // number of rendered frames timed
uint64_t phaseBufferTotal;   // sum of per-frame buffer work (lock + memset + point draw), ns
uint64_t phasePresentTotal;  // sum of per-frame present work (RenderClear + scaled blit + Present/VNC), ns
const char *rendererNameP;   // SDL renderer backend name, e.g. "opengl"/"opengles2" vs "software"
bool doTiming;

// These are all for SDL.
SDL_Window *window;
SDL_Renderer *renderer;
SDL_Texture *textures[2];           // Double buffered textures so we don't block while it is being processed.
SDL_Texture *textureP;              // The current texture.
int textureSelector;

SDL_PixelFormat pixelFormat;        // We will use whatever SDL3 tells us is preferred, set below.
const SDL_PixelFormatDetails *formatDetailsP;

int openPort(char *hostNameP, int port);
SDL_Renderer *createRenderer(void);
uint32_t blend(int srcR, int srcG, int srcB, int srcA, int destR, int destG, int destB, int destA);
uint64_t now(void);
int reader(void *argP);

void initializePoints(void);
void addActivePoint(uint16_t x, uint16_t y, byte intensity);
void removeActivePoint(uint32_t pointIdx, uint32_t prevIdx);

void initializeRgbas(void);
void drawPoint(uint8_t *pixels, int pitch, uint32_t rgba, int x, int y, int intensity, int bright);

void selectFadeTerms(void);
bool initializeGpuFade(void);
int fadeModel(int v, int n, int s);
bool fadeTerm(int term);
bool readTermPixel(int *valueP);
int spreadPoint(SDL_FPoint *outP, int x, int y, int intensity);
void renderGpuFrame(void);
void updatePen(int sockFD, bool penDown, int winX, int winY);
void loadConfig(bool full);
void sighandler(int sig);
void sighup(int sig);
void reconfigure(void);
void reportTiming(void);
void usage(void);
FILE *getFile(char *nameP);

// Keep track of what needs to be shared between the event processor and the main code.
typedef struct EventState {
    bool fullscreen;
    bool isLetterboxed;          // true when the window's aspect ratio differs from the 1024x1024
    bool penDown;
    int  penx, peny;
    bool dragging;               // true while the right mouse button is held for a window drag
    int  dragStartGlobalX;       // screen-absolute cursor x when right button was pressed
    int  dragStartGlobalY;       // screen-absolute cursor y when right button was pressed
    int  dragWinOriginX;         // window screen x when right button was pressed
    int  dragWinOriginY;         // window screen y when right button was pressed
    float fMouseGlobalX;         // scratch: SDL3 GetGlobalMouseState returns float
    float fMouseGlobalY;         // scratch: SDL3 GetGlobalMouseState returns float
    uint64_t cursorTime;
} EventState, *EventStateP;

static void handleEvent(SDL_Event *eventP, EventStateP stateP);

int
main(int argc, char **argv)
{
int opt;
int x, y;
uint32_t i;
char *cP;

uint64_t deltaTime;
uint64_t accumulator;       // Used for loop timing so the frane rate can be kept at 30fps.
uint64_t currentTime;
uint64_t lastTime;
uint64_t renderStart;       // timing: monotonic ns at the start of a rendered frame
uint64_t renderDelta;       // timing: duration (ns) of a rendered frame
uint64_t tAfterBuffer;      // timing: monotonic ns after buffer work, before present
uint64_t sleepRemaining;    // ns still left to wait before the next frame is due
uint64_t thisSleep;         // ns to sleep this slice of the wait, see EVENTPOLLNS

uint32_t pointIdx;
uint32_t prevIdx;
uint32_t nextIdx;
ActivePointP activePointP;
uint8_t *pixels;
uint32_t rgba;
int pitch;

SDL_Event event;
SDL_Rect bounds;
SDL_PropertiesID winPropsID;    // for SDL_CreateWindowWithProperties

SDL_Thread *threadP;

EventState evState;         // see typedef above main(): bundles the old penDown/dragging/etc locals

    // On Windows, Winsock must be initialized before any socket call.
    // winSockStartup() is a no-op returning 0 on Linux.
    if( winSockStartup() )
    {
        fprintf(stderr, "Winsock initialization failed.\n");
        exit(1);
    }

    hostNameP = DEFAULTHOST;
    portNum = DEFAULTPORT;        // display 0 on the pidp-1
    winSize = 1024;               // original Type 30 display size
    border = true;
    evState.fullscreen = false;
    // The window is always created with the same winSize value used for both width and height.
    // adn is therefore never letterboxed.
    // SDL_EVENT_WINDOW_RESIZED updates this if the user resizes
    // to a non-square shape or toggles fullscreen on a non-square display.
    evState.isLetterboxed = false;
    evState.penDown = false;
    doTiming = false;
    totalPoints = 0;
    receivedPoints = 0;
    frameMisses = 0;
    frameDelay = 0;
    evState.cursorTime = 0;
    evState.dragging = false;
    evState.dragStartGlobalX = 0;
    evState.dragStartGlobalY = 0;
    evState.dragWinOriginX = 0;
    evState.dragWinOriginY = 0;
    evState.fMouseGlobalX = 0.0f;
    evState.fMouseGlobalY = 0.0f;

    loadConfig(true);             // config overrides defines, command line overrides all

    while( (opt = getopt(argc, argv, "fg:lmnp:r:s:tvw:")) != -1 )
    {
        switch( opt )
        {
        case 'f':
            gpuFade = true;
            break;

        case 'g':
            gammaCorrection = atof(optarg);
            break;

        case 'l':
            doLinear = true;     // SDL linear scaling, else nearest neighbor
            break;

        case 'm':
            mikecMode = true;    // ok Mike, you wanted it
            break;

        case 'n':
            border = false;     // no border
            break;

        case 'p':
            portNum = atoi(optarg);
            break;

        case 'r':
            rendererSettingP = optarg;
            break;

        case 's':
            i = atoi(optarg);   // screen is n * n big
            if( (i >= MINSIZE) )
            {
                winSize = i;
                allowLabwcFix = false;
            }
            else
            {
                fprintf(stderr, "Window can't be made less than %d, ignored.\n", MINSIZE);
            }
            break;

        case 't':
            doTiming = true;
            break;

        case 'v':
            fprintf(stderr,"Vsync not supported for this SDL3 version, ignored.\n");
            break;

        case 'w':
            whiteBias = atoi(optarg);
            break;

        default:
            usage();
            break;
        }
    }

    if( optind < argc )
    {
        hostNameP = argv[optind];
    }

#ifndef _WIN32
    // Voluntarily lower our own scheduling priority so a co-resident PDP-1 emulator wins
    // CPU contention and keeps feeding display points. When the emulator is starved it
    // stops issuing points, the phosphor fades to black, and that reads as flicker -- the
    // failure mode seen running both the emulator and this client on one headless Pi over
    // VNC. Lowering one's OWN priority needs no privilege (unlike real-time priority), and
    // it has no effect on an uncontended system, so it is safe to leave on by default.
    // Done before the reader thread is created so that thread inherits the value.
    // niceValue 0 disables; the config file "nice=N" setting overrides DEFAULTNICE.
    if( niceValue != 0 )
    {
        if( setpriority(PRIO_PROCESS, 0, niceValue) != 0 )
        {
            fprintf(stderr, "Note: could not lower priority to nice %d; continuing at default.\n",
                niceValue);
        }
    }
#endif

    if( (pdp1FD = openPort(hostNameP, portNum)) < 0 )
    {
        fprintf(stderr, "Can't open port %d on host %s.\n", portNum, hostNameP);
        usage();
        exit(1);
    }

    // Keep SDL out of our interrrupt handling.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

    // SIGHUP will cause reloading of the configuration file, SIGTERM and SIGINT exit cleanly.
    signal(SIGHUP, sighup);
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    // init SDL
    if( !SDL_Init(SDL_INIT_VIDEO) )
    {
        fprintf(stderr,"SDL3 initialization failed, %s\n", SDL_GetError());
    }

    // If wayland/labwc is in use, it breaks any rational window enforcement of overlap with the task bar
    // or any guarantee the window title bar will be visible.
    // The actual window position on the screen can't be specified in wayland, it is ignored.
    // Labwc doesn't prevent windows overlapping the task bar.
    // Those were stupid design decisions.
    // This hack is to try to be sure the task bar and the window title bar remain visible.
    usingLabwc = (cP = getenv("XDG_CURRENT_DESKTOP")) && !strncmp(cP,"labwc", 5);
    driverNameP = SDL_GetCurrentVideoDriver();
    if( allowLabwcFix && (usingLabwc || (driverNameP && (SDL_strcmp(driverNameP, "wayland") == 0))) )
    {
        SDL_GetDisplayBounds(SDL_GetPrimaryDisplay(), &bounds);
        if( winSize > (bounds.h - WAYLANDMARGIN) )
        {
            winSize = (bounds.h - WAYLANDMARGIN);       // we only care about the vertical dimension
        }
    }

    // Create window via properties so SDL_WINDOWPOS_CENTERED can be specified.
    // X11 honors this and opens the window centered on the primary display.
    // Wayland ignores application-requested position
    // but already centers new windows by default, so this is a no-op there.
    if( !(winPropsID = SDL_CreateProperties()) )
    {
        fprintf(stderr, "Can't create window properties, %s\n", SDL_GetError());
        exit(1);
    }

    // Create the window hidden without mapping it yet.
    // SDL_CreateWindow() maps the window immediately,
    // before the renderer exists and before any clear+present has happened.
    // This results in 'window flicker' on startup as the window is set up.
    // Hiding it until the renderer has been created and we have initialized avoids this.
    SDL_SetStringProperty(winPropsID, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "T30dpy3 Type 30 Display");
    SDL_SetNumberProperty(winPropsID, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(winPropsID, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(winPropsID, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, winSize);
    SDL_SetNumberProperty(winPropsID, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, winSize);
    SDL_SetNumberProperty(winPropsID, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
        (Sint64)(SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | ((!border) ? SDL_WINDOW_BORDERLESS : 0)));

    window = SDL_CreateWindowWithProperties(winPropsID);
    SDL_DestroyProperties(winPropsID);

    if( !window )
    {
        fprintf(stderr, "Can't create window, %s\n", SDL_GetError());
        exit(1);
    }

    if( !(renderer = createRenderer()) )
    {
        exit(1);                // createRenderer() said why
    }

    // Record the actual renderer backend (e.g. "opengl", "opengles2", "software").
    // On the Pi / trixie the software fallback is the key thing to confirm when chasing fps,
    // since it makes the per-frame texture upload and scaling CPU/memory-bandwidth bound.
    rendererNameP = SDL_GetRendererName(renderer);

    // Initialize renderer and clear window.
    SDL_SetRenderLogicalPresentation(renderer, 1024, 1024, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);

    // Now make the window visible and render the black background.
    SDL_ShowWindow(window);
    SDL_RenderPresent(renderer);

    // Use RGBA8888 unconditionally.
    // SDL will convert internally if the renderer needs a different native format.
    pixelFormat = SDL_PIXELFORMAT_RGBA8888;
    formatDetailsP = SDL_GetPixelFormatDetails(pixelFormat);
    textures[0] = SDL_CreateTexture(renderer, pixelFormat, SDL_TEXTUREACCESS_STREAMING, 1024, 1024);
    textures[1] = SDL_CreateTexture(renderer, pixelFormat, SDL_TEXTUREACCESS_STREAMING, 1024, 1024);

    // Regardless of the window size, the logical size is always 1024 by 1024.
    // SDL2 was not very good at rendering pixels for some window sizes, SDL3 is much better at scaling.
    // Linear gives the best results balancing small screens vs larger ones but blurs the points some.
    // Nearest neighbor gives sharper dots, but some screen sizes don't scale well.
    // Select what works best for a given monitor and sceen size vial the command line or config file.
    //
    // Intensity is encoded directly into the rgb values themselves rather than relying on the renderer to
    // blend a separate alpha against the background.
    // This lets us use SDL_BLENDMODE_NONE, which is a straight pixel copy with no
    // per-pixel read-modify-write blend math instead of SDL_BLENDMODE_BLEND.
    SDL_SetTextureBlendMode(textures[0], SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(textures[0], (doLinear)?SDL_SCALEMODE_LINEAR:SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(textures[1], SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(textures[1], (doLinear)?SDL_SCALEMODE_LINEAR:SDL_SCALEMODE_NEAREST);

    blackPixel = SDL_MapRGBA(formatDetailsP, NULL, 0, 0, 0, 0);
     
    // Precomupute the possible rgba values over time and the intensity steps.
    initializeRgbas();
    // Set up the points array and the active list head.
    initializePoints();

    // Done before the reader thread starts, since gpuFade decides where it puts new points.
    // The CPU fade relies on the screen as target and the black draw color set above.
    if( gpuFade && !initializeGpuFade() )
    {
        fprintf(stderr, "The GPU fade is not available with the %s renderer, using the CPU fade.\n",
            (rendererNameP)?rendererNameP:"?");
        gpuFade = false;
        SDL_SetRenderTarget(renderer, NULL);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
        for( i = 0; i < FADETERMS; ++i )
        {
            if( termTextures[i] )
            {
                SDL_DestroyTexture(termTextures[i]);
                termTextures[i] = NULL;
            }
        }
    }

    // An async thread is used to read incoming data.
    // Lightpen updates are done in the main thread during the display update cycle.
    if( !(busyLockP = SDL_CreateMutex()) )
    {
        fprintf(stderr, "Can't create busy-list mutex\n");
        exit(1);
    }

    if( !(threadP = SDL_CreateThread(reader, "reader", 0)) )
    {
        fprintf(stderr, "Can't create reader thread\n");
        exit(1);
    }

    // Main loop. Check for keyboard and mouse evennts, scan the point matrix for active points,
    // update and display the screen.
    // Because of all the complex internal timing SDL3 does, it's much more reliable to use a fixed delay.
    startTime = now();          // use system time for this
    lastTime = SDL_GetTicksNS();
    accumulator = 0;

    SDL_HideCursor();            // it is enabled when the mouse moves

    while( !quit )
    {
        // Do some precision loop timekeeping so we mantian our 30fps rate.
        currentTime = SDL_GetTicksNS();
        deltaTime = currentTime - lastTime;
        accumulator += deltaTime;
        lastTime = currentTime;

        while( SDL_PollEvent(&event) )
        {
            handleEvent(&event, &evState);
        }

        if( reloadRequested )
        {
            reloadRequested = false;
            reconfigure();
        }

        if( evState.cursorTime && (((now() - evState.cursorTime) / NSECPERMSEC) > CURSORTIMEOUT) )
        {
            SDL_HideCursor();
            evState.cursorTime = 0;
        }

        // The display update is frame based.
        // All rgba values are comupted for a frame rate of 30fps.
        // Brute force delay, avoids all the SDL3 internal timing issues and is ok for this emualtion.
        // The 'accumulator' pattern is used to be sure the frame rate is correct.
        // This corrects for variations in the SDL rendering time.
        //
        // The wait is sliced into EVENTPOLLNS-sized chunks, acting on any events
        // queued during each slice, lightpen motion in particular.
        // Slicing bounds the worst case update delay to about EVENTPOLLNS.
        if( accumulator < FRAMETIME )
        {
            sleepRemaining = FRAMETIME - accumulator;
            while( (sleepRemaining > 0) && !quit )
            {
                thisSleep = (sleepRemaining < EVENTPOLLNS) ? sleepRemaining : EVENTPOLLNS;
                SDL_DelayNS(thisSleep);
                sleepRemaining -= thisSleep;

                while( SDL_PollEvent(&event) )
                {
                    handleEvent(&event, &evState);
                }
            }
        }
        else if( doTiming && (accumulator > FRAMETIME) )
        {
            frameMisses++;
            if( (accumulator - FRAMETIME)  > frameDelay )
            {
                frameDelay = accumulator - FRAMETIME;
            }
        }

        accumulator -= FRAMETIME;   // Any timing error accumulates so it can be corrected for.

        // More than a frame behind, after a stall or under a renderer that can't keep up, drop the
        // backlog as the SDL2 version does rather than render it back to back with no wait.
        // A renderer that never catches up would otherwise never wait again, and over VNC lavapipe
        // queued seconds of frames that way with no events handled.
        if( (int64_t)accumulator > FRAMETIME )
        {
            droppedNs += (accumulator - FRAMETIME);
            accumulator = FRAMETIME;
        }
        ++pacedFrames;              // counts every paced loop pass (including idle ones): the true cadence

        // With nothing active there is nothing to draw, so the expensive per-point work below is skipped.
        if( gpuFade )
        {
            renderGpuFrame();
        }
        else if( activeListHead != NOINDEX )
        {
            renderStart = now();
            textureP = textures[textureSelector];
            textureSelector ^= 1;

            if( !SDL_LockTexture(textureP, NULL, (void *)&pixels, &pitch) )
            {
                fprintf(stderr, "Can't lock texture, %s\n", SDL_GetError());
                exit(1);
            }

            // Clearing the entire pixel array seems to be faster than clearing individual points, surprising.
            memset(pixels, 0, pitch * 1024);

            // Cleared alongside the pixel buffer, tracks the brightest value written to each
            // pixel so far this frame, so drawPoint() can tell a freshly drawn point apart from
            // an existing one it overlaps and let the brighter of the two win.
            memset(brightBuffer, 0, sizeof(brightBuffer));

            // Go thru the active point list, handle each.
            // The list is threaded by index through activePool[] which
            // keeps traversal localized to a small contiguous pool instead of chasing pointers
            // across the full 1024x1024 grid.
            // This entire walk is done under a single lock acquisition, not just the individual removals.
            SDL_LockMutex(busyLockP);

            for( pointIdx = activeListHead, prevIdx = NOINDEX; pointIdx != NOINDEX; pointIdx = nextIdx )
            {
                activePointP = &activePool[pointIdx];
                x = activePointP->x;
                y = activePointP->y;
                rgba = rgbaValues[activePointP->intensity][activePointP->lifetime];

                nextIdx = activePointP->nextIdx;        // Get this now before a possible point removal.
                if( (rgba != blackPixel) && (activePointP->lifetime < MAXLIFETIME) )
                {
                    drawPoint(pixels, pitch, rgba, x, y, activePointP->intensity,
                        brightValues[activePointP->intensity][activePointP->lifetime]);
                    activePointP->lifetime++;
                    prevIdx = pointIdx;                 // Only update if we are not removing this point.
                    ++totalPoints;
                }
                else
                {
                    // We don't update prevIdx in this case because that point remains the previous point.
                    // The lock for this whole walk is already held above.
                    removeActivePoint(pointIdx, prevIdx);
                }
            }

            SDL_UnlockMutex(busyLockP);
            SDL_UnlockTexture(textureP);

            // Timing split point.
            // Everything above is CPU buffer work, everything below is presentation work.
            tAfterBuffer = now();

            // SDL_RenderClear() only earns its keep when the window's aspect ratio differs
            // from the square 1024x1024 logical area.
            // With SDL_LOGICAL_PRESENTATION_LETTERBOX the visible window can be larger than the logical area
            // and the letterbox bars outside it are part of what RenderClear repaints.
            // Without clearing in that case, those bars retain stale content
            // from previous frames, visible as phantom points outside the 1024x1024 area.
            // When the window IS square there are no bars, and SDL_RenderTexture()
            //  already overwrites every pixel of the target, so clearing is just wasted cycles.
            if( evState.isLetterboxed )
            {
                SDL_RenderClear(renderer);
            }
            SDL_RenderTexture(renderer, textureP, NULL, NULL);
            SDL_RenderPresent(renderer);
            ++totalFrames;

            if( doTiming )
            {
                renderDelta = now() - renderStart;
                renderTimeTotal += renderDelta;
                ++renderCount;
                if( renderDelta > renderTimeMax )
                {
                    renderTimeMax = renderDelta;
                }
                // Split the frame into buffer work vs present work so we can see which
                // dominates on a given backend (e.g. the software renderer over VNC).
                phaseBufferTotal += (tAfterBuffer - renderStart);
                phasePresentTotal += (renderDelta - (tAfterBuffer - renderStart));
            }
        }

    }


    if( doTiming )
    {
        reportTiming();
    }

    // Shutting the socket down wakes the reader thread's blocking SOCKREAD, which then returns 0 and
    // the thread exits. Closing it would not: on Linux a close() does not wake a read another thread
    // is already waiting in, so the reader would wait for the emulator's next word, and an idle
    // display sends none.
    // Wait for it here so we do not destroy the mutex while the reader thread might
    // still be inside SDL_LockMutex() / SDL_UnlockMutex().
    // The socket is closed only after that, so its fd number can't be reused under the reader.
    SOCKSHUTDOWN(pdp1FD);
    SDL_WaitThread(threadP, NULL);
    SOCKCLOSE(pdp1FD);

    // Do not call SDL_DestroyRenderer / SDL_DestroyWindow explicitly.
    // On X11, those calls trigger SDL's internal XTranslateCoordinates cleanup
    // which fires after X has already invalidated the window resource, producing
    // a BadWindow X error.  SDL_Quit() sequences the teardown correctly on its own.
    SDL_DestroyMutex(busyLockP);
    SDL_Quit();
    winSockCleanup();

    return(0);
}

// We are a client, try to open the display port on the pidp-1.
// Returns the fd on success, else -1.
int
openPort(char *hostNameP, int port)
{
int i;
int sockFD;
char portstr[32];
struct addrinfo hints;
struct addrinfo *resultP;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    sprintf(portstr, "%d", port);

    if( getaddrinfo(hostNameP, portstr, &hints, &resultP) )
    {
        return(-1);
    }

    if( (sockFD = (int)socket(AF_INET, SOCK_STREAM, 0)) < 0 )
    {
        freeaddrinfo(resultP);
        return(-1);         // fail
    }

    if( connect(sockFD, resultP->ai_addr, resultP->ai_addrlen) < 0 )
    {
        SOCKCLOSE(sockFD);
        freeaddrinfo(resultP);
        return(-1);         // fail
    }

    freeaddrinfo(resultP);

    // We want mouse events to go out quickly
    i = 1;
    // Cast to (const char *): Winsock's setsockopt() declares optval as
    // const char *, while POSIX declares it as const void * (which a
    // char * also satisfies), so this cast is portable to both.
    setsockopt(sockFD, IPPROTO_TCP, TCP_NODELAY, (const char *)&i, sizeof(i));
    return(sockFD);
}

// Open the window's renderer, the one the renderer setting or -r names if
// SDL_RENDER_DRIVER is set, else the first of opengl, opengles2 and software that opens.
// SDL tries vulkan and gpu before software, and when OpenGL can't open,
// those can land on lavapipe, far too slow for this.
// Interestingly, SDL's software renderer with the CPU fade keeps up.
// Returns NULL, with the reason on stderr, if none opens.
SDL_Renderer *
createRenderer(void)
{
SDL_Renderer *rendererP;
#ifndef _WIN32
static const char *orderP[] = { "opengl", "opengles2", "software" };
size_t i;
#endif

    if( rendererSettingP && strcmp(rendererSettingP, "auto") )
    {
        if( !(rendererP = SDL_CreateRenderer(window, rendererSettingP)) )
        {
            fprintf(stderr, "Can't create the %s renderer, %s\n", rendererSettingP, SDL_GetError());
        }
        return(rendererP);
    }

#ifndef _WIN32
    if( !SDL_GetHint(SDL_HINT_RENDER_DRIVER) )
    {
        for( i = 0; i < (sizeof(orderP) / sizeof(orderP[0])); ++i )
        {
            if( (rendererP = SDL_CreateRenderer(window, orderP[i])) )
            {
                return(rendererP);
            }
        }
    }
#endif

    // SDL's choice: on Windows, where Direct3D comes first; when SDL_RENDER_DRIVER is set;
    // and as the last resort when none of the above opens.
    if( !(rendererP = SDL_CreateRenderer(window, NULL)) )
    {
        fprintf(stderr, "Can't create renderer, %s\n", SDL_GetError());
    }
    return(rendererP);
}

// Reader thread to fetch data from server.
// See if there is data to read.
// If so, process it and add to the display list.
int
reader(void *argP)
{
int i;
int count;
int delay;
uint8_t intensity;
int x, y;
uint32_t cmd;
uint32_t pointIdx;
uint32_t buffer[READBUFSIZE];

static bool skipOne = false;

    while( !quit )
    {
        if( (count = SOCKREAD(pdp1FD, buffer, sizeof(buffer))) <= 0 )
        {
            quit = true;
            return(0);
        }

        count /= sizeof(uint32_t);          // command words

        SDL_LockMutex(busyLockP);           // since we are reading muliples, lock for the duration

        for( i = 0; i < count; i++ )
        {
            if( skipOne )                   // for dropping extended delay commands
            {
                skipOne = false;
                continue;
            }

            cmd = buffer[i];
            // Commands from the pidp-1 have a delay time, shouldn't be coming from it, we handle it, ignore.
            delay = cmd >> 23;

            if( delay == 511 )
            {
                skipOne = true;             // some delays take 2 command words, we need to skip the next word
                continue;
            }

            ++receivedPoints;

            // The command word encodes the intensity and position of the point to display.
            // This is the standard format pidp-1 uses, same as the simh format.
            // THe x and y coordinates have the origin at the lower left corner, not the Type 30
            // center of the screen.
            x = cmd & 01777;
            y = 1023 - ((cmd >> 10) & 01777);   // SDL y 0 is top of screen, not bottom, flop.
            intensity = (cmd >> 20) & 7;        // The standard display intensity, 0-7.

            // The GPU fade keeps no active points, only this frame's new ones.
            if( gpuFade )
            {
                if( newCount < MAXNEWPOINTS )
                {
                    newPoints[newFill][newCount++] = PACKPOINT(x, y, intensity);
                }
                else
                {
                    ++droppedPoints;
                }
                continue;
            }

            // It is possible that this point is already active.
            // If so, resetting the lifetime and intensity in its existing pool slot is all
            // that is needed - the active list linkage doesn't change.
            // If not, a new slot must be pulled from the free list and linked in.
            pointIdx = pointIndex[y][x];

            if( pointIdx == NOINDEX )            // This is not a currently active point, add to active list
            {
                addActivePoint((uint16_t)x, (uint16_t)y, intensity);
            }
            else
            {
                activePool[pointIdx].intensity = intensity;
                activePool[pointIdx].lifetime = 0;
            }
        }

        SDL_UnlockMutex(busyLockP);
    }

    return(0);
}

// Precompute the rgba values used for displaying points.
// There are only 256 possible rgba values for each pdp-1 intensity,
// one per each lifetime value which ranges from 0-255.
void
initializeRgbas()
{
int i, j;
int intensity;
int delta;
Uint8 r, g, b, a, bias;
Uint8 blueAlpha, yellowAlpha;
Rgba rgba;

    // Limit to the SDL3 range of 0-255
    if( (whiteBias + BLUER) > 255 )
    {
        whiteBias = 255 - BLUER;
    }

    if( (whiteBias + BLUEG) > 255 )
    {
        whiteBias = 255 - BLUEG;
    }

    // The internal 0-255 intensity is in 7 linear increments after the first, intensity 0 is the base value.
    delta = (255 - MININTENSITY) / 7;

    // The blue phosphor had a 25-75 microsecond lifetime to 10% brightness.
    // That is far less than the frame rate and wouldn't be visible, so it is strecthed,
    // but the intensity falloff is still a power-law relationship.
    // The yellow phosphor had a 400 milisecond lifetime to 10% brightness.
    // At the set 30fps, the decay alpha gives an accurate decay time.
    // This data from the RCA Phosphors TPM-1508A technical note.
    // Fadout is done by adjusting the blue and yellow alphas based on lifetime.
    // The initial value, which corresponds to the first displayed frame,
    // has the blue color adjusted by the white bias, see below.
    for( i = 7; i >= 0; --i )         // for each pdp1 intensity level
    {
        intensity = MININTENSITY + (i * delta);

        // The blending algorithm will take 100% of the source (blue) value if its alpha is 100% and
        // none of the second (yellow), so we have to adjust the initial rgba color a bit.
        // The blue shouldn't dominate, the real phosphor is only slightly blue when the point
        // is drawn because of the yellow secondary phosphor blending with it.
        // The white bias is added to the r and g values of the blue phospor to make it whiter, but only
        // for the first value. This compensates for the yellow being suppressed.
        // However, if a strking visual effect is desired, a low or zero white bias will give
        // a bright blue-purple inital dot.
        // The alpha for both colors always starts at 255, 100%, but then drops by the power law for it,
        // which is why the bias is only needed for the first value.
        bias = whiteBias;

        for( j = 0; j < 256; ++j )    // for each lifetime value, 0 being initial, 255 being final
        {
            blueAlpha = (Uint8)(powf((float)j+1, -BLUEDECAYALPHA) * 255.0f);
            yellowAlpha = (Uint8)(powf((float)j+1, -YELLOWDECAYALPHA) * 255.0f);

            // Yellow has a looong tailoff, so stop when it gets down to lowCutoff.
            // The blended alpha needs to be gamma adjusted.
            if( yellowAlpha > lowCutoff )
            {
                rgba = blend(BLUER + bias, BLUEG + bias, BLUEB, blueAlpha, YELLOWRGB, yellowAlpha);
                SDL_GetRGBA(rgba, formatDetailsP, NULL, &r, &g, &b, &a);
                a = APPLYGAMMA(((int)a * intensity) / 255, gammaCorrection);

                // Premultiply the alpha into r, g, and b here, once, at table-build time,
                // and store the result with alpha forced to fully opaque (255).
                // This lets the display loop use SDL_BLENDMODE_NONE (a plain pixel copy)
                // instead of SDL_BLENDMODE_BLEND (a per-pixel read-modify-write blend
                // against the background), since the brightness scaling that alpha used
                // to provide via blending is now baked directly into r, g, and b.
                r = (Uint8)(((int)r * (int)a) / 255);
                g = (Uint8)(((int)g * (int)a) / 255);
                b = (Uint8)(((int)b * (int)a) / 255);
                rgba = SDL_MapRGBA(formatDetailsP, NULL, r, g, b, 255);
                bias = 0;           // only the first value

                brightValues[i][j] = a;    // pre-premultiply alpha, see brightValues declaration
            }
            else
            {
                rgba = blackPixel;
                brightValues[i][j] = 0;
            }

            rgbaValues[i][j] = rgba;
        }
    }
}

// Initialize the screen-position lookup table, the active-point pool, and the
// free/active list heads.
// By convention, the first subscript is y, the second x.
// This matches the SDL convention.
// The active list is initialized to empty, and the free list is initialized to
// thread through every slot in activePool[] in order, 0..MAXACTIVEPOINTS-1.
void
initializePoints()
{
int32_t x, y;
uint32_t i;

    // No screen position is active yet.
    for( y = 0; y < LOGICALSIZE; ++y )
    {
        for( x = 0; x < LOGICALSIZE; ++x )
        {
            pointIndex[y][x] = NOINDEX;
        }
    }

    // Thread the free list through the whole pool: each slot's nextIdx points to
    // the following slot, and the last slot terminates the list with NOINDEX.
    for( i = 0; i < (MAXACTIVEPOINTS - 1); ++i )
    {
        activePool[i].nextIdx = i + 1;
    }
    activePool[MAXACTIVEPOINTS - 1].nextIdx = NOINDEX;

    // Pre-fault brightBuffer[][] here at startup instead of letting the first rendered
    // frame's memset() in main() take the minor page faults for its ~256 4K pages. This
    // keeps that fault cost off the first frame that actually has active points to draw,
    // where it would otherwise show up as unexplained extra latency on that one frame.
    memset(brightBuffer, 0, sizeof(brightBuffer));

    freeListHead = 0;
    activeListHead = NOINDEX;
    droppedPoints = 0;
}

// Remove a point from the active list, adjusting the head and prior point's link if needed,
// and return its pool slot to the head of the free list for reuse.
// PointIdx is the pool index of the point being removed; prevIdx is the pool index of the
// previous entry on the active list, or NOINDEX if pointIdx was the head.
// The lock must be in place before calling! (main()'s point-removal walk holds busyLockP
// for the entire pass before calling this, same convention as addActivePoint() below).
void
removeActivePoint(uint32_t pointIdx, uint32_t prevIdx)
{
    // This screen position no longer has an active point.
    pointIndex[activePool[pointIdx].y][activePool[pointIdx].x] = NOINDEX;

    if( prevIdx != NOINDEX )
    {
        activePool[prevIdx].nextIdx = activePool[pointIdx].nextIdx;
    }
    else
    {
        activeListHead = activePool[pointIdx].nextIdx;     // This point was the head, reset.
    }

    // Return this slot to the head of the free list so it can be reused.
    activePool[pointIdx].nextIdx = freeListHead;
    freeListHead = pointIdx;

    if( doTiming )
    {
        atomic_fetch_sub_explicit(&activePoints, 1, memory_order_relaxed);
    }
}

// Add a new point at screen position (x,y) with the given intensity to the active list,
// pulling a free slot from the pool and linking it in as the new head.
// The lock must be in place before calling! (the reader thread holds busyLock for the
// duration of a read batch, which covers this call).
// If the pool is exhausted (every slot already active - effectively every screen position
// lit at once, far beyond any realistic load), the new point is silently dropped and
// droppedPoints is incremented so this condition is observable via timing output.
void
addActivePoint(uint16_t x, uint16_t y, byte intensity)
{
uint32_t newIdx;
uint64_t u64tmp;

    if( freeListHead == NOINDEX )
    {
        ++droppedPoints;       // pool exhausted, can't track this point - drop it
        return;
    }

    // Pull a slot from the head of the free list.
    newIdx = freeListHead;
    freeListHead = activePool[newIdx].nextIdx;

    // Fill in the new active entry.
    activePool[newIdx].x = x;
    activePool[newIdx].y = y;
    activePool[newIdx].intensity = intensity;
    activePool[newIdx].lifetime = 0;

    // Link it in as the new head of the active list.
    activePool[newIdx].nextIdx = activeListHead;
    activeListHead = newIdx;

    // Record where to find this point's pool entry given its screen position.
    pointIndex[y][x] = newIdx;

    if( doTiming )
    {
        // A lot of hoop-jumping for just a statistical value, but it will the the correct value.
        u64tmp = atomic_fetch_add_explicit(&activePoints, 1, memory_order_relaxed) + 1;
        if( u64tmp  > maxActivePoints )
        {
            maxActivePoints = u64tmp;
        }
    }
}

// Blend 2 rgba values into a packed renderer-preferred result.
// This is the standard blend algorithm, same as SDL_BLEMDNOME_BLEND.
// We use this because we don't want to have to have SDL do the blending at display time.
// Note that a source alpha of 100%, 255, is opaque and will totally mask the destination alpha.
// For our purposes, blue is always the source and yellow the destination.
uint32_t
blend(int srcR, int srcG, int srcB, int sAlpha, int destR, int destG, int destB, int dAlpha)
{
int rR, rG, rB, rA;
float srcAlpha, destAlpha,newAlpha;

    srcAlpha = (float)sAlpha / 255.0;
    destAlpha = (float)dAlpha / 255.0;

    rR = (int)((srcR * srcAlpha) + (destR * (1.0f - srcAlpha)));
    rG = (int)((srcG * srcAlpha) + (destG * (1.0f - srcAlpha)));
    rB = (int)((srcB * srcAlpha) + (destB * (1.0f - srcAlpha)));
    newAlpha = srcAlpha + (destAlpha * (1.0f - srcAlpha));
    rA = (int)(newAlpha * 255.0f);

    if( rA > 255 )
    {
        rA = 255;               // keep in bounds
    }

    return( SDL_MapRGBA(formatDetailsP, NULL, rR, rG, rB, rA) );
}

// Spread a point to simulate beam spread on a crt.
// The Type 30 doccumentation states a spot diameter of 0.030" max, about 3 pixels on its display.
// This siumulates it well by drawing extra dots based on the 0-7 intensity level.
// The actual pattern is thanks to Claude/Sonnet5,credit where credit is due.
// Write rgba to *pixP, but only if bright is at least as bright as the brightness already
// recorded at *brightP for this frame; otherwise leave both alone.
void
drawPoint(uint8_t *pixels, int pitch, uint32_t rgba, int x, int y, int intensity, int bright)
{
uint32_t *rowP;          // pointer to (x, y) in the current row
uint32_t *aboveRowP;     // pointer to (x, y-1), only valid if notTopEdge
uint32_t *belowRowP;     // pointer to (x, y+1), only valid if notBotEdge
uint8_t *brightRowP;     // pointer to brightBuffer[y][x]
uint8_t *brightAboveP;   // pointer to brightBuffer[y-1][x], only valid if notTopEdge
uint8_t *brightBelowP;   // pointer to brightBuffer[y+1][x], only valid if notBotEdge
int stride;              // pixels per row, derived from pitch (which is in bytes)

    if( mikecMode)
    {
        *XYTOPTR(pixels, pitch, x, y) = rgba;
        return;
    }

    // Claude suggestion to optimize compiler and cpu register use.
    const bool notLeftEdge = (x > 0);
    const bool notRightEdge = (x < 1023);
    const bool notTopEdge = (y > 0);
    const bool notBotEdge = (y < 1023);

    // Compute the row base pointers once instead of repeating the full
    // XYTOPTR address arithmetic for every one of the up to 9 pixels touched.
    stride = pitch / (int)sizeof(uint32_t);
    rowP = (uint32_t *)(pixels + ((size_t)y * (size_t)pitch)) + x;
    aboveRowP = rowP - stride;
    belowRowP = rowP + stride;

    // Same idea as rowP/aboveRowP/belowRowP above, but into brightBuffer[][] (a plain
    // LOGICALSIZE-wide 2D array, so the row stride is just LOGICALSIZE, not pitch-derived).
    brightRowP = &brightBuffer[y][x];
    brightAboveP = brightRowP - LOGICALSIZE;
    brightBelowP = brightRowP + LOGICALSIZE;

    switch (intensity)
    {
    // Max intensity adds the sharp corners to complete the 3x3 square block
    case 7:
    case 6:
        if( notLeftEdge & notTopEdge )
        {
            DRAWIFBRIGHTER(aboveRowP - 1, brightAboveP - 1);
        }
        if( notLeftEdge & notBotEdge )
        {
            DRAWIFBRIGHTER(belowRowP - 1, brightBelowP - 1);
        }
        if( notRightEdge & notTopEdge )
        {
            DRAWIFBRIGHTER(aboveRowP + 1, brightAboveP + 1);
        }
        if( notRightEdge & notBotEdge )
        {
            DRAWIFBRIGHTER(belowRowP + 1, brightBelowP + 1);
        }
    // Medium intensity adds the left/right arms to form a wide cross
    case 5:
    case 4:
        if( notLeftEdge )
        {
            DRAWIFBRIGHTER(rowP - 1, brightRowP - 1);
        }
        if( notRightEdge )
        {
            DRAWIFBRIGHTER(rowP + 1, brightRowP + 1);
        }

    // Lowest intensities always draw the tight vertical core
    case 3:
    case 2:
    case 1:
    case 0:
        DRAWIFBRIGHTER(rowP, brightRowP);           // Center core
        if( notTopEdge )
        {
            DRAWIFBRIGHTER(aboveRowP, brightAboveP);    // Top arm
        }
        if( notBotEdge )
        {
            DRAWIFBRIGHTER(belowRowP, brightBelowP);    // Bottom arm
        }
        break;
    }
}

// Choose fadeTable's row for fadeTail, to the nearest 0.1 s within the table, and set up the
// terms' colors and the intensity levels from it. The targets restart from black.
void
selectFadeTerms(void)
{
const int yellowRgb[3] = { YELLOWRGB };
const int blueRgb[3] = { BLUER, BLUEG, BLUEB };
int row;
int term;
int i;
int delta;
float w;

    row = (int)((fadeTail * 10.0f) + 0.5f) - FADETABLEFIRST;
    if( row < 0 )
    {
        row = 0;
    }
    else if( row >= FADETABLEROWS )
    {
        row = (FADETABLEROWS - 1);
    }
    fadeTermsP = &fadeTable[row];

    // The same intensity steps initializeRgbas() uses, with the gamma applied to each.
    delta = ((255 - MININTENSITY) / 7);
    for( i = 0; i < 8; ++i )
    {
        levelValues[i] = powf(((float)(MININTENSITY + (i * delta)) / 255.0f), gammaCorrection);
    }

    // The fast term is drawn in its own color, its share of the yellow-green plus the blue, which
    // fades with it; it goes to the screen as it is. The other terms are drawn gray and get the
    // yellow-green, at their weights, as they are added to the screen. In a new point's first
    // frame the sum is a near-white, as the two phosphors' light adds.
    w = fadeTermsP->w[FASTTERM];
    for( i = 0; i < 3; ++i )
    {
        fastColor[i] = (((w * (float)yellowRgb[i]) + (float)blueRgb[i]) / 255.0f);
        if( fastColor[i] > 1.0f )
        {
            fastColor[i] = 1.0f;
        }
    }

    for( term = 0; term < FADETERMS; ++term )
    {
        w = fadeTermsP->w[term];
        if( term == FASTTERM )
        {
            SDL_SetTextureColorModFloat(termTextures[term], 1.0f, 1.0f, 1.0f);
        }
        else
        {
            SDL_SetTextureColorModFloat(termTextures[term], ((w * (float)yellowRgb[0]) / 255.0f),
                ((w * (float)yellowRgb[1]) / 255.0f), ((w * (float)yellowRgb[2]) / 255.0f));
        }
        SDL_SetTextureScaleMode(termTextures[term], (doLinear)?SDL_SCALEMODE_LINEAR:SDL_SCALEMODE_NEAREST);

        SDL_SetRenderTarget(renderer, termTextures[term]);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
    }

    SDL_SetRenderTarget(renderer, NULL);
    gpuIdleFrames = (fadeTermsP->life + 1);     // nothing to fade until a point arrives
}

// Set up the GPU fade: its blend modes, one target per term, and a check that the renderer
// fades and draws exactly as fadeModel() says, since the fit assumes it.
// Returns false, with the reason on stderr, if the renderer cannot do it.
bool
initializeGpuFade(void)
{
int term;
int j;
int v;
int got;

    subtractMode = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_REV_SUBTRACT, SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_REV_SUBTRACT);
    maxMode = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_MAXIMUM, SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_MAXIMUM);

    for( term = 0; term < FADETERMS; ++term )
    {
        if( !(termTextures[term] = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_TARGET, LOGICALSIZE, LOGICALSIZE)) )
        {
            fprintf(stderr, "GPU fade: can't create a render target, %s\n", SDL_GetError());
            return(false);
        }
        SDL_SetTextureBlendMode(termTextures[term], SDL_BLENDMODE_ADD);
    }

    selectFadeTerms();

    // Each term in use, from 255 until it reads 0, must match the model frame by frame.
    for( term = 0; term < FADETERMS; ++term )
    {
        if( fadeTermsP->w[term] < MINWEIGHT )
        {
            continue;
        }

        SDL_SetRenderTarget(renderer, termTextures[term]);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderClear(renderer);
        for( j = 0, v = 255; j <= fadeTermsP->life; ++j )
        {
            if( !readTermPixel(&got) )
            {
                fprintf(stderr, "GPU fade: can't read a render target back, %s\n", SDL_GetError());
                return(false);
            }

            if( got != v )
            {
                fprintf(stderr, "GPU fade: term %d reads %d after %d frames, the model says %d.\n",
                    term, got, j, v);
                return(false);
            }

            if( v == 0 )
            {
                break;
            }

            if( !fadeTerm(term) )
            {
                fprintf(stderr, "GPU fade: blend mode refused, %s\n", SDL_GetError());
                return(false);
            }
            v = fadeModel(v, fadeTermsP->n[term], fadeTermsP->s[term]);
        }

        // idle detection in renderGpuFrame() relies on every term being black by then
        if( v != 0 )
        {
            fprintf(stderr, "GPU fade: term %d is still %d after %d frames.\n", term, v, fadeTermsP->life);
            return(false);
        }
    }

    // A point drawn with maxMode lands on its own pixel and keeps the brighter value there:
    // 128 over 64 reads 128, and 32 over that still reads 128.
    SDL_SetRenderTarget(renderer, termTextures[FASTTERM]);
    SDL_SetRenderDrawColor(renderer, 64, 64, 64, 255);
    SDL_RenderClear(renderer);
    if( !SDL_SetRenderDrawBlendMode(renderer, maxMode) )
    {
        fprintf(stderr, "GPU fade: blend mode refused, %s\n", SDL_GetError());
        return(false);
    }
    SDL_SetRenderDrawColor(renderer, 128, 128, 128, 255);
    SDL_RenderPoint(renderer, 0.0f, 0.0f);
    SDL_SetRenderDrawColor(renderer, 32, 32, 32, 255);
    SDL_RenderPoint(renderer, 0.0f, 0.0f);
    if( !readTermPixel(&got) || (got != 128) )
    {
        fprintf(stderr, "GPU fade: a point drawn at 128 then 32 over 64 reads %d, not 128.\n", got);
        return(false);
    }

    selectFadeTerms();          // back to black
    return(true);
}

// One frame of a term's fade, on the current target: v becomes round(v * n / 255) - s, at least 0.
// Returns false if the renderer refuses a blend mode.
bool
fadeTerm(int term)
{
float k;
float s;

    k = ((float)fadeTermsP->n[term] / 255.0f);
    if( !SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_MOD) )
    {
        return(false);
    }
    SDL_SetRenderDrawColorFloat(renderer, k, k, k, 1.0f);
    SDL_RenderFillRect(renderer, NULL);

    if( fadeTermsP->s[term] > 0 )
    {
        s = ((float)fadeTermsP->s[term] / 255.0f);
        if( !SDL_SetRenderDrawBlendMode(renderer, subtractMode) )
        {
            return(false);
        }
        SDL_SetRenderDrawColorFloat(renderer, s, s, s, 1.0f);
        SDL_RenderFillRect(renderer, NULL);
    }

    return(true);
}

// What fadeTerm() does to one 8-bit value, v (0-255), with factor n/255 and subtraction s/255.
int
fadeModel(int v, int n, int s)
{
    v = ((((2 * v * n) + 255) / 510) - s);     // round(v * n / 255), with no ties to break
    return( (v < 0) ? 0 : v );
}

// Read the red value (0-255) of pixel (0, 0) of the current render target into *valueP.
// Returns false if the renderer can't read it back.
bool
readTermPixel(int *valueP)
{
SDL_Rect rect;
SDL_Surface *surfaceP;
Uint8 r, g, b, a;
bool ok;

    rect.x = 0;
    rect.y = 0;
    rect.w = 1;
    rect.h = 1;
    if( !(surfaceP = SDL_RenderReadPixels(renderer, &rect)) )
    {
        *valueP = -1;
        return(false);
    }

    ok = SDL_ReadSurfacePixel(surfaceP, 0, 0, &r, &g, &b, &a);
    SDL_DestroySurface(surfaceP);
    *valueP = (ok) ? (int)r : -1;
    return(ok);
}

// Write to outP the pixels a point at (x, y) covers at intensity, the pattern drawPoint() uses.
// Returns how many were written, at most MAXSPREAD.
int
spreadPoint(SDL_FPoint *outP, int x, int y, int intensity)
{
static const int dx[MAXSPREAD] = { 0, 0, 0, -1, 1, -1, -1, 1, 1 };
static const int dy[MAXSPREAD] = { 0, -1, 1, 0, 0, -1, 1, -1, 1 };
int total;
int count;
int i;
int px, py;

    // Center and the vertical arms, then the horizontal arms, then the corners.
    total = (mikecMode) ? 1 : ((intensity >= 6) ? 9 : ((intensity >= 4) ? 5 : 3));
    count = 0;
    for( i = 0; i < total; ++i )
    {
        px = (x + dx[i]);
        py = (y + dy[i]);
        if( (px >= 0) && (px < LOGICALSIZE) && (py >= 0) && (py < LOGICALSIZE) )
        {
            outP[count].x = (float)px;
            outP[count].y = (float)py;
            ++count;
        }
    }

    return(count);
}

// One frame of the GPU fade: take the reader's new points, fade every term and draw the new
// points into it, then add the terms onto the screen. Nothing is drawn once all has faded out.
void
renderGpuFrame(void)
{
uint32_t *pointsP;
uint32_t count;
uint32_t i;
uint32_t packed;
int term;
int level;
int perLevel[8];
int start[8];
int used[8];
float lv;
uint64_t renderStart;
uint64_t tAfterBuffer;
uint64_t renderDelta;

    // Swap buffers with the reader, so it fills the other while this frame's are drawn.
    SDL_LockMutex(busyLockP);
    pointsP = newPoints[newFill];
    count = newCount;
    newFill ^= 1;
    newCount = 0;
    SDL_UnlockMutex(busyLockP);

    // A point fades to black in fadeTermsP->life frames; after that the screen is black.
    if( count > 0 )
    {
        gpuIdleFrames = 0;
    }
    else if( gpuIdleFrames > fadeTermsP->life )
    {
        return;
    }
    else
    {
        ++gpuIdleFrames;
    }

    renderStart = now();

    // Spread each point to its pixels, grouped by intensity so each level is one draw per term.
    for( level = 0; level < 8; ++level )
    {
        perLevel[level] = 0;
        used[level] = 0;
    }
    for( i = 0; i < count; ++i )
    {
        ++perLevel[(pointsP[i] >> 20) & 7];
    }
    start[0] = 0;
    for( level = 1; level < 8; ++level )
    {
        start[level] = (start[level - 1] + (perLevel[level - 1] * MAXSPREAD));
    }
    for( i = 0; i < count; ++i )
    {
        packed = pointsP[i];
        level = ((packed >> 20) & 7);
        used[level] += spreadPoint(&spreadPoints[start[level] + used[level]],
            (int)(packed & 01777), (int)((packed >> 10) & 01777), level);
    }
    totalPoints += count;

    for( term = 0; term < FADETERMS; ++term )
    {
        if( fadeTermsP->w[term] < MINWEIGHT )
        {
            continue;
        }

        SDL_SetRenderTarget(renderer, termTextures[term]);
        fadeTerm(term);
        if( count > 0 )
        {
            SDL_SetRenderDrawBlendMode(renderer, maxMode);
            for( level = 0; level < 8; ++level )
            {
                if( used[level] == 0 )
                {
                    continue;
                }

                lv = levelValues[level];
                if( term == FASTTERM )
                {
                    SDL_SetRenderDrawColorFloat(renderer, (lv * fastColor[0]), (lv * fastColor[1]),
                        (lv * fastColor[2]), 1.0f);
                }
                else
                {
                    SDL_SetRenderDrawColorFloat(renderer, lv, lv, lv, 1.0f);
                }
                SDL_RenderPoints(renderer, &spreadPoints[start[level]], used[level]);
            }
        }
    }

    SDL_SetRenderTarget(renderer, NULL);
    tAfterBuffer = now();

    // The terms add, so the screen is cleared first every frame, letterboxed or not.
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    for( term = 0; term < FADETERMS; ++term )
    {
        if( fadeTermsP->w[term] >= MINWEIGHT )
        {
            SDL_RenderTexture(renderer, termTextures[term], NULL, NULL);
        }
    }
    SDL_RenderPresent(renderer);
    ++totalFrames;

    if( doTiming )
    {
        renderDelta = (now() - renderStart);
        renderTimeTotal += renderDelta;
        ++renderCount;
        if( renderDelta > renderTimeMax )
        {
            renderTimeMax = renderDelta;
        }
        phaseBufferTotal += (tAfterBuffer - renderStart);
        phasePresentTotal += (renderDelta - (tAfterBuffer - renderStart));
    }
}

// Handles one SDL event during the main loop.
// It is called both from the once-per-frame loop and short slices of the frame-wait.
// This allows lightpen motion to be queued much faster, needed for the new predicive lightpen code.
static void
handleEvent(SDL_Event *eventP, EventStateP stateP)
{
    switch(eventP->type)
    {
    case SDL_EVENT_QUIT:
        quit = true;
        break;

    // Fires for both user-driven edge-dragging resizes and SDL_SetWindowFullscreen() coordinates.
    // Recompute whether the window's aspect ratio still matches the square 1024x1024 logical area.
    // If it does, SDL_LOGICAL_PRESENTATION_LETTERBOX adds no bars and the per-frame SDL_RenderClear()
    // below can be skipped.
    case SDL_EVENT_WINDOW_RESIZED:
        stateP->isLetterboxed = (eventP->window.data1 != eventP->window.data2);
        break;

    case SDL_EVENT_KEY_DOWN:
        switch( eventP->key.scancode )
        {
        case SDL_SCANCODE_F11:
        case SDL_SCANCODE_F:
            stateP->fullscreen = !stateP->fullscreen;
            SDL_SetWindowFullscreen(window, (stateP->fullscreen)?SDL_WINDOW_FULLSCREEN:0);
            break;

        case SDL_SCANCODE_ESCAPE:
            quit = true;
            break;

        case SDL_SCANCODE_B:
            border = !border;
            SDL_SetWindowBordered(window, (border)?true:false);
            break;
        }
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if( eventP->button.button == SDL_BUTTON_LEFT )
        {
            stateP->penDown = true;
            SDL_ConvertEventToRenderCoordinates(renderer, eventP);
            stateP->penx = CONSTRAIN((int)eventP->button.x);
            stateP->peny = CONSTRAIN((int)eventP->button.y);
            updatePen(pdp1FD, true, stateP->penx, stateP->peny);
            SDL_ShowCursor();
            stateP->cursorTime = now();
        }
        else if( eventP->button.button == SDL_BUTTON_RIGHT )
        {
            // Snapshot screen-absolute cursor position and window origin at
            // drag start.
            stateP->dragging = true;
            SDL_GetGlobalMouseState(&stateP->fMouseGlobalX, &stateP->fMouseGlobalY);
            stateP->dragStartGlobalX = (int)stateP->fMouseGlobalX;
            stateP->dragStartGlobalY = (int)stateP->fMouseGlobalY;
            SDL_GetWindowPosition(window, &stateP->dragWinOriginX, &stateP->dragWinOriginY);
        }
        break;

    case SDL_EVENT_MOUSE_MOTION:
        stateP->cursorTime = now();
        if( stateP->dragging )
        {
            // Screen-absolute cursor position, independent of window position
            // and of SDL3's logical presentation scaling.
            // SDL computes position from window-relative coords, so after SDL_SetWindowPosition
            // shifts the window they are corrupted by the negative of the window's movement.
            SDL_GetGlobalMouseState(&stateP->fMouseGlobalX, &stateP->fMouseGlobalY);
            SDL_SetWindowPosition(window,
                (stateP->dragWinOriginX + ((int)stateP->fMouseGlobalX - stateP->dragStartGlobalX)),
                (stateP->dragWinOriginY + ((int)stateP->fMouseGlobalY - stateP->dragStartGlobalY)));
        }
        if( stateP->penDown )
        {
            SDL_ConvertEventToRenderCoordinates(renderer, eventP);
            stateP->penx = CONSTRAIN((int)eventP->motion.x);
            stateP->peny = CONSTRAIN((int)eventP->motion.y);
            updatePen(pdp1FD, true, stateP->penx, stateP->peny);
        }
        break;

    case SDL_EVENT_MOUSE_BUTTON_UP:
        if( eventP->button.button == SDL_BUTTON_LEFT )
        {
            stateP->penDown = false;
            updatePen(pdp1FD, false, 0, 0);
        }
        else if( eventP->button.button == SDL_BUTTON_RIGHT )
        {
            stateP->dragging = false;
        }
        break;
    }
}

// For the real hardware, the Type 30 would figure out if there was a hit
// at the last drawn pixel when issuing the completion pulse,
// but that's not possible here, let it be determined back in the pdp1 code.
// SDL3 handles the mouse coordinate transforms for scaled windows itself now, unlike all the hoops SDL2 forced.
void
updatePen(int sockFD, bool penDown, int mouseX, int mouseY)
{
uint32_t cmd;

    if( penDown )
    {
        // SDL has the upper left corner x,y as 0,0, ranging from 0 to 1023.
        // PDP1 is -511,511, ranging from -511 to 511 plus the PDP1 coords are 1's complement.
        // This really should have used the same 0-1023 range the display coordinates use,
        // but too many things depend upon it being center-origined.
        mouseX -= 511;
        if( mouseX < 0 )
        {
            --mouseX;             // 1's cmpl conversion
        }

        mouseY = 511 - mouseY;
        if( mouseY < 0 )
        {
            --mouseY;             // 1's cmpl conversion
        }

        cmd = CMDBITS;
        cmd |= (mouseX & 0x3FF) << 10;
        cmd |= (mouseY & 0x3FF);
    }
    else
    {
        cmd = LPCMD | LPUP;  // pen up cmd to host
    }

    // And send to host.
    SOCKWRITE(sockFD, &cmd, 4);
}

// Determine if a string represents a true or false value in the config file.
// If the string starts with 'y', 't', or has a numeric value of 1, it is true, else false.
// Not declared at the top of the file, only used below.
bool
isTrue(char *strP)
{
    if( (*strP == 'y') || (*strP == 't') )
    {
        return(true);
    }

    if( isdigit(*strP) && (atoi(strP) == 1) )
    {
        return(true);
    }

    return(false);
}

// See if there is a config file in the user home directory.
// If not, try the install directory.
// The file is named '.t30dyconfig' in the home directory,
// 't30dpyconfig' in the install directory.
// Lines are of the form 'param=value', empty lines or lines beginning with '#' are ignored,
// as are any invalid params.
// For booleans, isTrue(), above, checks for valid true words.
// A full load is only done at startup, sighup calls with false, some settings can't be dynamically changed.
void
loadConfig(bool full)
{
int i;
char *cP, *cP2;
FILE *fP;
char line[256];

    if( !(fP = getFile("~/.t30dpy.config")) )
    {
        if( !(fP = getFile("/opt/pidp1-mods/t30dpy.config")) )
        {
            return;         // no config file
        }
    }

    while( fgets(line, sizeof(line), fP) )
    {
        if( (line[0] == '\n') || (line[0] == '#') )
        {
            continue;
        }

        line[strlen(line) - 1] = '\0';  // drop the annoying newline
        if( (cP = strchr(line, '=')) )
        {
            *cP++ = '\0';

            // ignore embedded spaces
            while( isspace(*cP) )
            {
                ++cP;
            }

            if( (cP2 = strchr(line, ' ')) )
            {
                *cP2 = '\0';
            }

            if( full )
            {
                if( !strcmp(line, "host") )
                {
                    hostNameP = (char *)malloc(strlen(cP) + 1);
                    strcpy(hostNameP, cP);
                }
                else if( !strcmp(line, "port") )
                {
                    portNum = atoi(cP);
                }
                else if( !strcmp(line, "size") )
                {
                    i = atoi(cP);      // screen is n * n big
                    if( i >= MINSIZE )
                    {
                        winSize = i;
                        allowLabwcFix = false;
                    }
                }
                else if( !strcmp(line, "gpufade") )
                {
                    gpuFade = isTrue(cP);
                }
                else if( !strcmp(line, "renderer") )
                {
                    // Read at startup only, a reload doesn't recreate the renderer.
                    rendererSettingP = (char *)malloc(strlen(cP) + 1);
                    strcpy(rendererSettingP, cP);
                }
            }

            if( !strcmp(line, "nice") )
            {
                niceValue = atoi(cP);
            }
            else if( !strcmp(line, "border") )
            {
                border = isTrue(cP);
            }
            else if( !strcmp(line, "linear") )
            {
                doLinear = isTrue(cP);
            }
            else if( !strcmp(line, "mikecmode") )
            {
                mikecMode = isTrue(cP);
            }
            else if( !strcmp(line, "gamma") )
            {
                gammaCorrection = atof(cP);
            }
            else if( !strcmp(line, "whitebias") )
            {
                whiteBias = atoi(cP);
            }
            else if( !strcmp(line, "cutoff") )
            {
                lowCutoff = atoi(cP);
            }
            else if( !strcmp(line, "tail") )
            {
                fadeTail = atof(cP);
            }
        }
    }

    fclose(fP);
}

// Given filename, search for it and if found, return the FILE *ptr for it.
// If the name begins with '~', use the home directory for the caller.
// If the file isn't found, null is returned.
FILE *
getFile(char *nameP)
{
char *cP;
char *dirP;
#ifndef _WIN32
struct passwd *pwdP;
#endif
char tmpstr[4096];
char tmpstr2[4096];

    if( *nameP == '~' )      // need to do directory expansion
    {
        strcpy(tmpstr, nameP);
        if( !(cP = strchr(tmpstr, '/')) )
        {
            return(NULL);        // malformed
        }

        *cP++ = 0;

        if( strlen(tmpstr) == 1 )    // ~/... form, user home
        {
            if( !(dirP = getenv("HOME")) )
            {
#ifdef _WIN32
                // No pwd.h on Windows - fall back to %USERPROFILE% etc via wincompat.
                dirP = (char *)winGetHomeDir();
                if( !dirP[0] )
                {
                    return(NULL);
                }
#else
                pwdP = getpwuid(getuid());
                if( !pwdP )
                {
                    return(NULL);
                }

                dirP = pwdP->pw_dir;
#endif
            }
        }
        else                        // ~uname/... form, uname's home
        {
#ifdef _WIN32
            // No per-username home directory lookup on Windows; only the
            // "~/..." (current user) form above is supported.
            return(NULL);
#else
            pwdP = getpwnam(tmpstr + 1);
            if( !pwdP )
            {
                return(NULL);
            }

            dirP = pwdP->pw_dir;
#endif
        }

        sprintf(tmpstr2, "%s/%s", dirP, cP);
        nameP = tmpstr2;
    }

    return( fopen(nameP, "r") );
}

// SIGINT and SIGTERM: stop the main loop, which then exits the way Escape does, timing report included.
// Nothing else is done here: SDL, stdio and exit() are not safe in a signal handler, and could
// deadlock on a lock held by the code the signal interrupted.
void
sighandler(int sig)
{
    (void)sig;
    quit = true;
}

// SIGHUP: ask the main loop to reload the configuration. The reload itself (file I/O, SDL calls,
// the timing report) is not safe in a signal handler, so reconfigure() runs from the main loop.
void
sighup(int sig)
{
    (void)sig;
    reloadRequested = true;
}

// Called on SIGHUP to reload config file, doesn't affect host, poort, size, bordered.
void
reconfigure(void)
{
    loadConfig(false);
    initializeRgbas();
    // These might have changed.
    SDL_SetTextureScaleMode(textures[0], (doLinear)?SDL_SCALEMODE_LINEAR:SDL_SCALEMODE_NEAREST);
    SDL_SetTextureScaleMode(textures[1], (doLinear)?SDL_SCALEMODE_LINEAR:SDL_SCALEMODE_NEAREST);
    SDL_SetWindowBordered(window, (border)?true:false);
    if( gpuFade )
    {
        selectFadeTerms();      // tail, gamma and scaling
    }

    // We also report timing if being accumulated so a snapshot can be seen without exiting.
    if( doTiming )
    {
        reportTiming();
    }
}

// Get the current system clock time in ns.
uint64_t
now()
{
struct timespec tm;
uint64_t now;

    clock_gettime( CLOCK_MONOTONIC, &tm );
    now = tm.tv_nsec;
    now += (uint64_t)tm.tv_sec * 1000 * 1000 * 1000;

    return(now);
}

void
reportTiming()
{
uint64_t delta;

    delta = (now() - startTime) / (1000 * 1000 * 1000);
    if( delta == 0 )
    {
        delta = 1;          // avoid divide-by-zero on very short runs
    }

    printf("Video driver is %s, renderer is %s%s\n",
        driverNameP, (rendererNameP)?rendererNameP:"?", (usingLabwc)?", using labwc":"");
    printf("%lu points drawn in %lu total seconds, %lu points/sec.\n",
        totalPoints, delta, totalPoints/delta);
    // "rendered" frames are passes that actually had active points and were drawn.
    // "paced" frames are every pass the 30fps pacer ran, including idle passes with nothing to draw.
    // If paced/sec is ~30 but rendered/sec is lower, the low rendered number is idle dilution,
    // NOT a slow renderer; if paced/sec itself is below 30, the renderer is the real limit.
    printf("%lu rendered frames, %lu/sec; %lu paced frames, %lu/sec.\n",
        totalFrames, totalFrames/delta, pacedFrames, pacedFrames/delta);
    printf("%u frame late events, max delay %lu msecs.\n", frameMisses, frameDelay/1000000);
    printf("%lu frames dropped rather than caught up.\n", droppedNs / FRAMETIME);
    if( renderCount )
    {
        printf("render time: avg %lu usec, max %lu usec, over %lu frames.\n",
            (renderTimeTotal / renderCount) / 1000, renderTimeMax / 1000, renderCount);
        printf("  of which: buffer (lock+memset+draw) avg %lu usec, present (clear+blit+vnc) avg %lu usec.\n",
            (phaseBufferTotal / renderCount) / 1000, (phasePresentTotal / renderCount) / 1000);
    }
    printf("%lu received points\n", receivedPoints);
    printf("%lu received points/sec\n", receivedPoints/delta);
    if( gpuFade )
    {
        printf("GPU fade, %.2f s to black (%d frames).\n", (fadeTermsP->life / 30.0), fadeTermsP->life);
        printf("%lu points dropped because one frame had more than %d new.\n", droppedPoints, MAXNEWPOINTS);
    }
    else
    {
        printf("%lu maximum active points\n", maxActivePoints);
        printf("%lu points dropped because active-point pool exhausted.\n", droppedPoints);
    }
}

void
usage()
{
    fprintf(stderr, "usage: t30dpy [-f] [-l] [-m] [-n] [-t]\n");
    fprintf(stderr, "              [-g gamma] [-w bias] [-p port] [-r renderer] [-s size] [host]\n");
    fprintf(stderr, "where:\n");
    fprintf(stderr, "-f, fade on the GPU even if the config says gpufade=false; the GPU fade is the default,\n");
    fprintf(stderr, "    the config's tail=seconds sets how long a point takes to fade out, 0.8 to 2.7,\n");
    fprintf(stderr, "    default %.1f, and -w and the config's cutoff apply to the CPU fade only\n", DEFAULTTAIL);
    fprintf(stderr, "-l, use SDL linear scaling, else nearest neighbor, default neearest\n");
    fprintf(stderr, "-m, Mike C mode, see the documentation, default false\n");
    fprintf(stderr, "-n, start with no border, default bordered\n");
    fprintf(stderr, "-t, accumulate timing data, display on exit, default off\n");
    fprintf(stderr, "-g gamma, set gamma to use, floating point, default %.4f\n", GAMMA);
    fprintf(stderr, "-w bias, add to the blue phosphor r and g for a dot's first frame, default %d\n",
        WHITEBIAS);
    fprintf(stderr, "-p port, set port to use, default %d\n", DEFAULTPORT);
    fprintf(stderr, "-r renderer, an SDL renderer name such as opengl, opengles2, software or vulkan;\n");
    fprintf(stderr, "    the default, auto, takes the first of opengl, opengles2 and software that opens\n");
    fprintf(stderr, "-s size, set display size to size pixels, >= 256, default 1024\n");
    fprintf(stderr, "host, hostname of server to connect to, default localhost\n");
    fprintf(stderr, "While running:\n");
    fprintf(stderr, "F11 or the f character goes into full screen mode or returns from it.\n");
    fprintf(stderr, "The b character toggles between a bordered and borderless window.\n");
    exit(1);
}
