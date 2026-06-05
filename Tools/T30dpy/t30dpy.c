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

#define READBUFSIZE 512         // xize of the buffer we read from the server into, bytes

#define NUMPOINTS 1024*1024     // total number of points on screen
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

// The following can be overridden by the config file and some also via the command line.
// For those that have a command line override, it takes precedence.
// See usage() below for those that have command line versions.
#define DEFAULTHOST "localhost"
#define DEFAULTPORT 3400

// End of config and command line settings

#define CURSORTIMEOUT 4000      // if no mouse motion after this many milliseconds, hide it

#define XYTOINDEX(x, y) (((x) * 1024) + (y))
#define INDEXTOX(i) ((i) / 1024)
#define INDEXTOY(i) ((i) & 1023)
#define XYTOPTR(base, pitch, x, y) (uint32_t *)((base) + ((y) * (pitch)) + (x * sizeof(uint32_t)))
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
typedef struct {
    bool valid;                 // set when the point should be displayed
    byte intensity;
    uint16_t lifetime;          // value is only 0-255, but use 16 bits because it would be padded to it anyway
} Point, *PointP;

Point pointData[NUMPOINTS];     // where all the data lives, enough for every possible point, 1024 x 1024
// Th precomputed rgba values for each possibe pdp-1 intensity and internal time step.
Rgba rgbaValues[8][256];

int pdp1FD;
int portNum;
char *hostNameP;

int winSize;
int lowCutoff = LOWCUTOFF;
int whiteBias = WHITEBIAS;
unsigned int droppedPoints;

bool quit = false;
bool border;
bool doLinear = LINEAR;
bool doVsync = VSYNC;
bool mikecMode = false;
bool havePoints = false;

float gammaCorrection = GAMMA;

uint32_t rgbaBlack;                 // the value is numeric 0, but use the SDL generated version for consistency
uint64_t totalPoints;
uint64_t totalPixels;
uint64_t receivedPoints;

SDL_Window *window;
SDL_Renderer *renderer;
SDL_Texture *texture;
SDL_PixelFormat *pixelFormatP;      // We use RGBA8888, set below

int openPort(char *hostNameP, int port);
uint32_t blend(int srcR, int srcG, int srcB, int srcA, int destR, int destG, int destB, int destA);
uint64_t now(void);
void *reader(void *argP);
void initializeRgbas(void);
void setPixel(uint8_t *pixels, int pitch, uint32_t rgba, int x, int y);
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
int i;
int opt;
int x, y;
int penx, peny;
int pitch;
bool fullscreen;
bool penDown;
bool doTiming;
uint64_t startTime;
uint64_t lastTime;
uint64_t deltaTime;
uint64_t cursorTime;
uint32_t frameMisses;
uint32_t frameDelay;

PointP pointP;
uint8_t *pixels;
uint32_t *pixelP;
uint32_t rgba;

SDL_Event event;

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
    totalPixels = 0;
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
            if( (i >= 256) )
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
    window = SDL_CreateWindow("T30dpy Type 30 Display",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, winSize, winSize,
            ((!border)?SDL_WINDOW_BORDERLESS:0) | SDL_WINDOW_ALLOW_HIGHDPI);

    // Create the renderer, set to black and display.
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | (doVsync)?SDL_RENDERER_PRESENTVSYNC:0 );
    SDL_RenderSetLogicalSize(renderer, 1024, 1024);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);

    // Create a texture, we stream our points to it.
    // Regardless of the window size, the logical size is always 1024 by 1024.
    // However, SDL2 is not very good at rendering pixels for some window sizes.
    // Pixels can be blurry regardless of the scale quality that is set.
    // Linear gives the best results balancing small screens vs larger ones but blurs the points some.
    // Nearest neighbor gives sharper dots, but some screen sizes don't scale well.
    // Select what works best for a given monitor and sceen size vial the command line or config file.
    // We control intensity by adjusting the alpha value, which is blended with the black the renderer
    // was originally set to, with alpha 255 being brightest.
    // Pixels use RGBA8888 representation, which is 32 bits.
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, 1024, 1024);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, (doLinear)?SDL_ScaleModeLinear:SDL_ScaleModeNearest);
    pixelFormatP = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA8888);
    rgbaBlack = SDL_MapRGBA(pixelFormatP, 0, 0, 0, 0);
     
    // Precomupute the possible rgba values over time and the intensity steps.
    initializeRgbas();

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
                    updatePen(pdp1FD, window, false, penx, peny);
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

        // During a rendering pass if there are no active points left, clear this.
        // There is no need to render and display a blank window.
        // When the receive thread gets a point, havePoints is set.
        if( !havePoints )
        {
            continue;           // If we have no points, don't do any window processing
        }

        // Since we alpha blend, the renderer has to be cleared each time.
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
        SDL_RenderClear(renderer);

        // Go thru the point list, handle each.
        // Pitch is set by SDL, it's the number of bytes in a row.
        // It's not safe to assume how many there are, some GPUs require padding.
        // The XYTOPTR() macro uses it to get a pointer to the proper pixel location for an x,y coordinate.
        SDL_LockTexture(texture, NULL, (void *)(&pixels), &pitch);

        havePoints = false;
        for( pointP = pointData, i = 0; i < NUMPOINTS; ++i, ++pointP )
        {
            x = INDEXTOX(i);
            y = 1023 - INDEXTOY(i);  // sdl 0 is top of screen, not bottom

            // This is done to clear the secondary pixels set by the beam spread simulation,
            // those pixels aren't tracked as valid points.
            // It's fine to clear these every time because they will be rewritten if the dot is still active.
            // No reason to go thru the rest of the code.
            if( !pointP->valid )
            {
                pixelP = XYTOPTR(pixels, pitch, x, y);
                *pixelP = rgbaBlack;        // clear the pixel
                continue;
            }

            rgba = rgbaValues[pointP->intensity][pointP->lifetime];

            if( (rgba != rgbaBlack) && (pointP->lifetime < MAXLIFETIME) )
            {
                drawPoint(pixels, pitch, rgba, x, y, pointP->intensity);
                pointP->lifetime++;
                havePoints = true;
                ++totalPoints;
            }
            else
            {
                pointP->valid = false;              // done with this now
                pixelP = XYTOPTR(pixels, pitch, x, y);
                *pixelP = rgbaBlack;                // clear the pixel
            }
        }

        SDL_UnlockTexture(texture);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
    }

    if( doTiming )
    {
        // lastTime is now a delta in seconds
        lastTime = (now() - startTime) / (1000 * 1000 * 1000);
        printf("%lu points drawn in %lu total seconds, %lu points/sec.\n",
            totalPoints, lastTime, totalPoints/lastTime);
        printf("%u frame late events, max delay %u msecs.\n", frameMisses, frameDelay/1000000);
        printf("%lu received points, %u dropped points.\n", receivedPoints, droppedPoints);
        printf("%lu received points/sec.\n", receivedPoints/lastTime);
        printf("%lu total pixels drawn, %lu pixels/sec.\n", totalPixels, totalPixels/lastTime);
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
void *
reader(void *argP)
{
int i;
int count;
int delay;
int x, y, intensity;
uint32_t cmd;
PointP pointP;
uint32_t buffer[READBUFSIZE];

static bool skipOne = false;

    for(;;)
    {
        if( (count = read(pdp1FD, buffer, sizeof(buffer))) <= 0 )
        {
            quit = true;
        }

        count /= sizeof(uint32_t);          // command words

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
            y = (cmd >> 10) & 01777;
            intensity = (cmd >> 20) & 7;    // the standard display intensity, 0-7

            pointP = &pointData[XYTOINDEX(x, y)];
            pointP->intensity = intensity;
            pointP->lifetime = 0;
            pointP->valid = true;
            havePoints = true;
        }
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
                rgba = SDL_MapRGBA(pixelFormatP, r, g, b, a);
                bias = 0;           // only the first value
            }
            else
            {
                rgba = rgbaBlack;
            }

            rgbaValues[i][j] = rgba;
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

    return( SDL_MapRGBA(pixelFormatP, rR, rG, rB, rA) );
}

// The loweest level, put an rgba pixel value into the texture memory.
void
setPixel(uint8_t *pixels, int pitch, uint32_t rgba, int x, int y)
{
    if( (x < 0) || (x > 1023) || (y < 0) || (y > 1023) )
    {
        return;     // not a valid point
    }

    *XYTOPTR(pixels, pitch, x, y) = rgba;
    ++totalPixels;
}

// Spread a point to simulate beam spread on a crt.
// The Type 30 doccumentation states a spot diameter of 0.030" max, about 3 pixels on its display.
// This siumulates it well by drawing extra dots based on the 0-7 intensity level.
void
drawPoint(uint8_t *pixels, int pitch, uint32_t rgba, int x, int y, int intensity)
{
    // If in Mike C. mode, draw only a single pixel.
    if( mikecMode )
    {
        setPixel(pixels, pitch, rgba, x, y);
        return;
    }

    // Intensities add extra bits.
    // Max fill is a 3x3 block.
    switch( intensity )
    {
    case 7:
    case 6:
        setPixel(pixels, pitch, rgba, x-1, y);
    case 5:
    case 4:
        setPixel(pixels, pitch, rgba, x, y-1);
        setPixel(pixels, pitch, rgba, x, y+1);
    case 3:
        setPixel(pixels, pitch, rgba, x+1, y-1);
        setPixel(pixels, pitch, rgba, x+1, y+1);
    case 2:
    case 1:
    case 0:
        setPixel(pixels, pitch, rgba, x, y);
        setPixel(pixels, pitch, rgba, x-1, y-1);
        setPixel(pixels, pitch, rgba, x-1, y+1);
        setPixel(pixels, pitch, rgba, x+1, y);
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

        pdpX = (int)((float)mouseX * (1024.0 / scale)) + offsetX;
        pdpY = (int)((float)mouseY * (1024.0 / scale)) + offsetY;

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

// See if there is a config file in the user home directory.
// If not, try the install directory.
// The file is named '.t30dyconfig' in the home directory,
// 't30dpyconfig' in the install directory.
// Lines are of the form 'param=value', empty lines or lines beginning with '#' are ignored,
// as are any invalid params.
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
                    if( i >= 256 )
                    {
                        winSize = i;
                    }
                }
            }

            if( !strcmp(line, "vsync") )
            {
                if( !strcmp(cP, "true") || (*cP == 'y') )
                {
                    doVsync = true;
                }
            }
            else if( !strcmp(line, "border") )
            {
                border = (!strcmp(cP, "true") || (*cP == 'y'));
            }
            else if( !strcmp(line, "linear") )
            {
                doLinear = (!strcmp(cP, "true") || (*cP == 'y'));
            }
            else if( !strcmp(line, "mikecmode") )
            {
                mikecMode = (!strcmp(cP, "true") || (*cP == 'y'));
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
    SDL_SetTextureScaleMode(texture, (doLinear)?SDL_ScaleModeLinear:SDL_ScaleModeNearest);
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
    fprintf(stderr, "-s size, set display size to size pixels, >= 256, default 1024\n");
    fprintf(stderr, "host, hostname of server to connect to, default localhost\n");
    fprintf(stderr, "While running:\n");
    fprintf(stderr, "F11 or the f character goes into full screen mode or returns from it.\n");
    fprintf(stderr, "The b character toggles between a bordered and borderless window.\n");
    exit(1);
}
