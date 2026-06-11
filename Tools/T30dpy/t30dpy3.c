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
 *  10-Jun-2026 wje add workaround for totally broken wayland/labwc window control
 *  11-Jun-2026 wje CPU optimization pass for high active-point counts.
 *     - replaced the per-screen-position pointData[1024][1024] active list with a compact
 *       index-linked activePool[] (MAXACTIVEPOINTS) plus a pointIndex[][] lookup table,
 *       to keep active-list traversal cache-friendly.
 *     - drawPoint() now computes row base pointers once per point instead of recomputing
 *       full address arithmetic for every one of the up to 9 pixels it can touch.
 *     - rgbaValues[][] now stores premultiplied-alpha rgb, allowing
 *       SDL_BLENDMODE_NONE instead of SDL_BLENDMODE_BLEND and removing the per-frame
 *       SDL_RenderClear(), eliminating a full-screen blend and a full-screen clear
 *       every frame.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <ctype.h>
#include <pthread.h>
#include <signal.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <time.h>
#include <pwd.h>
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

// The following can be overridden by the config file and some also via the command line.
// For those that have a command line override, it takes precedence.
// See usage() below for those that have command line versions.
#define DEFAULTHOST "localhost"
#define DEFAULTPORT 3400

#define NSECPERUSEC 1000
#define NSECPERMSEC 1000000
#define CURSORTIMEOUT 4000      // if no mouse motion after this many milliseconds, hide it

#define XYTOPTR(base, pitch, x, y) (uint32_t *)((base) + ((y) * (pitch)) + ((x) * sizeof(uint32_t)))
#define APPLYGAMMA(alpha, gamma) (int)(powf((float)(alpha) / 255.0f, (gamma)) * 255.0f);

#define CONSTRAIN(x) ((x) < 0?0:(((x) > 1023)?1023:(x)))    // keep a value in the range 0-1023
#define FRAMETIME 33333333L     // nanoseconds between frames, this is 30 fps

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
// CACHE LOCALITY NOTE:
// Earlier versions stored Points by x,y screen position in a 1024x1024 with active points linked thru
// them.
// At high active-point counts, walking that list meant chasing pointers scattered across a ~25MB region,
// causing a cache miss on essentially every node every frame.
// Instead, ActivePoint records now live in a small, contiguous pool (activePool[]),
// and the list is threaded via array indices (nextIdx) rather than pointers.
// A separate, much smaller lookup table (pointIndex[][]) maps a logical (x,y) screen
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

// We keep the active points in a pool that effectively implements 2 linked lists.
// One is a list of free pool entries, the other a list of active points.
// As points come in, an entry is moved from the free list to the active list.
// When a point is no longer displayed, it is moved back to the free list.
// This way we don't have to check every point location to see if it's valid.
// Locking is not done between the reader and display threads.
// When a new item is added to the busy list, it is added to the head and the link atomically updated.
// When an item is taken from the free list, it is always taken from the head.
// When an item is moved from the busy list, it is added to the head of the free list.
ActivePoint activePool[MAXACTIVEPOINTS];     // compact pool of active-point records
uint32_t pointIndex[LOGICALSIZE][LOGICALSIZE]; // maps screen (y,x) -> pool index, or NOINDEX
uint32_t activeListHead;                     // head index of the active list, or NOINDEX if empty
uint32_t freeListHead;                       // head index of the free list, or NOINDEX if exhausted
uint64_t droppedPoints;                      // counts points dropped because the pool was exhausted
pthread_mutex_t busyLock = PTHREAD_MUTEX_INITIALIZER;       // for interlocking with the reader thread

Rgba rgbaValues[8][256];        // The rgba values for each possibe intensity and internal time step.

int pdp1FD;
int portNum;
char *hostNameP;
const char *driverNameP;

int winSize;
int lowCutoff = LOWCUTOFF;
int whiteBias = WHITEBIAS;

bool usingLabwc = false;
bool quit = false;
bool border;
bool doLinear = LINEAR;
bool mikecMode = false;

float gammaCorrection = GAMMA;

uint32_t blackPixel;                 // The value is numeric 0, but use the SDL generated version for consistency.

// These values are used only for timing metrics.
uint64_t totalPoints;
uint64_t receivedPoints;
uint64_t totalFrames;
uint64_t maxActivePoints;
uint64_t activePoints;
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
uint32_t blend(int srcR, int srcG, int srcB, int srcA, int destR, int destG, int destB, int destA);
uint64_t now(void);
void *reader(void *argP);

void initializePoints(void);
void addActivePoint(uint16_t x, uint16_t y, byte intensity);
void removeActivePoint(uint32_t pointIdx, uint32_t prevIdx);

void initializeRgbas(void);
void drawPoint(uint8_t *pixels, int pitch, uint32_t rgba, int x, int y, int intensity);
void updatePen(int sockFD, SDL_Window *winwdow, bool penDown, int winX, int winY);
void loadConfig(bool full);
void sighandler(int sig);
void reconfigure(int sig);
void usage(void);
FILE *getFile(char *nameP);

int
main(int argc, char **argv)
{
int opt;
int x, y;
int penx, peny;
uint32_t i;
bool fullscreen;
bool penDown;
char *cP;

uint64_t startTime;
uint64_t currentTime;
uint64_t lastTime;
uint64_t deltaTime;
uint64_t accumulator;       // Used for loop timing so the frane rate can be kept at 30fps.

uint64_t cursorTime;
uint64_t frameDelay;
uint32_t frameMisses;

uint32_t pointIdx;
uint32_t prevIdx;
uint32_t nextIdx;
ActivePointP activePointP;
uint8_t *pixels;
uint32_t rgba;
int pitch;

SDL_Event event;
SDL_PropertiesID props;
SDL_Rect bounds;

pthread_t thread;
pthread_attr_t tattr;
 
    hostNameP = DEFAULTHOST;
    portNum = DEFAULTPORT;        // display 0 on the pidp-1
    winSize = 1024;               // original Type 30 display size
    border = true;
    fullscreen = false;
    penDown = false;
    doTiming = false;
    totalPoints = 0;
    receivedPoints = 0;
    frameMisses = 0;
    frameDelay = 0;
    cursorTime = 0;

    loadConfig(true);             // config overrides defines, command line overrides all

    while( (opt = getopt(argc, argv, "g:lmnp:s:tvw:")) != -1 )
    {
        switch( opt )
        {
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

        case 's':
            i = atoi(optarg);   // screen is n * n big
            if( (i >= MINSIZE) )
            {
                winSize = i;
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

    if( (pdp1FD = openPort(hostNameP, portNum)) < 0 )
    {
        fprintf(stderr, "Can't open port %d on host %s.\n", portNum, hostNameP);
        usage();
        exit(1);
    }

    // SIGHUP will cause reloading of the configuration file, SIGTERM exits cleanly
    signal(SIGHUP, reconfigure);
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
    if( usingLabwc || (driverNameP && (SDL_strcmp(driverNameP, "wayland") == 0)) )
    {
        SDL_GetDisplayBounds(SDL_GetPrimaryDisplay(), &bounds);
        if( winSize > (bounds.h - WAYLANDMARGIN) )
        {
            winSize = (bounds.h - WAYLANDMARGIN);       // we only care about the vertical dimension
        }
    }

    if( !SDL_CreateWindowAndRenderer("T30dpy3 Type 30 Display", winSize, winSize,
        SDL_WINDOW_RESIZABLE|((!border)?SDL_WINDOW_BORDERLESS:0), &window, &renderer) )
    {
        fprintf(stderr,"Can't create window, %s\n", SDL_GetError());
        exit(1);
    }

    // Initialize renderer and clear window.
    SDL_SetRenderLogicalPresentation(renderer, 1024, 1024, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);

    // Since we're going to write to GPU vram, we need to use its pixel format for best efficiency.
    pixelFormat = SDL_PIXELFORMAT_RGBA8888;      // Default fallback
    props = SDL_GetRendererProperties(renderer);
    if( props )
    {
        // Query the preferred texture format for the specific backend.
        pixelFormat = ((SDL_PixelFormat *)SDL_GetPointerProperty(props,
            SDL_PROP_RENDERER_TEXTURE_FORMATS_POINTER, NULL))[0];
    }

    formatDetailsP = SDL_GetPixelFormatDetails(pixelFormat);
    textures[0] = SDL_CreateTexture(renderer, pixelFormat, SDL_TEXTUREACCESS_STREAMING, 1024, 1024);
    textures[1] = SDL_CreateTexture(renderer, pixelFormat, SDL_TEXTUREACCESS_STREAMING, 1024, 1024);

    // Regardless of the window size, the logical size is always 1024 by 1024.
    // SDL2 was not very good at rendering pixels for some window sizes, SDL3 is much better at scaling.
    // Linear gives the best results balancing small screens vs larger ones but blurs the points some.
    // Nearest neighbor gives sharper dots, but some screen sizes don't scale well.
    // Select what works best for a given monitor and sceen size vial the command line or config file.
    //
    // Intensity is encoded directly into the rgb values themselves (see initializeRgbas(),
    // which premultiplies by the desired alpha), rather than relying on the renderer to
    // blend a separate alpha against the background.
    // This lets us use SDL_BLENDMODE_NONE, which is a straight pixel copy with no
    // per-pixel read-modify-write blend math, instead of SDL_BLENDMODE_BLEND.
    // Because the copy covers the full 1024x1024 logical area every frame, this also makes
    // the per-frame SDL_RenderClear() unnecessary (see the main loop), saving a second
    // full-screen fill every frame on top of the blend itself.
    SDL_SetTextureBlendMode(textures[0], SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(textures[0], (doLinear)?SDL_SCALEMODE_LINEAR:SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(textures[1], SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(textures[1], (doLinear)?SDL_SCALEMODE_LINEAR:SDL_SCALEMODE_NEAREST);

    blackPixel = SDL_MapRGBA(formatDetailsP, NULL, 0, 0, 0, 0);
     
    // Precomupute the possible rgba values over time and the intensity steps.
    initializeRgbas();
    // Set up the points array and the active list head.
    initializePoints();

    // An async thread is used to read incoming data.
    // Lightpen updates are done in the main thread during the display update cycle.
    pthread_attr_init (&tattr);
    if( pthread_create(&thread, &tattr, reader, 0) )
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
            switch(event.type)
            {
            case SDL_EVENT_QUIT:
                quit = true;
                break;

            case SDL_EVENT_KEY_DOWN:
                switch( event.key.scancode )
                {
                case SDL_SCANCODE_F11:
                case SDL_SCANCODE_F:
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(window, (fullscreen)?SDL_WINDOW_FULLSCREEN:0);
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
                penDown = true;
                SDL_ConvertEventToRenderCoordinates(renderer, &event);
                penx = (int)event.button.x;
                peny = (int)event.button.y;
                penx = CONSTRAIN(penx);
                peny = CONSTRAIN(peny);
                updatePen(pdp1FD, window, true, penx, peny);
                SDL_ShowCursor();
                cursorTime = now();
                break;
                
            case SDL_EVENT_MOUSE_MOTION:
                cursorTime = now();

                if( penDown )
                {
                    SDL_ConvertEventToRenderCoordinates(renderer, &event);
                    penx = (int)event.motion.x;
                    peny = (int)event.motion.y;
                    penx = CONSTRAIN(penx);
                    peny = CONSTRAIN(peny);

                    updatePen(pdp1FD, window, true, penx, peny);
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_UP:
                penDown = false;
                updatePen(pdp1FD, window, false, penx, peny);
                break;
            }
        }

        if( cursorTime && (((now() - cursorTime) / NSECPERMSEC) > CURSORTIMEOUT) )
        {
            SDL_HideCursor();
            cursorTime = 0;
        }

        // The display update is frame based.
        // All rgba values are comupted for a frame rate of 30fps.
        // Brute force delay, avoids all the SDL3 internal timing issues and is ok for this emualtion.
        // The 'accumulator' pattern is used to be sure the frame rate is correct.
        // This corrects for variations in the SDL rendering time.
        if( accumulator < FRAMETIME )
        {
            SDL_DelayNS(FRAMETIME - accumulator);
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
        ++totalFrames;

        if( activeListHead == NOINDEX )
        {
            continue;               // nothing to do this cycle
        }

        textureP = textures[textureSelector];
        textureSelector ^= 1;

        if( !SDL_LockTexture(textureP, NULL, (void *)&pixels, &pitch) )
        {
            fprintf(stderr, "Can't lock texture, %s\n", SDL_GetError());
            exit(1);
        }

        // Clearing the entire pixel array seems to be faster than clearing individual points, surprising.
        memset(pixels, 0, pitch * 1024);

        // Go thru the active point list, handle each.
        // The list is threaded by index through activePool[] (see ActivePoint above), which
        // keeps traversal localized to a small contiguous pool instead of chasing pointers
        // across the full 1024x1024 grid.
        // Technically, we should lock over the entire operation, but all that could happen
        // is that for one cycle the active list head might not be current, no big deal.
        for( pointIdx = activeListHead, prevIdx = NOINDEX; pointIdx != NOINDEX; pointIdx = nextIdx )
        {
            activePointP = &activePool[pointIdx];
            x = activePointP->x;
            y = activePointP->y;
            rgba = rgbaValues[activePointP->intensity][activePointP->lifetime];

            nextIdx = activePointP->nextIdx;        // Get this now before a possible point removal.
            if( (rgba != blackPixel) && (activePointP->lifetime < MAXLIFETIME) )
            {
                drawPoint(pixels, pitch, rgba, x, y, activePointP->intensity);
                activePointP->lifetime++;
                prevIdx = pointIdx;                 // Only update if we are not removing this point.
                ++totalPoints;
            }
            else
            {
                // We don't update prevIdx in this case because that point remains the previous point.
                // Locking is handled in removeActivePoint().
                removeActivePoint(pointIdx, prevIdx);
            }
        }

        SDL_UnlockTexture(textureP);

        // No SDL_RenderClear() here: with SDL_BLENDMODE_NONE (set above) this
        // SDL_RenderTexture() call is a straight copy of the full 1024x1024 logical
        // area, so every destination pixel is overwritten unconditionally and a
        // separate clear would be redundant work.
        SDL_RenderTexture(renderer, textureP, NULL, NULL);
        SDL_RenderPresent(renderer);
    }

    if( doTiming )
    {
        // lastTime is now a delta in seconds
        lastTime = (now() - startTime) / (1000 * 1000 * 1000);
        printf("Video driver is %s%s\n", driverNameP, (usingLabwc)?", using labwc":"");
        printf("%lu points drawn in %lu total seconds, %lu points/sec.\n",
            totalPoints, lastTime, totalPoints/lastTime);
        printf("%lu total frames, %lu frames/sec.\n", totalFrames, totalFrames/lastTime);
        printf("%u frame late events, max delay %lu msecs.\n", frameMisses, frameDelay/1000000);
        printf("%lu received points\n", receivedPoints);
        printf("%lu received points/sec\n", receivedPoints/lastTime);
        printf("%lu maximum active points\n", maxActivePoints);
        printf("%lu points dropped because active-point pool exhausted.\n", droppedPoints);
    }

    close(pdp1FD);
     
    // clean up SDL.
    // Not really necessary, but it's good form.
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
     
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

    if( (sockFD = socket(AF_INET, SOCK_STREAM, 0)) < 0 )
    {
        freeaddrinfo(resultP);
        return(-1);         // fail
    }

    if( connect(sockFD, resultP->ai_addr, resultP->ai_addrlen) < 0 )
    {
        close(sockFD);
        freeaddrinfo(resultP);
        return(-1);         // fail
    }

    freeaddrinfo(resultP);

    // We want mouse events to go out quickly
    i = 1;
    setsockopt(sockFD, IPPROTO_TCP, TCP_NODELAY, &i, sizeof(i));
    return(sockFD);
}

// Reader thread to fetch data from server.
// See if there is data to read.
// If so, process it and add to the display list.
void *
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

    for(;;)
    {
        if( (count = read(pdp1FD, buffer, sizeof(buffer))) <= 0 )
        {
            quit = true;
            return(0);
        }

        count /= sizeof(uint32_t);          // command words

        pthread_mutex_lock(&busyLock);      // since we are reading muliples, lock for the duration

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

        pthread_mutex_unlock(&busyLock);
    }
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
            }
            else
            {
                rgba = blackPixel;
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

    freeListHead = 0;
    activeListHead = NOINDEX;
    droppedPoints = 0;
}

// Remove a point from the active list, adjusting the head and prior point's link if needed,
// and return its pool slot to the head of the free list for reuse.
// PointIdx is the pool index of the point being removed; prevIdx is the pool index of the
// previous entry on the active list, or NOINDEX if pointIdx was the head.
void
removeActivePoint(uint32_t pointIdx, uint32_t prevIdx)
{
    pthread_mutex_lock(&busyLock);

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

    pthread_mutex_unlock(&busyLock);

    if( doTiming )
    {
        --activePoints;
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
        ++activePoints;
        if( activePoints > maxActivePoints )
        {
            maxActivePoints = activePoints;
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
uint32_t rslt;

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
// The actual pattern is thanks to Google AI and optimized by Claude.
// Credit where credit is due, even if they were trained on everyone else's data.
// Saved me the small effort of figuring it out myself.
// Note that x and y here are explicitly intended to be int, not u16_t.
// This allows the compiler to use more efficient cpu instructions.
// Yes, I worry about such things, and more developers should.
void
drawPoint(uint8_t *pixels, int pitch, uint32_t rgba, int x, int y, int intensity)
{
// Row base pointers and the byte offset of column x within a row.
// XYTOPTR computes (base + y*pitch + x*sizeof(uint32_t)) from scratch for every pixel;
// since every pixel touched here is one of (x,y), its row neighbors, or its column
// neighbors, we can compute the three relevant row starts (this row, the row above,
// the row below) and the column byte offset just once, then reach every pixel with
// only an add. This avoids redundant multiplies that previously happened up to 9 times
// per point.
uint8_t *rowP;
uint8_t *aboveRowP;
uint8_t *belowRowP;
size_t xOff;

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

    rowP = pixels + ((size_t)y * (size_t)pitch);
    aboveRowP = rowP - pitch;          // only valid/used when notTopEdge
    belowRowP = rowP + pitch;          // only valid/used when notBotEdge
    xOff = (size_t)x * sizeof(uint32_t);

    switch (intensity)
    {
    // Max intensity adds the sharp corners to complete the 3x3 square block
    case 7:
    case 6:
        if( notLeftEdge & notTopEdge )
        {
            *(uint32_t *)(aboveRowP + xOff - sizeof(uint32_t)) = rgba;   // (x-1, y-1)
        }
        if( notLeftEdge & notBotEdge )
        {
            *(uint32_t *)(belowRowP + xOff - sizeof(uint32_t)) = rgba;   // (x-1, y+1)
        }
        if( notRightEdge & notTopEdge )
        {
            *(uint32_t *)(aboveRowP + xOff + sizeof(uint32_t)) = rgba;   // (x+1, y-1)
        }
        if( notRightEdge & notBotEdge )
        {
            *(uint32_t *)(belowRowP + xOff + sizeof(uint32_t)) = rgba;   // (x+1, y+1)
        }
    // Medium intensity adds the left/right arms to form a wide cross
    case 5:
    case 4:
        if( notLeftEdge )
        {
            *(uint32_t *)(rowP + xOff - sizeof(uint32_t)) = rgba;        // (x-1, y)
        }
        if( notRightEdge )
        {
            *(uint32_t *)(rowP + xOff + sizeof(uint32_t)) = rgba;        // (x+1, y)
        }
    // Lowest intensities always draw the tight vertical core
    case 3:
    case 2:
    case 1:
    case 0:
        *(uint32_t *)(rowP + xOff) = rgba;             // Center core
        if( notTopEdge )
        {
            *(uint32_t *)(aboveRowP + xOff) = rgba;    // Top arm
        }
        if( notBotEdge )
        {
            *(uint32_t *)(belowRowP + xOff) = rgba;    // Bottom arm
        }
        break;
    }
}

// For the real hardware, the Type 30 would figure out if there was a hit
// at the last drawn pixel when issuing the completion pulse,
// but that's not possible here, let it be determined back in the pdp1 code.
// SDL3 handles the mouse coordinate transforms for scaled windows itself now, unlike all the hoops SDL2 forced.
void
updatePen(int sockFD, SDL_Window *window, bool penDown, int mouseX, int mouseY)
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

        cmd = 0xFF0 << 20;
        cmd |= (mouseX & 0x3FF) << 10;
        cmd |= (mouseY & 0x3FF);
    }
    else
    {
        cmd = 0xFF1 << 20;  // pen up cmd to host
    }

    // And send to host.
    write(sockFD, &cmd, 4);
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
                    }
                }
            }

            if( !strcmp(line, "border") )
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
struct passwd *pwdP;
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
                pwdP = getpwuid(getuid());
                if( !pwdP )
                {
                    return(NULL);
                }

                dirP = pwdP->pw_dir;
            }
        }
        else                        // ~uname/... form, uname's home
        {
            pwdP = getpwnam(tmpstr + 1);
            if( !pwdP )
            {
                return(NULL);
            }

            dirP = pwdP->pw_dir;
        }

        sprintf(tmpstr2, "%s/%s", dirP, cP);
        nameP = tmpstr2;
    }

    return( fopen(nameP, "r") );
}

// Just close and exit
void
sighandler(int sig)
{
    if( pdp1FD )
    {
        close(pdp1FD);
    }

    // Not really necessary, but it's good form.
    SDL_Quit();
    exit(0);
}

// Called on SIGHUP to reload config file, doesn't affect host, poort, size, bordered.
void
reconfigure(int sig)
{
    loadConfig(false);
    initializeRgbas();
    // These might have changed.
    SDL_SetTextureScaleMode(textures[0], (doLinear)?SDL_SCALEMODE_LINEAR:SDL_SCALEMODE_NEAREST);
    SDL_SetTextureScaleMode(textures[1], (doLinear)?SDL_SCALEMODE_LINEAR:SDL_SCALEMODE_NEAREST);
    SDL_SetWindowBordered(window, (border)?true:false);
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
usage()
{
    fprintf(stderr, "usage: t30dpy [-l] [-m] [-n] [-t]\n");
    fprintf(stderr, "              [-g gamma] [-w bias] [-p port] [-s size] [host]\n");
    fprintf(stderr, "where:\n");
    fprintf(stderr, "-l, use SDL linear scaling, else nearest neighbor, default neearest\n");
    fprintf(stderr, "-m, Mike C mode, see the documentation, default false\n");
    fprintf(stderr, "-n, start with no border, default bordered\n");
    fprintf(stderr, "-t, accumulate timing data, display on exit, default off\n");
    fprintf(stderr, "-g gamma, set gamma to use, floating point, default %.4f\n", GAMMA);
    fprintf(stderr, "-w bias, add to the blue phosphor r and g for a dot's first frame, default %d\n",
        WHITEBIAS);
    fprintf(stderr, "-p port, set port to use, default %d\n", DEFAULTPORT);
    fprintf(stderr, "-s size, set display size to size pixels, >= 256, default 1024\n");
    fprintf(stderr, "host, hostname of server to connect to, default localhost\n");
    fprintf(stderr, "While running:\n");
    fprintf(stderr, "F11 or the f character goes into full screen mode or returns from it.\n");
    fprintf(stderr, "The b character toggles between a bordered and borderless window.\n");
    exit(1);
}
