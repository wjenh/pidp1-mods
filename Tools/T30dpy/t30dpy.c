/*
 * t30dpy - a replacement for p7sim
 * This is MUCH lighter weight but simulates the Type 30 behavior quite well, better in some ways than p7sim.
 * The major differences are:
 * The color p7sim displayed was not correct for a p7 phosphor, the initial spot was too white.
 * The yellow phosphor decay time was too long.
 * See initializePhosphors() below for more details.
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
 * Second, the first frame uses a different rgb value to make it brighter. Unfortunately, this shifts the
 * color, but following frames use the correct rgb value.
 * It uses a logical window size of 1024*1024 to match the Type 30 display and lets SDL do the scaling.
 *
 * The variable dot size is handled by drawing a sequence of dots for each intensity.
 *
 * Author - Bill Ezell, wje
 * This can be freely used, modified, whatever. Please just keep the attribution to me.
 *
 * 20-May-2026 wje initial version
 * 23-May-2026 wje much fiddling to try to get window scaling to give decent visual results
*/

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <SDL2/SDL.h>

#define DEFAULTHOST "localhost"
#define DEFAULTPORT 3400
#define READBUFSIZE 512         // size of the buffer we read from the server into, bytes

#define NUMPOINTS 1024*1024     // Total number of points on screen
#define KEEPPOINTS NUMPOINTS    // Total number of points that can be active, t30dpy can handle them all

// The alpha values for an intensity are computed using a power law.
#define BLUEDECAYALPHA  1.5     // intensity = time^-DECAYALPHA (time to the -alpha power) normalized to 0-255
#define YELLOWDECAYALPHA  0.85

// After blending, the resulting alpha also has a power law applied to adjust for the response of a crt
// which was nonlinear.
#define GAMMA 0.7               // gamma applies after an alpha is calculated

#define BLUEFIRSTRGB 201, 140, 255
#define BLUERGB 61, 0, 255
#define YELLOWRGB 179, 255, 0

// To help mimic the very bright 50 usec intensity of the original crt, these two do some fakery:
#define BLUEHOLD 4              // show the initial blue until the lifetime is greater than or equal to this
#define YELLOWDEFER 2           // don't show any yellow until the lifetime is greater than or equal to this

// Note that the alpha decay times are based on a lifetime of 255 frames
#define MAXLIFETIME 255         // number of frames a point will exist unless its alptha falls below:
#define LOWCUTOFF 5             // alphas less than this are not displayed

#define CURSORTIMEOUT 4000      // If no mouse motion after this many milliseconds, hide it

#define XYTOINDEX(x, y) (((x) * 1024) + (y))
#define INDEXTOX(i) ((i) / 1024)
#define INDEXTOY(i) ((i) & 1023)
#define XYTOPTR(base, pitch, x, y) (uint32_t *)((base) + ((y) * (pitch)) + (x * sizeof(uint32_t)))

#define FRAMETIME 33333000      // nanoseconds between frames, this is 30 fps

typedef unsigned char byte;
uint64_t totalPoints;
uint64_t receivedPoints;

// The description of a point to display.
// It contains the values to compute the intensity over time for the two phospors,
// the fast-decay blue and the slow-decay yellow.
// The lifetime field is used to select the alpha for the decay interval and ranges from 0-255.
// The phosphors decay in intensity by power-law decay, not exponential decay.
// The intensity field ranges from 0-7 and is used to scale the alpha values, 0 being dimmest, 7 brightest.
typedef struct {
    byte valid;
    byte intensity;
    int lifetime;
} Point, *PointP;

Point pointData[NUMPOINTS];      // where all the data lives

// Map the passed intensity levels 0-7 to the base display intensity levels
byte intensityMap[] = {32, 64, 96, 128, 160, 192, 224, 255};

int pdp1FD;
_Atomic int numPoints;
unsigned droppedPoints;

bool quit = false;

// We precompute these values, the index is the time from when the point was first displayed, 0-255
byte blueAlphas[256];
byte yellowAlphas[256];
SDL_PixelFormat *pixelFormatP;

int openPort(char *hostNameP, int port);
void *reader(void *argP);
void initializeAlphas(byte blue[], byte yellow[]);
uint32_t blend(int srcR, int srcG, int srcB, float srcA, int destR, int destG, int destB, float destA);
void setPixel(uint8_t *pixels, int pitch, uint32_t rgba, int x, int y);
void drawPoint(uint8_t *pixels, int pitch, uint32_t rgba, int x, int y, int intensity);
void updatePen(int sockFD, SDL_Window *winwdow, bool penDown, int winX, int winY);
void usage(void);
uint64_t now(void);

int
main(int argc, char **argv)
{
int portNum;
int winSize;
int i;
int opt;
int x, y;
int blueAlpha, yellowAlpha;
int intensity;
int penx, peny;
bool border;
bool fullscreen;
bool penDown;
bool doTiming;
char *hostNameP;

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
uint32_t rgbaBlack;
int pitch;

SDL_Event event;
SDL_Window *window;
SDL_Renderer *renderer;
SDL_Texture *texture;

pthread_t thread;
pthread_attr_t tattr;
 
    hostNameP = DEFAULTHOST;
    portNum = DEFAULTPORT;          // and display 0 on the pidp-1
    winSize = 1024;                 // typical
    border = true;
    fullscreen = false;
    penDown = false;
    doTiming = false;
    totalPoints = 0;
    receivedPoints = 0;
    frameMisses = 0;
    frameDelay = 0;
    cursorTime = 0;

    while( (opt = getopt(argc, argv, "p:s:nt")) != -1 )
    {
        switch( opt )
        {
        case 'p':
            portNum = atoi(optarg);
            break;

        case 'n':
            border = false;     // no border
            break;

        case 's':
            i = atoi(optarg);     // screen is n * n big
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

        default:
            usage();
            break;
        }
    }

    if( optind < argc )
    {
        hostNameP -argv[optind];
    }

    if( (pdp1FD = openPort(hostNameP, portNum)) < 0 )
    {
        fprintf(stderr, "Can't open port %d on host %s.\n", portNum, hostNameP);
        usage();
        exit(1);
    }

    // init SDL
    SDL_Init(SDL_INIT_VIDEO);
    window = SDL_CreateWindow("Type 30 Display",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, winSize, winSize,
            ((!border)?SDL_WINDOW_BORDERLESS:0) | SDL_WINDOW_ALLOW_HIGHDPI);

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(renderer, 1024, 1024);

    // Create a texture, we stream our points to it.
    // Regardless of the window size, the logical size is always 1024
    // However, SDL2 is not very good at rendering pixels for some window sizes less than the texture size.
    // Pixels can be blurry regardless of the scale quality that is set.
    // Nearest gives the best results.
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, 1024, 1024);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_ScaleModeNearest);
    pixelFormatP = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA8888);
    rgbaBlack = SDL_MapRGBA(pixelFormatP, 0, 0, 0, 0);
     
    // We also precomupute the alpha values over time
    initializeAlphas(blueAlphas, yellowAlphas);

    // An async thread is used to read incoming data.
    // Lightpen updates are done in the main thread during the display update cycle.
    pthread_attr_init (&tattr);
    if( pthread_create(&thread, &tattr, reader, 0) )
    {
        fprintf(stderr, "Can't create reader thread\n");
        exit(1);
    }

    // display the screen
    startTime = lastTime = now();

    SDL_ShowCursor(SDL_DISABLE);

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

        deltaTime = (now() - lastTime);

        if( deltaTime < FRAMETIME )
        {
            usleep((FRAMETIME - deltaTime)/1000);
        }
        else if( deltaTime > FRAMETIME )
        {
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

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
        SDL_RenderClear(renderer);

        // Go thru the point list, handle each
        SDL_LockTexture(texture, NULL, (void *)(&pixels), &pitch);

        for( pointP = pointData, i = 0; i < NUMPOINTS; ++i, ++pointP )
        {
            x = INDEXTOX(i);
            y = 1023 - INDEXTOY(i);  // sdl 0 is top of screen, not bottom

            if( pointP->valid == 0 )
            {
                pixelP = XYTOPTR(pixels, pitch, x, y);
                *pixelP = rgbaBlack;        // clear the pixel
                continue;
            }

            // Fadout is done by adjusting the alphas based on time using the precomputed alpha modifiers.
            intensity = intensityMap[pointP->intensity];
            blueAlpha = (int)((float)intensity * ((float)blueAlphas[pointP->lifetime] / 255.0) + 0.5);
            yellowAlpha = (int)((float)intensity * ((float)yellowAlphas[pointP->lifetime] / 255.0) + 0.5);

            // Yellow has a looong tailoff, so stop when it gets down to LOWCUTOFF.
            // Also skip adding yellow for the first YELLOWDEFER frames so the blue will show up better.
            if( (yellowAlpha > LOWCUTOFF) && (pointP->lifetime >= YELLOWDEFER) && (blueAlpha > 0) )
            {
                rgba = blend((pointP->lifetime >= BLUEHOLD)?BLUERGB:BLUEFIRSTRGB, (float)blueAlpha / 255.0,
                    YELLOWRGB, (float)yellowAlpha / 255.0);
            }
            else if( blueAlpha > 0 )
            {
                // The first BLUEHOLD frames use an enhanced blue.
                // Remember that the real thing only intensified the screen dot for 5 usecs for the Type 30,
                // and only 0.5 usecs for the Type 340, and in both cases much brighter than a modern LCD
                // can replicate.
                // This bit makes sure the initial display is visible.
                rgba = SDL_MapRGBA(pixelFormatP, (pointP->lifetime >= BLUEHOLD)?BLUERGB:BLUEFIRSTRGB, blueAlpha);
            }
            else if( yellowAlpha > LOWCUTOFF )
            {
                // At this point, the blue has totally decayed, no reason to alpha blend now.
                rgba = SDL_MapRGBA(pixelFormatP, YELLOWRGB, yellowAlpha);
            }
            else
            {
                rgba = 0;
            }

            if( rgba )
            {
                ++totalPoints;
                drawPoint(pixels, pitch, rgba, x, y, pointP->intensity);
            }

            if( (yellowAlpha <= LOWCUTOFF) || (pointP->lifetime >= MAXLIFETIME) )
            {
                pointP->valid = 0;                  // done with this onw
                --numPoints;
            }
            else
            {
                pointP->lifetime++;
            }
        }

        SDL_UnlockTexture(texture);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
    }

    if( doTiming )
    {
        // lastTime is now a delta
        lastTime = (now() - startTime) / (1000 * 1000 * 1000);
        printf("%lu points drawn in %lu total seconds, %lu points/sec.\n",
            totalPoints, lastTime, totalPoints/lastTime);
        printf("%u frame late events, max delay %u msecs.\n", frameMisses, frameDelay/1000000);
        printf("%lu received points, %u dropped points.\n", receivedPoints, droppedPoints);
        printf("%lu received points/sec.\n", receivedPoints/lastTime);
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
                skipOne = true;
                continue;
            }

            if( numPoints > KEEPPOINTS )
            {
                ++droppedPoints;
                continue;                   // drop it
            }

            ++receivedPoints;

            x = cmd & 01777;
            y = (cmd >> 10) & 01777;
            intensity = (cmd >> 20) & 7;    // the standard display intensity, 0-7

            pointP = &pointData[XYTOINDEX(x, y)];
            if( !pointP->valid )
            {
                ++numPoints;
            }

            pointP->intensity = intensity;
            pointP->lifetime = 0;
            pointP->valid = 1;
        }
    }
}

// Precompute the decay alphas.
void
initializeAlphas(byte blue[], byte yellow[])
{
int i;

    // The blue phosphor had a 25-75 microsecond lifetime to 10% brightness.
    // That is far less than the frame rate and wouldn't be visible, so it is strecthed,
    // but the intensity falloff is still a power-law relationship.
    // The yellow phosphor had a 400 milisecond lifetime to 10% brightness.
    // At the set 30fps, the decay alpha gives an accurate decay time.
    // This data from the RCA Phosphors TPM-1508A technical note.
    for( i = 0; i < 256; ++i )
    {
        blue[i] = (int)(powf((float)i+1, -BLUEDECAYALPHA) * 255.0f);
        yellow[i] = (int)(powf((float)i+1, -YELLOWDECAYALPHA) * 255.0f);
    }
}

// Blend 2 rgba values into a packed RGBA8888 result.
// This is the standard blend algorithm, same as SDL_BLEMDNOME_BLEND.
// We use this to know what's going on, and it's a bit more efficient than writing 2 sets of pixels
// and letting SDL do the blending.
uint32_t
blend(int srcR, int srcG, int srcB, float srcAlpha, int destR, int destG, int destB, float destAlpha)
{
int rR, rG, rB, rA;
float newAlpha;
uint32_t rslt;

    rR = (int)((srcR * srcAlpha) + (destR * (1.0f - srcAlpha)));
    rG = (int)((srcG * srcAlpha) + (destG * (1.0f - srcAlpha)));
    rB = (int)((srcB * srcAlpha) + (destB * (1.0f - srcAlpha)));
    newAlpha = srcAlpha + (destAlpha * (1.0f - srcAlpha));
    rA = (int)(powf(newAlpha, GAMMA) * 255.0f + 0.5);

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
}

// Spread a point to simulate beam spread on a crt.
// The Type 30 doccumentation states a spot diameter of 0.030" max, about 3 pixels on its display.
// This siumulates it well enogh by drawing extra dots based on the 0-7 intensity level.
void
drawPoint(uint8_t *pixels, int pitch, uint32_t rgba, int x, int y, int intensity)
{
int i, j;
SDL_Rect rect;

    switch( intensity )
    {
    case 7:
        rect.x = x - 2;
        rect.y = y - 2;
        rect.h = 4;
        rect.w = 4;
        break;
    case 6:
    case 5:
    case 4:
    case 3:
        rect.x = x - 1;
        rect.y = y - 2;
        rect.h = 3;
        rect.w = 3;
        break;
    case 2:
    case 1:
    case 0:
        rect.x = x - 1;
        rect.y = y - 2;
        rect.h = 2;
        rect.w = 2;
        break;
    }

    // Our version of fillRect, not provided for streaming textures
    for( i = 0; i < rect.h; ++i )
    {
        for( j = 0; j < rect.w; ++j )
        {
            setPixel(pixels, pitch, rgba, rect.x + j, rect.y + i);
        }
    }
}

// For the real hardware, the Type 30 hardware would figure out if there was a hit
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

void
usage()
{
    fprintf(stderr, "usage: t30dpy [-p port] [-n] [-t] [-s size] [host]\n");
    fprintf(stderr, "where:\n");
    fprintf(stderr, "-p port, set port to use, default is %d\n", DEFAULTPORT);
    fprintf(stderr, "-n, start with no border\n");
    fprintf(stderr, "-t, accumulate timing data, display on exit\n");
    fprintf(stderr, "-s size, set display size to size pixels, >= 256\n");
    fprintf(stderr, "host, hostname of server to connect to\n");
    fprintf(stderr, "Host defaults to localhost, port to 3400.\n");
    fprintf(stderr, "While running:\n");
    fprintf(stderr, "F11 or the f character go into full screen mode or return from it.\n");
    fprintf(stderr, "Bhe b character toggles between a bordered and borderless window.\n");
    exit(1);
}
