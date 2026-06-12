/*
 * t30dpy - a replacement for p7sim
 * This is MUCH lighter weight but simulates the Type 30 behavior quite well, better in some ways than p7sim.
 * The major differences are:
 * The color p7sim displayed was not totally correct for a p7 phosphor, the initial spot was too white.
 * The yellow phosphor decay time was too long.
 * See initializeRgbas() below for more details.
 * The increase in dot size with higher intensities to mimic beam spread was far too much.
 * Finally, p7sim was complex and an immense cpu hog. This version generally uses a fraction as much cpu.
 * It is a pure SDL2 application, no GL, which has benefits and drawbacks. SDL3 would be better, but it's
 * not supported yet on rPi.
 * GL is just too bloody complicated.
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
 * Author - Bill Ezell, wje
 * This can be freely used, modified, whatever. Please just keep the attribution to me.
 *
 * 20-May-2026 wje initial version
 * 23-May-2026 wje much fiddling to try to get window scaling to give decent visual results
 * 23-May-2026 wje fix typo assigning to hostNameP from cmd line arg
 * 25-May-2026 wje allow setting of gamma, vsync and linear/nearest via command line,
 *    adjust default gamma to match linear intensity to human-perceived intensity,
 *    modify dot-size adjustment for best appearance,
 *    add config file for various settings
 * 26-May-2026 wje precompute all possible rgba values, completely avoids floating point calcs while running
 * 31-May-2026 wje add Mike Chaoponis mode, optimize idle time
 * 31-May-2026 wje completely rework blending and eliminate blue hold,
 *    it was too blue and the blending wasn't quite right
 *  3-Jun-2026 wje add config reload on sighup, clean exit on sigint
 *  4-Jun-2026 wje commentary and code cleanup, remove unused includes, no functional changes
 *  7-Jun-2026 wje use the Google AI generated suggestion for the dot matrix vs intensity drawing
 *  8-Jun-2026 wje and add the Claude enhancements to above, backport the new list optimization from t30dpy3
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
    12-Jun-2026 wje minor code cleanup, remove some unused vars
*/

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <time.h>
#include <pwd.h>
#include <SDL2/SDL.h>

#define MINSIZE 256             // window can't be smaller than this
#define READBUFSIZE 512         // size of the buffer we read from the server into, bytes
#define WAYLANDMARGIN 128       // to work around stupid wayland, the inital window must be physical size - this

#define SIZE 1024               // really shouldn't be changed, it's proper for the Type 30
#define NUMPOINTS (SIZE*SIZE)   // total number of points on screen
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
#define VSYNC false             // ask SDL renderer to use vsync
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

// End of config and command line settings

#define CURSORTIMEOUT 4000      // if no mouse motion after this many milliseconds, hide it

#define XYTOPTR(base, pitch, x, y) (uint32_t *)((base) + ((y) * (pitch)) + ((x) * sizeof(uint32_t)))
#define APPLYGAMMA(alpha, gamma) (int)(powf((float)(alpha) / 255.0f, (gamma)) * 255.0f);

#define FRAMETIME 33333333L      // nanoseconds between frames, this is 30 fps

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
    uint16_t x;
    uint16_t y;
    byte intensity;
    uint16_t lifetime;          // value is only 0-255, but use 16 bits because it would be padded to it anyway
    uint32_t nextIdx;           // the forward link to the next one in the active list, or NOINDEX
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
ActivePoint activePool[MAXACTIVEPOINTS];   // the pool of active-point entries, indexed by uint32_t
uint32_t pointIndex[SIZE][SIZE];           // pointIndex[y][x] -> activePool[] index, or NOINDEX
uint32_t activeListHead;                   // head index of the list of points to display in a cycle, or NOINDEX
uint32_t freeListHead;                     // head index of the free pool entries, or NOINDEX
pthread_mutex_t busyLock = PTHREAD_MUTEX_INITIALIZER;       // for interlocking with the reader thread

// Th precomputed rgba values for each possibe pdp-1 intensity and internal time step.
Rgba rgbaValues[8][256];

int pdp1FD;
int portNum;
char *hostNameP;
const char *driverNameP;

int winSize;
int lowCutoff = LOWCUTOFF;
int whiteBias = WHITEBIAS;
uint64_t droppedPoints;         // count of points dropped because activePool[] was exhausted

bool usingLabwc = false;
bool quit = false;
bool border;
bool doLinear = LINEAR;
bool doVsync = VSYNC;
bool mikecMode = false;

float gammaCorrection = GAMMA;

uint32_t blackPixel;                 // The value is numeric 0, but use the SDL generated version for consistency.

uint64_t totalPoints;
uint64_t receivedPoints;
uint64_t totalFrames;
uint64_t maxActivePoints;
uint64_t activePoints;
bool doTiming;

SDL_PixelFormat *pixelFormatP;      // We use RGBA8888, set below.
SDL_Window *window;
SDL_Renderer *renderer;
SDL_Texture *textures[2];           // Double buffered textures so we don't block while it is being processed.
SDL_Texture *textureP;              // The current texture.
int textureSelector;

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
int penx, peny;
int opt;
int x, y;
uint32_t i;
int pitch;
bool fullscreen;
bool penDown;
uint64_t startTime;
uint64_t lastTime;
uint64_t deltaTime;
uint64_t cursorTime;
uint32_t frameMisses;
uint64_t frameDelay;
char *cP;

uint32_t pointIdx;
uint32_t prevIdx;
uint32_t nextIdx;
ActivePointP activePointP;
uint8_t *pixels;
uint32_t rgba;

SDL_Event event;
SDL_Rect bounds;

pthread_t thread;
pthread_attr_t tattr;
struct timespec sleepTime;
 
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
                fprintf(stderr, "Window can't be made less than 256, ignored.\n");
            }
            break;

        case 't':
            doTiming = true;
            break;

        case 'v':
            doVsync = true;     // enable vsync rendering
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
    SDL_Init(SDL_INIT_VIDEO);

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
        SDL_GetDisplayBounds(0, &bounds);
        if( winSize > (bounds.h - WAYLANDMARGIN) )
        {
            winSize = (bounds.h - WAYLANDMARGIN);
        }
    }

    window = SDL_CreateWindow("T30dpy Type 30 Display",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, winSize, winSize,
            ((!border)?SDL_WINDOW_BORDERLESS:0) | SDL_WINDOW_ALLOW_HIGHDPI);

    // Create the renderer, set to black and display.
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | (doVsync)?SDL_RENDERER_PRESENTVSYNC:0 );
    SDL_RenderSetLogicalSize(renderer, 1024, 1024);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);

    // Create the textures we write our points to.
    // We double-buffer to minimize screen tearing.
    // Regardless of the window size, the logical size is always 1024 by 1024.
    // However, SDL2 is not very good at rendering pixels for some window sizes.
    // Pixels can be blurry regardless of the scale quality that is set.
    // Linear gives the best results balancing small screens vs larger ones but blurs the points some.
    // Nearest neighbor gives sharper dots, but some screen sizes don't scale well.
    // Select what works best for a given monitor and sceen size vial the command line or config file.
    // We control intensity by adjusting the alpha value, which is blended with the black the renderer
    // was originally set to, with alpha 255 being brightest.
    // Pixels use RGBA8888 representation, which is 32 bits.
    // rgbaValues[][] (see initializeRgbas()) is premultiplied with alpha forced to 255,
    // so the texture can use SDL_BLENDMODE_NONE (a plain copy) instead of
    // SDL_BLENDMODE_BLEND (a per-pixel blend), saving a full-screen blend every frame.
    textures[0] = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, 1024, 1024);
    SDL_SetTextureBlendMode(textures[0], SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(textures[0], (doLinear)?SDL_ScaleModeLinear:SDL_ScaleModeNearest);
    textures[1] = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, 1024, 1024);
    SDL_SetTextureBlendMode(textures[1], SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(textures[1], (doLinear)?SDL_ScaleModeLinear:SDL_ScaleModeNearest);

    pixelFormatP = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA8888);
    blackPixel = SDL_MapRGBA(pixelFormatP, 0, 0, 0, 0);
     
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
    startTime = lastTime = now();

    SDL_ShowCursor(SDL_DISABLE);            // it is enabled when the mouse moves

    while( !quit )
    {
        while( SDL_PollEvent(&event) )
        {
            switch(event.type)
            {
            case SDL_QUIT:
                quit = true;
                break;

            case SDL_KEYDOWN:
                switch( event.key.keysym.scancode )
                {
                case SDL_SCANCODE_F11:
                case SDL_SCANCODE_F:
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(window, (fullscreen)?SDL_WINDOW_FULLSCREEN_DESKTOP:0);
                    break;

                case SDL_SCANCODE_ESCAPE:
                    quit = true;
                    break;

                case SDL_SCANCODE_B:
                    border = !border;
                    SDL_SetWindowBordered(window, (border)?SDL_TRUE:SDL_FALSE);
                    break;
                }
                break;

            case SDL_MOUSEMOTION:
                SDL_ShowCursor(SDL_ENABLE);
                cursorTime = now();
                if( penDown )
                {
                    SDL_GetMouseState(&penx, &peny);
                    updatePen(pdp1FD, window, true, penx, peny);
                }
                break;

            case SDL_MOUSEBUTTONDOWN:
                if( event.button.button == 1 )
                {
                    penDown = true;
                    SDL_GetMouseState(&penx, &peny);
                    updatePen(pdp1FD, window, true, penx, peny);
                }
                break;

            case SDL_MOUSEBUTTONUP:
                if(event.button.button == 1)
                {
                    penDown = false;
                    updatePen(pdp1FD, window, false, 0, 0);
                }
                break;

            case SDL_WINDOWEVENT:
                switch(event.window.event)
                {
                case SDL_WINDOWEVENT_CLOSE:
                    quit = true;
                    break;
                }
            }
        }

        // The display update is frame based.
        // If not time for the next frame, sleep until it is.
        // All rgba values are comupted for a frame rate of 30fps.
        deltaTime = (now() - lastTime);

        if( deltaTime < FRAMETIME )
        {
            sleepTime.tv_sec = 0;
            sleepTime.tv_nsec = FRAMETIME - deltaTime;
            nanosleep(&sleepTime, NULL);
        }
        else if( deltaTime > FRAMETIME )
        {
            // This is just for the timing metrics
            frameMisses++;
            if( deltaTime > frameDelay )
            {
                frameDelay = deltaTime;
            }
        }

        lastTime = now();
        if( cursorTime && (((lastTime - cursorTime) / 1000000) > CURSORTIMEOUT) )
        {
            SDL_ShowCursor(SDL_DISABLE);
            cursorTime = 0;
        }

        if( activeListHead == NOINDEX )
        {
            continue;               // nothing to do this cycle
        }

        textureP = textures[textureSelector];
        textureSelector ^= 1;

        if( SDL_LockTexture(textureP, NULL, (void *)&pixels, &pitch) != 0 )
        {
            fprintf(stderr, "Can't lock texture, %s\n", SDL_GetError());
            exit(1);
        }

        // Clearing the entire pixel array seems to be faster than clearing individual points, surprising.
        memset(pixels, 0, pitch * 1024);

        // Go thru the point list, handle each.
        // Technically, we should lock over the entire operation, but all that could happen
        // is that for one cycle the active list head might be off, no big deal.
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

        // No SDL_RenderClear() here: with SDL_BLENDMODE_NONE and the texture's
        // pixel buffer fully memset() above, SDL_RenderCopy() fully overwrites
        // the render target every frame, making a separate clear redundant.
        SDL_RenderCopy(renderer, textureP, NULL, NULL);
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
    SDL_FreeFormat(pixelFormatP);
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
// Precompute the rgba values used for displaying points.
// There are only 256 possible rgba values for each pdp-1 intensity,
// one per each lifetime value which ranges from 0-255.
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
            // Resetting the lifetime and intensity is all that is needed.
            pointIdx = pointIndex[y][x];

            if( pointIdx == NOINDEX )           // This is not a currently active point, add to active list
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

void
initializeRgbas()
{
int i, j;
int intensity;
int delta;
Uint8 r, g, b, a, bias;
Uint8 blueAlpha, yellowAlpha;
Rgba rgba;

    // Limit to the SDL2 range of 0-255
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
                SDL_GetRGBA(rgba, pixelFormatP, &r, &g, &b, &a);
                a = APPLYGAMMA(((int)a * intensity) / 255, gammaCorrection);

                // Premultiply the alpha into rgb here, at table-build time, and force
                // the stored alpha to fully opaque (255). This lets the texture use
                // SDL_BLENDMODE_NONE (a plain copy) instead of SDL_BLENDMODE_BLEND
                // (a per-pixel blend) at display time, and lets us skip the per-frame
                // SDL_RenderClear() since every pixel is now fully written every frame.
                r = (Uint8)(((int)r * (int)a) / 255);
                g = (Uint8)(((int)g * (int)a) / 255);
                b = (Uint8)(((int)b * (int)a) / 255);
                rgba = SDL_MapRGBA(pixelFormatP, r, g, b, 255);
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
// Initialize the pointIndex[][] lookup table to "no active point" everywhere,
// chain all of activePool[] onto the free list, and set both list heads to empty.
// By convention, the first subscript of pointIndex is y, the second x.
// This matches the SDL convention.
void
initializePoints()
{
int32_t x, y;
uint32_t i;

    activeListHead = NOINDEX;

    // No screen position has an active point yet.
    for( y = 0; y < SIZE; ++y )
    {
        for( x = 0; x < SIZE; ++x )
        {
            pointIndex[y][x] = NOINDEX;
        }
    }

    // Chain every pool entry onto the free list, each pointing to the next,
    // with the last entry terminated by NOINDEX.
    for( i = 0; i < (MAXACTIVEPOINTS - 1); ++i )
    {
        activePool[i].nextIdx = i + 1;
    }

    activePool[MAXACTIVEPOINTS - 1].nextIdx = NOINDEX;
    freeListHead = 0;
}

// Remove a point from the active list, adjusting the head and prior point if needed,
// clear its pointIndex[][] entry, and return its pool slot to the free list.
void
removeActivePoint(uint32_t pointIdx, uint32_t prevIdx)
{
ActivePointP pointP;

    pthread_mutex_lock(&busyLock);

    pointP = &activePool[pointIdx];

    pointIndex[pointP->y][pointP->x] = NOINDEX;     // done with this now

    if( prevIdx != NOINDEX )
    {
        activePool[prevIdx].nextIdx = pointP->nextIdx;
    }
    else
    {
        activeListHead = pointP->nextIdx;   // If there was no previous point, this point was the head, reset.
    }

    // Return this slot to the head of the free list.
    pointP->nextIdx = freeListHead;
    freeListHead = pointIdx;

    pthread_mutex_unlock(&busyLock);
}

// Add a point to the active list as the new head, taking a slot from the free list.
// The lock must be in place before calling!
// If the pool is exhausted, the point is silently dropped and droppedPoints is incremented.
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


// Blend 2 rgba values into a packed RGBA8888 result.
// This is the standard blend algorithm, same as SDL_BLEMDNOME_BLEND.
// We use this because we don't want to have to have SDL do the blending at display time.
// Note that a source alpha of 100%, 255, is opaque and will totally mask the destination alpha.
// For our purposes, blue is always the source and yellow the destination.
uint32_t
blend(int srcR, int srcG, int srcB, int sAlpha, int destR, int destG, int destB, int dAlpha)
{
int rR, rG, rB, rA;
float srcAlpha, destAlpha, newAlpha;

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

    return( SDL_MapRGBA(pixelFormatP, rR, rG, rB, rA) );
}

// Spread a point to simulate beam spread on a crt.
// The Type 30 doccumentation states a spot diameter of 0.030" max, about 3 pixels on its display.
// This siumulates it well by drawing extra dots based on the 0-7 intensity level.
// The actual pattern is thanks to Google AI.
// Credit where credit is due, even if it was trained on everyone else's data.
// Saved me the small effort of figuring it out myself.
// Note that x and y here are explicitly intended to be int, not u16_t.
// This allows the compiler to use more efficient cpu instructions.
// Yes, I worry about such things, and more developers should.
void
drawPoint(uint8_t *pixels, int pitch, uint32_t rgba, int x, int y, int intensity)
{
uint32_t *rowP;          // pointer to (x, y) in the current row
uint32_t *aboveRowP;     // pointer to (x, y-1), only valid if notTopEdge
uint32_t *belowRowP;     // pointer to (x, y+1), only valid if notBotEdge
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

    switch (intensity)
    {
    // Max intensity adds the sharp corners to complete the 3x3 square block
    case 7:
    case 6:
        if( notLeftEdge & notTopEdge )
        {
            *(aboveRowP - 1) = rgba;
        }
        if( notLeftEdge & notBotEdge )
        {
            *(belowRowP - 1) = rgba;
        }
        if( notRightEdge & notTopEdge )
        {
            *(aboveRowP + 1) = rgba;
        }
        if( notRightEdge & notBotEdge )
        {
            *(belowRowP + 1) = rgba;
        }
    // Medium intensity adds the left/right arms to form a wide cross
    case 5:
    case 4:
        if( notLeftEdge )
        {
            *(rowP - 1) = rgba;
        }
        if( notRightEdge )
        {
            *(rowP + 1) = rgba;
        }

    // Lowest intensities always draw the tight vertical core
    case 3:
    case 2:
    case 1:
    case 0:
        *rowP = rgba;                // Center core
        if( notTopEdge )
        {
            *aboveRowP = rgba;       // Top arm
        }
        if( notBotEdge )
        {
            *belowRowP = rgba;       // Bottom arm
        }
        break;
    }
}

// For the real hardware, the Type 30 would figure out if there was a hit
// at the last drawn pixel when issuing the completion pulse,
// but that's not possible here, let it be determined back in the pdp1 code.
// SDL scaling in full screen mode is flaky. It preserves the aspect ratio of the renderer, so if the
// height and width of the full screen are different, it pads the sides of the longer dimension but doesn't
// adjust the mouse coordinaates to match. Silly.
// We have to determine the offset and apply it during scaling.
void
updatePen(int sockFD, SDL_Window *window, bool penDown, int mouseX, int mouseY)
{
int winX, winY;
int pdpX, pdpY;
int offsetX, offsetY;
float scale;
uint32_t cmd;

    if( penDown )
    {
        // Window size might not match the logical drawing size of 1024, scale coords.
        // SDL doesn't properly scale mouse events in full screen mode, we have to use the smaller of the'
        // reported dimensions.
        SDL_GetWindowSize(window, &winX, &winY);
        if( winX > winY )
        {
            scale = (float)winY;
            offsetY = 0;
            offsetX = (winX - winY) / 2;
        }
        else
        {
            scale = (float)winX;
            offsetX = 0;
            offsetY = (winY - winX) / 2;
        }

        pdpX = (int)((float)(mouseX - offsetX) * (1024.0 / scale));
        pdpY = (int)((float)(mouseY - offsetY) * (1024.0 / scale));
        // Constrain the mouse since the SDL stuff is not reliable.
        if( pdpY < 0 )
        {
            pdpY = 0; 
        }

        if( pdpY > 1023 )
        {
            pdpY = 1023;
        }

        if( pdpX < 0 )
        {
            pdpX = 0; 
        }

        if( pdpX > 1023 )
        {
            pdpX = 1023;
        }

        // SDL has the upper left corner x,y as 0,0, ranging from 0 to 1023.
        // PDP1 is -511,511, ranging from -511 to 511 plus the PDP1 coords are 1's complement.
        // This really should have used the same 0-1023 range the display coordinates use,
        // but too many things depend upon it being center-origined.
        pdpX -= 511;
        if( pdpX < 0 )
        {
            --pdpX;             // 1's cmpl conversion
        }

        pdpY = 511 - pdpY;
        if( pdpY < 0 )
        {
            --pdpY;             // 1's cmpl conversion
        }

        cmd = 0xFF0 << 20;
        cmd |= (pdpX & 0x3FF) << 10;
        cmd |= (pdpY & 0x3FF);
    }
    else
    {
        cmd = 0xFF1 << 20;  // pen up cmd to host
    }

    // And send to host.
    write(sockFD, &cmd, 4);
}

// Get the current time in ns.
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
            else if( !strcmp(line, "vsync") )
            {
                doVsync = isTrue(cP);
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
    SDL_SetTextureScaleMode(textures[0], (doLinear)?SDL_ScaleModeLinear:SDL_ScaleModeNearest);
    SDL_SetTextureScaleMode(textures[1], (doLinear)?SDL_ScaleModeLinear:SDL_ScaleModeNearest);
    SDL_RenderSetVSync(renderer, doVsync);
    SDL_SetWindowBordered(window, (border)?SDL_TRUE:SDL_FALSE);
}

void
usage()
{
    fprintf(stderr, "usage: t30dpy [-l] [-m] [-n] [-t] [-v]\n");
    fprintf(stderr, "              [-g gamma] [-w bias] [-p port] [-s size] [host]\n");
    fprintf(stderr, "where:\n");
    fprintf(stderr, "-l, use SDL linear scaling, else nearest neighbor, default neearest\n");
    fprintf(stderr, "-m, Mike C mode, see the documentation, default false\n");
    fprintf(stderr, "-n, start with no border, default bordered\n");
    fprintf(stderr, "-t, accumulate timing data, display on exit, default off\n");
    fprintf(stderr, "-v, enable SDL vsync on render, default off\n");
    fprintf(stderr, "-g gamma, set gamma to use, floating point, default %.4f\n", GAMMA);
    fprintf(stderr, "-w bias, add to the blue phosphor r and g for a dot's first frame, default %d\n",
        WHITEBIAS);
    fprintf(stderr, "-p port, set port to use, default %d\n", DEFAULTPORT);
    fprintf(stderr, "-s size, set display size to size pixels, >= %d, default 1024\n", MINSIZE);
    fprintf(stderr, "host, hostname of server to connect to, default localhost\n");
    fprintf(stderr, "While running:\n");
    fprintf(stderr, "F11 or the f character goes into full screen mode or returns from it.\n");
    fprintf(stderr, "The b character toggles between a bordered and borderless window.\n");
    exit(1);
}
