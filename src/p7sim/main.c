/*
 * p7sim - simulator for Type 30 display
 * This was written originally by Angelo Papenhoff, aap.
 * It has been modified by Bill Ezell, wje, pdp1@quackers.net to add:
 * Lightpen control
 * Allow toggling borderless via 'b' key
 *
 * Amd, make it readable. :)
 * It uses the One True Formatting Style, keep it.
 * The formatting is based on research done at Stanford many years ago that determined the major causes
 * of coding errors, the formatting reduced that. It works.
 *
 * wje 05-Jan-26 break from original repo, now independent. Initial reformatting. New features.
 * wje 08-Feb-26 rework lightpen code
 * wje 04-Mar-26 clean up unused code, add window scaling from config file
 * wje 25-Mar-26 fix mouse coords and dpy scaling when window size is not 1024x1024
 * wje 27-Mar-26 allow starting with window size 512 and up, limit to phys screen size, don't allow
 *    resizing, makes computing light pen coords a huge mess
 * wje 28-Mar-26 don't constrain size if larger than physical screen
 * wje 3-May-26 optimizations, increase max pending vertex update list size
 * wje 3-May-26 decrease the vector list size a bit, 100K seems adequate
 *
*/

//#define DOLOGGING
#include "../blincolnlights/logger.h"
// Set desired log type to 1 to enable output assuming logging is defined.
#define LOG_LIGHTPEN 0
#define LOG_MOUSE 0
#define LOG_SCALING 0
#define LOG_SDL 0
#define LOG_DEBUG 0

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <unistd.h>

#include <sys/types.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

#include <pthread.h>

#include <SDL.h>
#include "glad/glad.h"

typedef uint64_t uint64;
typedef uint32_t uint32;
typedef uint16_t uint16;
typedef uint8_t uint8;

#define nil NULL
#define nelem(a) (sizeof(a)/sizeof(*a))

#define READBUFSIZE 2048
#define FRAMETIME 33333 // 30 fps
#define WIDTH 1024
#define HEIGHT 1024
#define BORDER 2
#define MAXUPDATES 100000   // defines the maximum number of vertices that can be updated, extras dropped
#define DEFAULTPORT 3400

// Safety enforecement, all the scaling and rounding could result in an index > 1023
#define CONSTRAIN_INDEX(i) (((i) > 1023)?1023:(i))

int winSize = WIDTH;                 // default if nothing set, is the logical window size, default is 1024x1024
int realxSize;                       // The size of the actual SDL window
int realySize;
bool fixedSize;
float xScaling;                     // computed by setScaling()
float yScaling;

int fullWidth = (WIDTH + 2*BORDER); // is overridden in main(), but preserve initialization just in case
int fullHeight = (HEIGHT + 2*BORDER);

int scalefoo = 0;
int intensityOverride = 8;

float maxsz = 0.0055f;
float minsz = 0.0018f;
float maxbr = 1.00f;
float minbr = 0.25f;

SDL_Window *window;
int netfd;

typedef struct Point Point;
struct Point
{
int x, y;
int i;
int time;
};

typedef struct Vertex Vertex;
struct Vertex
{
    float x, y;
    float u, v;
};

typedef struct PVertex PVertex;
struct PVertex
{
    float x, y;
    float u, v;
    float cx, cy;
    float size, age;
    float intensity;
};

//PVertex pverts[6 * 10000];
// Each displayed point takes 6 entries here, no idea why
PVertex pverts[6 * MAXUPDATES];

#define void_offsetof (void*)(uintptr_t)offsetof
int indices[1024 * 1024];
Point newpoints[1024 * 1024];
int nnewpoints;
Point points[1024 * 1024];
int npoints;

uint32 screenmodes[2] = { 0, SDL_WINDOW_FULLSCREEN_DESKTOP };
int fullscreen;

GLuint vbo;
GLuint pvbo;
GLint point_program, excite_program, combine_program;
GLuint gltex;
GLuint whiteTex, yellowTex[2];
GLuint whiteFBO, yellowFBO[2];
int flip;

int border = 1;
int doLightpen = 0;
int penx;
int peny;

void usage(char *nameP);
void updatepen(bool penDown, int x, int y);
bool checkConfig(char *optionP);
bool getConfig(char *optionP, char *rsltP);
void closeConfigFile(void);
void setScaling(void);
int getMaxWindowSize(SDL_Window *windowP);

void
panic(char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
    SDL_Quit();
}

int
readn(int fd, void *data, int n)
{
    int m;

    while(n > 0)
    {
        m = read(fd, data, n);

        if(m <= 0)
        {
            return -1;
        }

        data += m;
        n -= m;
    }

    return 0;
}

int
dial(const char *host, int port)
{
    char portstr[32];
    int sockfd;
    struct addrinfo *result, *rp, hints;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(portstr, 32, "%d", port);

    if(getaddrinfo(host, portstr, &hints, &result))
    {
        perror("error: getaddrinfo");
        return -1;
    }

    for(rp = result; rp; rp = rp->ai_next)
    {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

        if(sockfd < 0)
        {
            continue;
        }

        if(connect(sockfd, rp->ai_addr, rp->ai_addrlen) >= 0)
        {
            goto win;
        }

        close(sockfd);
    }

    freeaddrinfo(result);
    perror("error");
    return -1;

win:
    freeaddrinfo(result);
    return sockfd;
}

int
serve1(int port)
{
int sockfd, confd;
socklen_t len;
struct sockaddr_in server, client;
int x;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if(sockfd < 0)
    {
        perror("error: socket");
        return -1;
    }

    x = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (void *)&x, sizeof x);

    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons(port);

    if(bind(sockfd, (struct sockaddr*)&server, sizeof(server)) < 0)
    {
        perror("error: bind");
        return -1;
    }

    listen(sockfd, 5);
    len = sizeof(client);

    while(confd = accept(sockfd, (struct sockaddr*)&client, &len), confd >= 0)
    {
        return confd;
    }

    perror("error: accept");
    return -1;
}

void
printlog(GLuint object)
{
GLint log_length;
char *log;

    if(glIsShader(object))
    {
        glGetShaderiv(object, GL_INFO_LOG_LENGTH, &log_length);
    }
    else
        if(glIsProgram(object))
        {
            glGetProgramiv(object, GL_INFO_LOG_LENGTH, &log_length);
        }
        else
        {
            fprintf(stderr, "printlog: Not a shader or a program\n");
            return;
        }

    log = (char*) malloc(log_length);

    if(glIsShader(object))
    {
        glGetShaderInfoLog(object, log_length, NULL, log);
    }
    else if(glIsProgram(object))
    {
        glGetProgramInfoLog(object, log_length, NULL, log);
    }

    fprintf(stderr, "%s", log);
    free(log);
}

GLint
compileshader(GLenum type, const char *src)
{
GLint shader, success;

    shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);

    if(!success)
    {
        fprintf(stderr, "Error in shader\n");
        printf("%s\n", src);
        printlog(shader);
        exit(1);
        return -1;
    }

    return shader;
}

GLint
linkprogram(GLint vs, GLint fs)
{
    GLint program, success;

    program = glCreateProgram();

    glBindAttribLocation(program, 0, "in_pos");
    glBindAttribLocation(program, 1, "in_uv");
    glBindAttribLocation(program, 2, "in_params1");
    glBindAttribLocation(program, 3, "in_params2");

    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &success);

    if(!success)
    {
        fprintf(stderr, "glLinkProgram:");
        printlog(program);
        exit(1);
        return -1;
    }

    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "tex0"), 0);
    glUniform1i(glGetUniformLocation(program, "tex1"), 1);

    return program;
}

void
setvbo(void)
{
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    int stride = sizeof(Vertex);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, 0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, void_offsetof(Vertex, u));
}

void
setpvbo(void)
{
    glBindBuffer(GL_ARRAY_BUFFER, pvbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);
    int stride = sizeof(PVertex);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, 0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, void_offsetof(PVertex, u));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, void_offsetof(PVertex, cx));
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, void_offsetof(PVertex, intensity));
}

pthread_mutex_t mutex;

void
init_synch(void)
{
    pthread_mutex_init(&mutex, nil);
}

void
lockData(void)
{
    pthread_mutex_lock(&mutex);
}

void
unlockData(void)
{
    pthread_mutex_unlock(&mutex);
}

void
draw(void)
{
int w, h;
float xScale, yScale;

    logger(LOG_DEBUG, "%f %d. %.2f %.2f %.2f\n", dt, npoints, st, rt, rt - st);

    glViewport(0, 0, fullWidth, fullHeight);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    /* draw white phosphor */
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);

    glBindFramebuffer(GL_FRAMEBUFFER, whiteFBO);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(point_program);

    PVertex *vp = pverts;
    int i;

    if( fixedSize )
    {
        xScale = yScale = 1024.0/(float)winSize;
    }
    else if( fullscreen )
    {
        // Scaling is done for us by SDL
        xScale = 1.0;
        yScale = 1.0;
    }
    else
    {
        xScale = xScaling;
        yScale = yScaling;
    }

    lockData();
    for(i = 0; i < npoints; i++)
    {
        if(vp >= &pverts[nelem(pverts)])
        {
            break;
        }

        float x = ((float)points[i].x / xScale) / (float)winSize + (float)((border)?(BORDER / fullWidth):0);
        float y = ((float)points[i].y / yScale) / (float)winSize + (float)((border)?(BORDER / fullWidth):0);
// teco uses 3
// spacewar uses 4
// DDT uses 7
        float sz = minsz + (maxsz - minsz) * (points[i].i / 7.0f);
        float br = minbr + (maxbr - minbr) * (points[i].i / 7.0f);

        PVertex *v = vp++;
        // TODO: could also do that in shader
        v->cx = x * 2.0f - 1.0f;
        v->cy = y * 2.0f - 1.0f;
        v->size = sz;
        v->age = points[i].time / 50000.0f;
        v->intensity = br;
        memcpy(&(vp++)->cx, &v->cx, sizeof(PVertex) - sizeof(Vertex));
        memcpy(&(vp++)->cx, &v->cx, sizeof(PVertex) - sizeof(Vertex));
        memcpy(&(vp++)->cx, &v->cx, sizeof(PVertex) - sizeof(Vertex));
        memcpy(&(vp++)->cx, &v->cx, sizeof(PVertex) - sizeof(Vertex));
        memcpy(&(vp++)->cx, &v->cx, sizeof(PVertex) - sizeof(Vertex));
    }
    unlockData();

    setpvbo();
    glBufferData(GL_ARRAY_BUFFER, i * 6 * sizeof(PVertex), pverts, GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, i* 6);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_BLEND);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, whiteTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, yellowTex[flip]);

    /* draw and age yellow layer */
    setvbo();
    glBindFramebuffer(GL_FRAMEBUFFER, yellowFBO[!flip]);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(excite_program);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    /* compose final image */
    SDL_GetWindowSize(window, &w, &h);

    if(w > h)
    {
        glViewport((w - h) / 2, 0, h, h);
    }
    else
    {
        glViewport(0, (h - w) / 2, w, w);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, yellowTex[!flip]);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(combine_program);
    glDrawArrays(GL_TRIANGLES, 0, 6);


    /* clear state */
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);

    flip = !flip;
    SDL_GL_SwapWindow(window);
}

#ifdef GLES
#define glslheader "#version 100\nprecision highp float; precision highp int;\n" \
    "#define VSIN attribute\n" \
    "#define VSOUT varying\n" \
    "#define FSIN varying\n"
#define outcolor
#define output "gl_FragColor = color;\n"
#else
#define glslheader "#version 130\n" \
    "#define VSIN in\n" \
    "#define VSOUT out\n" \
    "#define FSIN in\n"
#define outcolor
#define output "gl_FragColor = color;\n"
#endif

const char *vs_src =
    glslheader
    "VSIN vec2 in_pos;\n"
    "VSIN vec2 in_uv;\n"
    "VSOUT vec2 v_uv;\n"
    "void main()\n"
    "{\n"
    "	v_uv = in_uv;\n"
    "	gl_Position = vec4(in_pos.x, in_pos.y, -0.5, 1.0);\n"
    "}\n";

/*
const char *fs_src =
glslheader
outcolor
"FSIN vec2 v_uv;\n"
"uniform sampler2D tex0;\n"
"void main()\n"
"{\n"
"   vec2 uv = vec2(v_uv.x, 1.0-v_uv.y);\n"
"   vec4 color = texture2D(tex0, uv);\n"
output
"}\n";
*/

const char *point_vs_src =
    glslheader
    "VSIN vec2 in_pos;\n"
    "VSIN vec2 in_uv;\n"
    "VSIN vec4 in_params1;\n"
    "VSIN float in_params2;\n"
    "VSOUT vec2 v_uv;\n"
    "VSOUT float v_fade;\n"
    "VSOUT float v_intensity;\n"
    "#define coord in_params1.xy\n"
    "#define scl in_params1.z\n"
    "#define age in_params1.w\n"
    "#define intensity in_params2\n"
    "void main()\n"
    "{\n"
    "	v_uv = in_uv;\n"
    "	v_intensity = intensity;\n"
    "	v_fade = pow(0.5, age);\n"
    "	gl_Position = vec4(in_pos.x*scl+coord.x, in_pos.y*scl+coord.y, -0.5, 1.0);\n"
    "}\n";

const char *point_fs_src =
    glslheader
    outcolor
    "FSIN vec2 v_uv;\n"
    "FSIN float v_fade;\n"
    "FSIN float v_intensity;\n"
    "void main()\n"
    "{\n"
    "	float dist = pow(length(v_uv*2.0 - 1.0), 2.0);\n"
    "	float intens = clamp(1.0-dist, 0.0, 1.0)*v_intensity;\n"
    "	vec4 color = vec4(0);\n"
    "	color.x = intens*v_fade;\n"
    "	color.y = intens;\n"
    "	color.z = 1.0;\n"
    output
    "}\n";

const char *excite_fs_src =
    glslheader
    outcolor
    "FSIN vec2 v_uv;\n"
    "uniform sampler2D tex0;\n"
    "uniform sampler2D tex1;\n"
    "void main()\n"
    "{\n"
    "	vec2 uv = vec2(v_uv.x, v_uv.y);\n"
    "	vec4 white = texture2D(tex0, uv);\n"
    "	vec4 yellow = texture2D(tex1, uv);\n"
    "	vec4 color = max(vec4(white.y*white.z), 0.987*yellow);\n"
    "	color = floor(color*255.0)/255.0;\n"
    output
    "}\n";

const char *combine_fs_src =
    glslheader
    outcolor
    "FSIN vec2 v_uv;\n"
    "uniform sampler2D tex0;\n"
    "uniform sampler2D tex1;\n"
    "void main()\n"
    "{\n"
    "	vec4 bphos1 = vec4(0.24, 0.667, 0.969, 1.0);\n"
    "	vec4 yphos1 = 0.9*vec4(0.475, 0.8, 0.243, 1.0);\n"
    "	vec4 yphos2 = 0.975*vec4(0.494, 0.729, 0.118, 0.0);\n"

    "	vec2 uv = vec2(v_uv.x, v_uv.y);\n"
    "	vec4 white = texture2D(tex0, uv);\n"
    "	vec4 yellow = texture2D(tex1, uv);\n"
    "	vec4 yel = mix(yphos2, yphos1, yellow.x);\n"
    "	float a = 0.663 * (yel.a + (1.0-cos(3.141569*yel.a))/2.0)/2.0;\n"
    "	vec4 color = bphos1*white.x*white.z + yel*a;\n"
    output
    "}\n";

void
texDefaults(void)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void
makeFBO(GLuint *fbo, GLuint *tex)
{
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fullWidth, fullHeight,
        0, GL_RGBA, GL_UNSIGNED_BYTE, nil);
    texDefaults();
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
}

void
initGL(void)
{
    GLint vs = compileshader(GL_VERTEX_SHADER, vs_src);
    GLint point_vs = compileshader(GL_VERTEX_SHADER, point_vs_src);
    GLint point_fs = compileshader(GL_FRAGMENT_SHADER, point_fs_src);
    GLint excite_fs = compileshader(GL_FRAGMENT_SHADER, excite_fs_src);
    GLint combine_fs = compileshader(GL_FRAGMENT_SHADER, combine_fs_src);
    point_program = linkprogram(point_fs, point_vs);
    excite_program = linkprogram(excite_fs, vs);
    combine_program = linkprogram(combine_fs, vs);

    glGenTextures(1, &gltex);
    glBindTexture(GL_TEXTURE_2D, gltex);
    texDefaults();

    makeFBO(&whiteFBO, &whiteTex);
    makeFBO(&yellowFBO[0], &yellowTex[0]);
    makeFBO(&yellowFBO[1], &yellowTex[1]);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);


    Vertex screenquad[] =
    {
        { -1.0f, -1.0f,     0.0f, 0.0f },
        { 1.0f, -1.0f,      1.0f, 0.0f },
        { 1.0f, 1.0f,       1.0f, 1.0f },

        { -1.0f, -1.0f,     0.0f, 0.0f },
        { 1.0f, 1.0f,       1.0f, 1.0f },
        { -1.0f, 1.0f,      0.0f, 1.0f },
    };
    GLuint stride = sizeof(Vertex);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(screenquad), screenquad, GL_STATIC_DRAW);


    stride = sizeof(PVertex);
    glGenBuffers(1, &pvbo);
    glBindBuffer(GL_ARRAY_BUFFER, pvbo);

    // fixed coordinates for the verts, beginning compatible with Vertex
    for(int i = 0; i < nelem(pverts); i++)
    {
        memcpy(&pverts[i], &screenquad[i % 6], sizeof(Vertex));
    }

    glBufferData(GL_ARRAY_BUFFER, sizeof(pverts), pverts, GL_DYNAMIC_DRAW);
}

void
keydown(SDL_Keysym keysym)
{
    if(keysym.scancode == SDL_SCANCODE_F11)
    {
        // We also want to constrain the mouse to the 1024x1024 Type 30 screen,
        // but setWindowMouseRect usually won't work in Linux, especially running Wayland,
        // so we hack that when we see a mouse event.
        // We don't allow full screen if a fixed size was given, that defeats the purpose.
        if( !fixedSize )
        {
            fullscreen = !fullscreen;
            SDL_SetWindowFullscreen(window, screenmodes[fullscreen]);
        }
    }

    if(keysym.scancode == SDL_SCANCODE_ESCAPE)
    {
        exit(0);
    }

    // Many of the scancodes that were here did absolutely nothing.
    switch(keysym.scancode)
    {
    case SDL_SCANCODE_UP:
        // was sizefoo, never used
        break;

    case SDL_SCANCODE_DOWN:
        // was sizefoo, never used
        break;

    case SDL_SCANCODE_LEFT:
        // was intfoo, never used
        break;

    case SDL_SCANCODE_RIGHT:
        // was intfoo, never used
        break;

    case SDL_SCANCODE_S:
        // This is now controlled by a config file setting, here just to remember it
        scalefoo = (scalefoo + 1) % 3;
        break;

    case SDL_SCANCODE_R:
        // This is now controlled by a config file setting, here just to remember it
        scalefoo = 0;
        break;

    case SDL_SCANCODE_B:
        border = !border;
        SDL_SetWindowBordered(window, (border)?SDL_TRUE:SDL_FALSE);
        setScaling();
        break;

    case SDL_SCANCODE_I:
    case SDL_SCANCODE_X:
        intensityOverride = (intensityOverride + 1) % 9;
        break;
    }
}

void
process(int frametime)
{
Point *pointP;
Point *newPointP;
int i, n, idx;

    n = 0;

    // Age the points we have.
    // Remove ones that have aged out, move down to fill the hole.
    for(i = 0; i < npoints; i++)
    {
        pointP = &points[i];
        pointP->time += frametime;

        if(pointP->time < 200000)
        {
            idx = n++;
            points[idx] = *pointP;
        }
        else
        {
            idx = -1;
        }

        indices[CONSTRAIN_INDEX(pointP->y) * 1024 + (CONSTRAIN_INDEX(pointP->x))] = idx;
    }

    npoints = n;

    /* add new points */
    for(i = 0; i < nnewpoints; i++)
    {
        newPointP = &newpoints[i];
        idx = indices[CONSTRAIN_INDEX(newPointP->y) * 1024 + CONSTRAIN_INDEX(newPointP->x)];

        if(idx < 0)
        {
            idx = npoints++;
            indices[CONSTRAIN_INDEX(newPointP->y) * 1024 + CONSTRAIN_INDEX(newPointP->x)] = idx;
        }

        pointP = &points[idx];
        pointP->x = newPointP->x;
        pointP->y = newPointP->y;
        pointP->i = newPointP->i;
        pointP->time = frametime - newPointP->time;
    }

    nnewpoints = 0;
}

void *
readthread(void *args)
{
int ncmds;
int nbytes;
int i;
int x, y, intensity, delayTime;
uint32 cmd;
uint64 time;
Point *newPointP;
uint32 cmds[READBUFSIZE];

    time = 0;
    int esc = 0;

    for(;;)
    {
        nbytes = read(netfd, cmds, sizeof(cmds));

        if(nbytes <= 0)
        {
            break;
        }

        if((nbytes % 4) != 0)
        {
            printf("yikes %d\n", nbytes), exit(1);
        }

        ncmds = nbytes / 4;

        lockData();
        for(i = 0; i < ncmds; i++)
        {
            cmd = cmds[i];
            delayTime = cmd >> 23;

            // escape for longer delays of nothing
            if(esc)
            {
                esc = 0;
                time += cmd;
            }
            else if(delayTime == 511)
            {
                esc = 1;
            }
            else
            {
                x = cmd & 01777;
                y = cmd >> 10 & 01777;
                intensity = cmd >> 20 & 7;
                time += delayTime;

                if(x || y)
                {
                    newPointP = &newpoints[nnewpoints++];
                    newPointP->x = x;
                    newPointP->y = y;
                    newPointP->i = intensity;

                    if(intensityOverride != 8)
                    {
                        newPointP->i = intensityOverride;
                    }

                    newPointP->time = time;
                }
            }
        }
        unlockData();

        // we hope draw is finished before we decide to flip again
        // 30fps should be doable
        while(time > FRAMETIME)
        {
            time -= FRAMETIME;
            lockData();
            process(FRAMETIME);
            unlockData();
        }
    }

    exit(0);
}

// For the real hardware, the Type 30 hardware would figure out if there was a hit
// at the last drawn pixel when issuing the completion pulse,
// but that's not possible here, let it be determined back in the pdp1 code.
// The window size might have been changed from the original 1024, adjust for that.
// If the size is not 1024x1024, the Type 30 display area is rescaled to fit in the size automatically.
// However, mouse x,y is relative to the window size, so will not be correct for anything other than 1024x1024.
// Both mouse coordinates are offset by the respective windowsize - 1024 if the size is > 1024,
// or scaled by 1024/size if less.
// BUT if in fullscreen mode, then the 1024x1024 area is scaled by the smaller of the new sizes, aspect
// ratio is preserved. In that case, the offset comes from the actual window size, but the scaling from
// the smaller window dimension.
// Remember, the scale factor gets smaller as the size increases, sw we want the larger of the scale factors.
void
updatepen(bool penDown, int winX, int winY)
{
int pdpx, pdpy;
int xOffset, yOffset;
float xscale, yscale, ftmp;
uint32 cmd;

    if( penDown )
    {
        // Handle the fullscreen fiddling.
        // Fullscreen keeps the aspect ratio, so the scaling is done based on the smaller window dimension.
        if( fullscreen )
        {
            xscale = (xScaling > yScaling)?xScaling:yScaling;
            yscale = (yScaling > xScaling)?yScaling:xScaling;
        }
        else
        {
            xscale = xScaling;
            yscale = yScaling;
        }

        // How much 0,0 has been logically shifted.
        // If in fullscreen, the offset needs to be applied based on the
        // window size, but scaling applied based on the scale factor.
        // If using a fixed size, then there is no offset.
        // Are all the explicit casts necessary? No, but it makes it clear what's going on.
        if( fixedSize )
        {
            xOffset = yOffset = 0;
        }
        else if( fullscreen )
        {
            xOffset = (int)((float)realxSize - 1024.0/xscale + 0.5) / 2;
            yOffset = (int)((float)realySize - 1024.0/yscale + 0.5) / 2;
        }
        else
        {
            // If the window was resized by dragging, no scaling of offset, SDL just shifted it
            xOffset = (realxSize - 1024.0) / 2;
            yOffset = (realySize - 1024.0) / 2;
        }

        pdpx = (winX - xOffset) * xscale;
        pdpy = (winY - yOffset) * yscale;
        logger(LOG_MOUSE,"Mouse scaling, xscale %f, yscale %f, xOffset %d, yOffset %d\n",
            xscale, yscale, xOffset, yOffset);

        // Here is where we constrain the mouse since the SDL stuff is not reliable.
        if( pdpy < 0 )
        {
            pdpy = 0; 
        }

        if( pdpy > 1023 )
        {
            pdpy = 1023;
        }

        if( pdpx < 0 )
        {
            pdpx = 0; 
        }

        if( pdpx > 1023 )
        {
            pdpx = 1023;
        }

        logger(LOG_MOUSE,"PDP1 mouse orig %d, %d, now %d, %d\n", winX, winY, pdpx, pdpy);

        // The original code did not properly adjust the coords from SDL to PDP1.
        // SDL has the upper left corner x,y as 0,0, ranging from 0 to 1023.
        // PDP1 is -511,511, ranging from -511 to 511 plus the PDP1 coords are 1's complement.
        pdpx -= 511;
        if( pdpx < 0 )
        {
            --pdpx;             // 1's cmpl conversion
        }

        pdpy = 511 - pdpy;
        if( pdpy < 0 )
        {
            --pdpy;             // 1's cmpl conversion
        }

        logger(LOG_MOUSE,"PDP1 1's cmpl pen coords %d, %d\n",pdpx, pdpy);
        cmd = 0xFF0 << 20;
        cmd |= (pdpx & 0x3FF) << 10;
        cmd |= (pdpy & 0x3FF);
    }
    else
    {
        cmd = 0xFF1 << 20;  // pen up cmd to host
    }

    write(netfd, &cmd, 4);
}

void
usage(char *nameP)
{
    fprintf(stderr,
        "usage: %s [-d] [-p port] [-n] [-s size] [server]\n", nameP);
    fprintf(stderr, "where:\n");
    fprintf(stderr, "-d, enable debug\n");
    fprintf(stderr, "-p port, set port to use, default is %d\n", DEFAULTPORT);
    fprintf(stderr, "-n, start with no border\n");
    fprintf(stderr, "-s size, set display size to size pixels, silently limited to screen size\n");
    fprintf(stderr, "server, hostname of server to connect to\n");
    exit(1);
}

int
main(int argc, char *argv[])
{
int i;
int running;
int port;
int opt;
bool penDown;
pthread_t th;
SDL_Event event;
char tmpstr[64];

    port = DEFAULTPORT;
    fixedSize = false;

    // Set from config first, cmd line overrides
    doLightpen = checkConfig("type30lightpen");
    if( getConfig("type30border", 0) )
    {
        border = checkConfig("type30border");
    }

    if( getConfig("type30size", tmpstr) )
    {
        i = atoi(tmpstr);
        // Ok, they want a different window size
        if( (i >= 512) && (i <= 1024) )
        {
            winSize = i;
            fixedSize = true;
        }
        else
        {
            fprintf(stderr, "Window can't be made less than 512 or larger than 1024, ignored.\n");
        }
    }

    closeConfigFile();

    while( (opt = getopt(argc, argv, "p:s:dnl")) != -1 )
    {
        switch( opt )
        {
        case 'p':
            port = atoi(optarg);
            break;

        case 'd':
            // does nothing now, use logger setting LOG_DEBUG
            break;

        case 'n':
            border = 0;     // no border
            break;

        case 's':
            i = atoi(optarg);     // screen is n * n big
            if( (i >= 512) )
            {
                winSize = i;
                fixedSize = true;
            }
            else
            {
                fprintf(stderr, "Window can't be made less than 512 or larger than 1024, ignored.\n");
            }
            break;

        default:
            usage(argv[0]);
            break;
        }
    }

    if( optind < argc )
    {
        netfd = dial(argv[optind], port);
    }
    else
    {
        netfd = serve1(port);
    }

    if( netfd < 0 )
    {
        fprintf(stderr, "Can't open port!\n");
        return(1);
    }

    penDown = false;
    fullWidth = winSize + ((border)?2*BORDER:0);
    fullHeight = winSize + ((border)?2*BORDER:0);
    logger(LOG_LIGHTPEN, "Type 30 lightpen %s\n", (doLightpen)?"enabled":"disabled");

    SDL_Init(SDL_INIT_EVERYTHING);

#ifdef GLES
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | // SDL_WINDOW_RESIZABLE |
        SDL_WINDOW_ALLOW_HIGHDPI);
    if( !border )
    {
        window_flags |= SDL_WINDOW_BORDERLESS;
    }

    window = SDL_CreateWindow("P7 sim", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        fullWidth, fullHeight, window_flags);

    if(window == nil)
    {
        fprintf(stderr, "can't create window\n");
        return(1);
    }

    i = getMaxWindowSize(window);
    if( winSize > i )
    {
        logger(LOG_SCALING, "Requested window size of %d exceeds physical size of %d.\n",
            winSize, i);
    }

    setScaling();         // compute any mouse scaling we need.
    if( doLightpen )
    {
        // We want mouse events to go out quickly
        i = 1;
        setsockopt(netfd, IPPROTO_TCP, TCP_NODELAY, &i, sizeof(i));
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1); // vsync (1 on, 0 off)

    // why not memset(indices, 0xF, sizeof(indices))?
    for(int i = 0; i < 1024 * 1024; i++)
    {
        indices[i] = -1;
    }

    gladLoadGLES2Loader((GLADloadproc)SDL_GL_GetProcAddress);

    initGL();

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    init_synch();
    pthread_create(&th, nil, readthread, nil);
    int cursortimer = 0;
    SDL_ShowCursor(SDL_DISABLE);

    running = 1;

    while(running)
    {
        while(SDL_PollEvent(&event))
        {
            switch(event.type)
            {
            case SDL_TEXTINPUT:
                break;

            case SDL_KEYDOWN:
                keydown(event.key.keysym);
                break;

            case SDL_KEYUP:
                break;

            case SDL_MOUSEMOTION:
                SDL_ShowCursor(SDL_ENABLE);
                cursortimer = 50;

                if( doLightpen && penDown )
                {
                    SDL_GetMouseState(&penx, &peny);
                    updatepen(true, penx, peny);
                    logger(LOG_LIGHTPEN, "Type 30 lightpen moved to x %d y %d\n", penx, peny);
                }
                break;

            case SDL_MOUSEBUTTONDOWN:
                if( event.button.button == 1 )
                {
                    penDown = true;

                    if( doLightpen )
                    {
                        SDL_GetMouseState(&penx, &peny);
                        updatepen(true, penx, peny);
                        logger(LOG_LIGHTPEN, "Type 30 lightpen down\n");
                    }
                }
                break;

            case SDL_MOUSEBUTTONUP:
                if(event.button.button == 1)
                {
                    penDown = false;
                    updatepen(false, 0, 0);
                    logger(LOG_LIGHTPEN, "Type 30 lightpen up\n");
                }
                break;

            case SDL_QUIT:
                running = 0;
                break;

            case SDL_WINDOWEVENT:
                switch(event.window.event)
                {
                case SDL_WINDOWEVENT_CLOSE:
                    running = 0;
                    break;

                case SDL_WINDOWEVENT_SIZE_CHANGED:
                    logger(LOG_SCALING, "window size changed\n");
                    setScaling();
                    break;

                case SDL_WINDOWEVENT_MOVED:
                case SDL_WINDOWEVENT_ENTER:
                case SDL_WINDOWEVENT_LEAVE:
                case SDL_WINDOWEVENT_FOCUS_GAINED:
                case SDL_WINDOWEVENT_FOCUS_LOST:
#ifdef SDL_WINDOWEVENT_TAKE_FOCUS
                case SDL_WINDOWEVENT_TAKE_FOCUS:
#endif
                    break;
                }
            }
        }

        draw();

        if( (cursortimer > 0) && (--cursortimer == 0) )
        {
            SDL_ShowCursor(SDL_DISABLE);
        }

        usleep(FRAMETIME);
    }

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

// Get the current window size and compute scaling factors, f = 1024/size.
// Thus, a bigger window gives a smaller factor, it's the 1024 size relative to the window size.
void
setScaling()
{
    // We keep the explicit size setting if given
    SDL_GetWindowSize(window, &realxSize, &realySize);
    logger(LOG_SCALING, "Set scaling, fullscreen %d, realxSize %d, realySize %d\n", fullscreen, realxSize, realySize);

    if( border && !fullscreen )
    {
        realxSize -= BORDER * 2;
        realySize -= BORDER * 2;
        logger(LOG_SCALING, "Set scaling, border, now realxSize %d, realySize %d\n", realxSize, realySize);
    }

    if( realxSize != 1024 )
    {
        xScaling = 1024.0 / (float)realxSize;
    }
    else
    {
        xScaling = 1.0;
    }

    if( realySize != 1024 )
    {
        yScaling = 1024.0 / (float)realySize;
    }
    else
    {
        yScaling = 1.0;
    }

    logger(LOG_SCALING, "Set scaling, fixedSize %d, xScaling %f, yScaling %f\n", fixedSize, xScaling, yScaling);
}

int
getMaxWindowSize(SDL_Window *windowP)
{
int idx;
SDL_DisplayMode display;
        
    if( (idx = SDL_GetWindowDisplayIndex(windowP)) < 0 )
    {
        logger(LOG_SDL, "SDL_GetWindowDisplayIndex failed, %s\n", SDL_GetError());
        return(1024);
    }

    if( SDL_GetDesktopDisplayMode(idx, &display) )
    {
        logger(LOG_SDL, "SDL_GetDesktopDisplayMode failed, %s\n", SDL_GetError());
        return(1024);
    }

    // We limit to the smaller dimension of the display size.
    logger(LOG_SDL, "SDL_GetDesktopDisplayMode says width %d height %d\n", display.w, display.h);
    return( (display.w < display.h)?display.w:display.h );
}
