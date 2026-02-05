/*
 * This is the hardware panel driver for the pidp-1.
 * It uses data set by the emulator in a shared memory segment to update the panel lights
 * and to read the switches.
 *
 * Original version: Angelo Papenhoff (aap)
 * Modified by: Bill Ezell (wje) to reduce load and improve light behavior
 *
 * wje 26-Jan-26 - initial work, reformat (sorry), add timing measurement, tweak some delays and counts
 *
*/
#include "common.h"
#include "pinctrl/gpiolib.h"
#include "panel_pidp1.h"
#include <math.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

// define this to get the loop timing for the 2 main loops
#define TIMINGS

int ADDR[] = {4, 17, 27, 22};
int COLUMNS[] = {26, 19, 13, 6, 5, 11, 9, 10, 18, 23, 24, 25, 8, 7, 12, 16, 20, 21 };

typedef struct PanelLamps PanelLamps;
struct PanelLamps
{
    u8 lamps[10][18];
    Panel *p;
};

void
inRow(void)
{
    for(int i = 0; i < nelem(COLUMNS); i++)
    {
        gpio_set_fsel(COLUMNS[i], GPIO_FSEL_INPUT);
    }
}

void
outRow(void)
{
    for(int i = 0; i < nelem(COLUMNS); i++)
    {
        gpio_set_fsel(COLUMNS[i], GPIO_FSEL_OUTPUT);
    }
}

void setPin(int p, int val)
{
    gpio_set_drive(p, val ? DRIVE_HIGH : DRIVE_LOW);
}

int getPin(int p)
{
    return(gpio_get_level(p));
}

void
setRow(int l)
{
    for(int i = 0; i < nelem(COLUMNS); i++)
    {
        setPin(COLUMNS[i], (l >> i) & 1);
    }
}

void
setAddr(int a)
{
    for(int i = 0; i < nelem(ADDR); i++)
    {
        setPin(ADDR[i], (a >> i) & 1);
    }
}

#include "pwmtab.inc"

// resolution ~50-150ns
void
xsleep(u64 dt)
{
    u64 t1 = gettime();
    u64 t2 = gettime();

    while(t1 + dt > t2)
    {
        t2 = gettime();
    }
}

// calculate exponential delays for every phase
// this could be done a lot better...
u32 phase_delays[31];
void
init_delays(void)
{
    float base = 1.3f;

    for(int i = 0; i < 31; i++)
    {
        phase_delays[i] = pow(base, i) * 30;
    }
}

void
lightRow(int a, u8 *l)
{
    setRow(~0);
    setAddr(a);
    usleep(100);

    for(int phase = 0; phase < 31; phase++)
    {
        for(int i = 0; i < nelem(COLUMNS); i++)
        {
            setPin(COLUMNS[i], !(phase < l[i]));
        }

// The following used to be nsleep(1) followed by xsleep(3000), with the comment 'allowing syscalls takes too long'.
// But, nsleep() calls nanosleep(), which allows a reschedule. So, the comment made no sense.
// Replaced with just nsleep().
        nsleep(phase_delays[phase]);
    }

    setRow(~0);
    setAddr(8);
    usleep(100);
}

u32
readRow(int a)
{
    setAddr(a);
    usleep(10);
    int sw = 0777777;

    for(int i = 0; i < nelem(COLUMNS); i++)
    {
        if(getPin(COLUMNS[i]))
        {
            sw &= ~(1 << i);
        }
    }

    setAddr(8);
    usleep(100);
    return(sw);
}

void
setLights(PanelLamps *p)
{
    outRow();
    lightRow(0, p->lamps[0]);
    lightRow(1, p->lamps[1]);
    lightRow(2, p->lamps[2]);
    lightRow(3, p->lamps[3]);
    lightRow(4, p->lamps[4]);
    lightRow(5, p->lamps[5]);
    lightRow(6, p->lamps[6]);

    // IO panel
    lightRow(12, p->lamps[7]);
    lightRow(13, p->lamps[8]);
    lightRow(14, p->lamps[9]);
}

void
readSwitches(Panel *p)
{
    static u32 cycle = 0;

    inRow();
    int i = (cycle++) % 4;
    (&p->sw0)[i] = readRow(8 + i);
}

void
countRow(u32 *on, u32 bits)
{
    for(int i = 0; i < 18; i++)
    {
        on[i] += (bits >> i) & 1;
    }
}

//float fall = 0.995f;
float fall = 0.990f;
//float rise = 0.012f;
//float rise = 0.040f;
float rise = 0.030f;
float power = 1.0f;

float map(float x, float x1, float x2, float y1, float y2)
{
    float t = (x - x1) / (x2 - x1) * (y2 - y1) + y1;
    return(t < y1 ? y1 : t > y2 ? y2 : t);
}

#ifdef TIMINGS
u64 decayDelta, decayLoopCount;
#endif

// The following 2 values greatly affect processor loading.
// The original did a non-rescheduling delay which really chewed up processing time.
// Switching to a usleep lets a reschedule occur which works fine and dramatically reduces cpu load.
//
// Setting NSAMPLES to 1K and SAMPLEDELAY to 3 usecs works well.

// The number of times the light array from shared mem is scanned per cycle, originally 10K
#define NSAMPLES 1000

// The delay between each light array scan, usecs
#define SAMPLEDELAY 3

void *
lampthread(void *arg)
{
#ifdef TIMINGS
    u64 lastTime, currentTime;
#endif

PanelLamps *p = (PanelLamps*)arg;
Panel cur;
u32 on[10][18];
float intensity[10][18];
u64 now, prev;
float dt;

    memset(intensity, 0, sizeof(intensity));
#ifdef TIMINGS
    lastTime = now = gettime();
#endif

    for(;;)
    {
        memset(on, 0, sizeof(on));

        for(int i = 0; i < NSAMPLES; i++)
        {
            cur = *p->p;
            countRow(on[0], cur.lights0);
            countRow(on[1], cur.lights1);
            countRow(on[2], cur.lights2);
            countRow(on[3], cur.lights3);
            countRow(on[4], cur.lights4);
            countRow(on[5], cur.lights5);
            countRow(on[6], cur.lights6);
            countRow(on[7], cur.lights7);
            countRow(on[8], cur.lights8);
            countRow(on[9], cur.lights9);
            usleep(3);
        }

#ifdef TIMINGS
        prev = now;
        now = gettime();
        dt = (now - prev) / (1000.0f * 1000.0f);
#endif

        for(int i = 0; i < 10; i++)
        {
            for(int j = 0; j < 18; j++)
            {
                float targ = (float)on[i][j] / NSAMPLES;

                if(targ >= intensity[i][j])
                {
                    float t = powf(1.0f - rise, dt);
                    intensity[i][j] = intensity[i][j] * t + targ * (1 - t);
                }
                else
                {
                    float t = powf(fall, dt);
                    intensity[i][j] = intensity[i][j] * t + targ * (1 - t);
                }

                float l = intensity[i][j];
                int in = map(powf(l, power), 0.1f, 1.0f, 0.0f, 31.5f);
                p->lamps[i][j] = in;
            }
        }

#ifdef TIMINGS
        currentTime = gettime();
        decayDelta += (currentTime - lastTime) / 1000;  // just usecs
        lastTime = currentTime;
        ++decayLoopCount;
#endif
    }
}

volatile int doexit;
void*
panelthread(void *arg)
{
#ifdef TIMINGS
    u64 startTime, lastTime, now, delta, elapsed, loopCount;
#endif

pthread_t th;
PanelLamps panel;
struct sched_param sp;

    memset(&panel, 0, sizeof(panel));
    panel.p = (Panel*)arg;
    pthread_create(&th, nil, lampthread, &panel);

    sp.sched_priority = 99;  // not high, just above the minimum of 1
    int rt = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
    printf("realtime thread: %s\n", rt ? "yes" : "no");

    init_delays();

#ifdef TIMINGS
    startTime = lastTime = gettime();
    delta = loopCount = 0;
#endif

    while(!doexit)
    {
        setLights(&panel);
        readSwitches(panel.p);

#ifdef TIMINGS
        now = gettime();
        delta += (now - lastTime) / 1000;   // just usecs
        lastTime = now;
        ++loopCount;
#endif
    }

#ifdef TIMINGS
    elapsed = (gettime() - startTime) / 1000;   // just usecs
    delta /= loopCount; // now per-loop avg
    printf("Avg main loop time %lu usec over %lu cycles, elapsed time %lu usec, %.2f percent of elapsed time.\n",
        delta, loopCount, elapsed, ((float)(loopCount * delta) / (float)elapsed) * 100.0);

    decayDelta /= decayLoopCount; // now per-loop avg
    printf("Avg decay loop time %lu usec over %lu cycles, elapsed time %lu usec, %.2f percent of elapsed time.\n",
        decayDelta, decayLoopCount, elapsed,
        ((float)(decayDelta * decayLoopCount) / (float)elapsed) * 100.0);
#endif

    setAddr(8);
    inRow();
    exit(0);
}

void
sighandler(int sig)
{
    doexit = 1;
}

int
initGPIO(void)
{
    int ngpio = gpiolib_init();

    if(ngpio <= 0)
    {
        return(1);
    }

    if(gpiolib_mmap())
    {
        return(1);
    }

    for(int i = 0; i < nelem(ADDR); i++)
    {
        gpio_set_fsel(ADDR[i], GPIO_FSEL_OUTPUT);
    }

    for(int i = 0; i < nelem(COLUMNS); i++)
    {
        gpio_set_pull(COLUMNS[i], PULL_UP);
    }

    inRow();
    setAddr(8);

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    return(0);
}

int
main(int argc, char *argv[])
{
    Panel *p;

    p = createseg("/tmp/pdp1_panel", sizeof(Panel));

    if(p == nil)
    {
        return(1);
    }

    if(initGPIO())
    {
        return(1);
    }

    panelthread(p);

    return(0);      // can't happen
}
