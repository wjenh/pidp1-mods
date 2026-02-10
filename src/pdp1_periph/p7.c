/*
 * This was written originally by Angelo Papenhoff, aap.
 * It has been modified by Bill Ezell, wje, pdp1@quackers.net to add:
 * Lightpen control
 * Amd, make it readable. :)
 *
 * It uses the One True Formatting Style, keep it.
 * The formatting is based on research done at Stanford many years ago that determined the major causes
 * of coding errors, the formatting reduced that. It works.
 *
 * wje 07-Jan-26 break from original repo, now independent. Initial reformatting. lightpen support.
 * wje 08-Feb-26 update with new lightlen code.
 *
*/

#include <stdbool.h>
#define DO_LOGGING
#include "logger.h"
#define LOG_LIGHTPEN 1

#ifdef UNITY_BUILD
#define LIGHTPEN

GLuint pvbo;
GLint point_program, excite_program, combine_program;
GLuint whiteTex, yellowTex[2];
GLuint whiteFBO, yellowFBO[2];

int penx;
int peny;
int pendown;

int flip;
typedef struct Point Point;
struct Point
{
    int x, y;
    int i;
    int time;
};

int indices[1024 * 1024];
Point newpoints[1024 * 1024];
int nnewpoints;
Point points[1024 * 1024];
int npoints;
float pverts[4 * 10000];

struct
{
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    int canprocess, candraw;
} synch;

void init_synch(void)
{
    pthread_mutex_init(&synch.mutex, nil);
    pthread_cond_init(&synch.wake, nil);
    synch.canprocess = 1;
    synch.candraw = 0;
}

void signal_process(void)
{
    pthread_mutex_lock(&synch.mutex);
    synch.canprocess = 1;
    pthread_cond_signal(&synch.wake);
    pthread_mutex_unlock(&synch.mutex);
}

void wait_canprocess(void)
{
    pthread_mutex_lock(&synch.mutex);

    while(!synch.canprocess)
    {
        pthread_cond_wait(&synch.wake, &synch.mutex);
    }

    synch.canprocess = 0;
    pthread_mutex_unlock(&synch.mutex);
}

void signal_draw(void)
{
    pthread_mutex_lock(&synch.mutex);
    synch.candraw = 1;
    pthread_mutex_unlock(&synch.mutex);
}

int candraw(void)
{
    int ret;
    pthread_mutex_lock(&synch.mutex);
    ret = synch.candraw;
    synch.candraw = 0;
    pthread_mutex_unlock(&synch.mutex);
    return(ret);
}

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

const char *point_vs_src =
    glslheader
    "VSIN vec4 in_pos;\n"
    "VSOUT float v_fade;\n"
    "VSOUT float v_intensity;\n"
    "uniform float u_pointSize;\n"
    "void main()\n"
    "{\n"
    "	v_fade = pow(0.5, in_pos.z);\n"
    "	float sz = mix(0.0018, 0.0055, in_pos.w)*1024.0/2.0;\n"
    "	v_intensity = mix(0.25, 1.0, in_pos.w);\n"
    "	gl_Position = vec4((in_pos.xy / 512.0) - 1.0, 0, 1);\n"
    "	gl_PointSize = u_pointSize*sz;\n"
    "}\n";

const char *point_fs_src =
    glslheader
    outcolor
    "FSIN float v_fade;\n"
    "FSIN float v_intensity;\n"
    "void main()\n"
    "{\n"
    "	float dist = length(2.0*gl_PointCoord - vec2(1));\n"
    "	float intens = clamp(1.0 - dist*dist, 0.0, 1.0)*v_intensity;\n"
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
initDisplay(void)
{
    GLint vs = compileshader(GL_VERTEX_SHADER, vs_src);
    GLint point_vs = compileshader(GL_VERTEX_SHADER, point_vs_src);
    GLint point_fs = compileshader(GL_FRAGMENT_SHADER, point_fs_src);
    GLint excite_fs = compileshader(GL_FRAGMENT_SHADER, excite_fs_src);
    GLint combine_fs = compileshader(GL_FRAGMENT_SHADER, combine_fs_src);
    point_program = linkprogram(point_fs, point_vs);
    excite_program = linkprogram(excite_fs, vs);
    combine_program = linkprogram(combine_fs, vs);

    makeFBO(&whiteFBO, &whiteTex);
    makeFBO(&yellowFBO[0], &yellowTex[0]);
    makeFBO(&yellowFBO[1], &yellowTex[1]);

    clearState();

    makeQuad();

    glGenBuffers(1, &pvbo);
}

void
drawDisplayUpdate(void)
{
    if(dbgflag)
    {
        float dt = getDeltaTime();
        float st = simtime / 1000000.0f;
        float rt = (float)realtime / SDL_GetPerformanceFrequency();
        printf("%f %d. %.2f %.2f %.2f\n", dt, npoints, st, rt, rt - st);
    }

    glViewport(0, 0, BWIDTH, BHEIGHT);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    /* draw white phosphor */
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);

    glBindFramebuffer(GL_FRAMEBUFFER, whiteFBO);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(point_program);


    float pointSize = 2.0f * WIDTH / 1024.0f;
    glUniform1f(glGetUniformLocation(point_program, "u_pointSize"), pointSize);

    int i;

    for(i = 0; i < npoints; i++)
    {
        if(i >= nelem(pverts))
        {
            break;
        }

        pverts[i * 4 + 0] = points[i].x;
        pverts[i * 4 + 1] = points[i].y;
        pverts[i * 4 + 2] = points[i].time / 50000.0f;
        pverts[i * 4 + 3] = points[i].i / 7.0f;
    }

// THREAD: signal ready to process
    signal_process();
    int stride = sizeof(float[4]);
    glBindBuffer(GL_ARRAY_BUFFER, pvbo);
    glBufferData(GL_ARRAY_BUFFER, i * stride, pverts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, stride, 0);
    glDrawArrays(GL_POINTS, 0, i);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_BLEND);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, whiteTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, yellowTex[flip]);

    /* draw and age yellow layer */
    setvbo(vbo);
    glBindFramebuffer(GL_FRAMEBUFFER, yellowFBO[!flip]);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(excite_program);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    flip = !flip;

    clearState();
}

void
drawDisplay(Region *r)
{
    setSquareRegion(r);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, whiteTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, yellowTex[flip]);

    glUseProgram(combine_program);
    setvbo(vbo);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    clearState();
}


void
process(int frmtime)
{
    Point *p;
    int i, n, idx;

    /* age */
    n = 0;

    for(i = 0; i < npoints; i++)
    {
        p = &points[i];
        p->time += frmtime;

        if(p->time < 200000)
        {
            points[idx = n++] = *p;
        }
        else
        {
            idx = -1;
        }

        indices[p->y * 1024 + p->x] = idx;
    }

    npoints = n;

    /* add new points */
    for(i = 0; i < nnewpoints; i++)
    {
        Point *np = &newpoints[i];
        idx = indices[np->y * 1024 + np->x];

        if(idx < 0)
        {
            idx = npoints++;
            indices[np->y * 1024 + np->x] = idx;
        }

        p = &points[idx];
        p->x = np->x;
        p->y = np->y;
        p->i = np->i;
        p->time = frmtime - np->time;
    }

    nnewpoints = 0;
}

void*
dispthread(void *args)
{
    uint32 cmd;
    uint32 cmds[128];
    int ncmds;
    int nbytes;
    int i;
    uint64 time;
    uint64 frmtime = 33333;
    int x, y, intensity, dt;

    uint64 realtime_start = SDL_GetPerformanceCounter();
    simtime = 0;
    realtime = realtime_start;

    time = 0;
    int esc = 0;

    for(;;)
    {
        nbytes = read(dpyfd, cmds, sizeof(cmds));

        if(nbytes <= 0)
        {
            // This seems to happen when the pdp1 isn't noticing the closed
            // connection quickly enough. shouldn't be a huge issue in practice
            fprintf(stderr, "dpy disconnected\n");
            break;
        }

        if((nbytes % 4) != 0)
        {
            printf("yikes %d\n", nbytes), exit(1);
        }

        ncmds = nbytes / 4;

        for(i = 0; i < ncmds; i++)
        {
            cmd = cmds[i];
            dt = cmd >> 23;

            // escape for longer delays of nothing
            if(esc)
            {
                esc = 0;
                time += cmd;
            }

            if(dt == 511)
            {
                esc = 1;
            }
            else
            {
                x = cmd & 01777;
                y = cmd >> 10 & 01777;
                intensity = cmd >> 20 & 7;
                time += dt;

                if(x || y)
                {
                    Point *np = &newpoints[nnewpoints++];
                    np->x = x;
                    np->y = y;
                    np->i = intensity;
                    np->time = time;
                }
            }

            // we hope draw is finished before we decide to flip again
            // 30fps should be doable
            while(time > frmtime)
            {
                time -= frmtime;
                simtime += frmtime;
                realtime = SDL_GetPerformanceCounter() - realtime_start;

// THREAD: wait here until ready
                wait_canprocess();
                process(frmtime);
// THREAD: signal ready to draw
                signal_draw();
            }
        }
    }

    SDL_Event event = { SDL_QUIT };
    SDL_PushEvent(&event);
}

#ifdef LIGHTPEN
// For the real hardware, the Type 30 hardware would figure out if there was a hit
// at the last drawn pixel when issuing the completion pulse,
// but that's not possible here, let it be determined back in the pdp1 code.
void
updatepen(bool penDown)
{
int i;
int pdpx, pdpy;
uint32 cmd;

    if( penDown )
    {
        pdpx = penx;
        pdpy = peny;

        // The original code did not properly adjust the coords from SDL to PDP1.
        // SDL has the upper left corner x,y as 0,0, PDP1 is -512,512, plus the PDP1 coords are 1's complement.
        pdpx -= 512;
        if( pdpx < 0 )
        {
            --pdpx;             // 1's cmpl conversion
        }

        pdpy = 512 - pdpy;
        if( pdpy < 0 )
        {
            --pdpy;            // 1's cmpl conversion
        }

        cmd = 0xFF0 << 20;
        cmd |= (pdpx & 0x3FF) << 10;
        cmd |= (pdpy & 0x3FF);
    }
    else
    {
        cmd = 0xFF1 << 20;  // pen up cmd to host
    }

    if( (i =write(dpyfd, &cmd, 4)) != 4 )
    {
        logger(LOG_LIGHTPEN,"lightpen write failed %d\n", i);
    }
}
#else
void
updatepen(bool penDown)
{
}
#endif

#endif
