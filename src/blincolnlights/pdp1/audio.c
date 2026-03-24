#include <stdbool.h>

#include "common.h"
#include "pdp1.h"
#include "lowpass.h"

#include <SDL2/SDL.h>

#define SAMPLE_TIME(rate) (200000/(rate))    // scheduling time for pidp1's timing loop in cycles
#define PRELOAD 64                           // number of samples to accumulate in SDL buffer before playing
#define MINSAMPLES (16 * 2 * sizeof(float))  // number of sample bytes before we send them out

// The values we use for the square wave
#define HIVAL   1.0
#define LOWVAL  -HIVAL

// Filter settings
#define ALPHA 0.20                           // initial value, generally works well

// A FILTERGAIN of 0.0 is the same as 1.0, but avoids one floating multiply per cycle.
// Normaally, use 0.0 and adjust the gain with MIXGAIN.
#define FILTERGAIN 0.0

// And output scaling
// Warning - SDL will clip if you set the gain too high, you'll have to experiment.
#define MIXGAIN 0.5                         // works with the default alpha

static SDL_AudioDeviceID dev;

static int nsamples;
static int overflows;
static int negOverflow;
static int posOverflow;
static u64 nexttime;
static bool isStopped = true;
static bool isInitialized = false;
static bool rateChanged = false;
static int sampleRate = 22000;

// Set default values
static float mixerGain = MIXGAIN;
static float tuning = 1.0;

// We set initial values for alpha
static FilterSpec voice1 = {.alpha = ALPHA};
static FilterSpec voice2 = {.alpha = ALPHA};
static FilterSpec voice3 = {.alpha = ALPHA};
static FilterSpec voice4 = {.alpha = ALPHA};

static void openAudio(void);

void
initaudio(void)
{
    if( isInitialized )
    {
        return;
    }

    SDL_Init(SDL_INIT_AUDIO);

    // Be careful with the gain, SDL will clip if the sample value sent to it is outside the range of -1.0 to 1.0.
    // HIVAL, LOWVAL are the maximum ranges for SDL input, so the gain should generally not be greater than 1.
    initializeFilter(&voice1, FILTERGAIN, LOWVAL);
    initializeFilter(&voice2, FILTERGAIN, LOWVAL);
    initializeFilter(&voice3, FILTERGAIN, LOWVAL);
    initializeFilter(&voice4, FILTERGAIN, LOWVAL);

    openAudio();

    isInitialized = true;
    isStopped = true;
}

static void
openAudio()
{
SDL_AudioSpec spec;

    memset(&spec, 0, sizeof(spec));
    spec.freq = sampleRate;
    spec.format = AUDIO_S16;
    spec.channels = 2;
    spec.samples = 256;            // SDL's buffer size
    spec.callback = nil;
    dev = SDL_OpenAudioDevice(nil, 0, &spec, nil, 0);
    overflows = 0;
    negOverflow = 0;
    posOverflow = 0;
}

int
isAudioInitialized()
{
    return( isInitialized );
}

void
startaudio(void)
{
    if( !isAudioInitialized() )
    {
        initaudio();
    }
    
    continueaudio();
}

void
stopaudio(void)
{
    if( (dev == 0) || !isInitialized )
    {
        return;
    }

    SDL_PauseAudioDevice(dev, 1);
    SDL_ClearQueuedAudio(dev);
    nsamples = 0;
    nexttime = 0;
    isStopped = true;
}

void
continueaudio(void)
{
    if( (dev == 0) || !isStopped || !isInitialized )
    {
        return;
    }

    SDL_ClearQueuedAudio(dev);              // clean things up, svc_audio() will unpause
    nsamples = 0;
    nexttime = 0;
    isStopped = false;
    // Start playing
    SDL_PauseAudioDevice(dev, 0);
}

void
svc_audio(PDP1 *pdp)
{
int i;
int16_t buf[2];  // and our converted result
float chan1, chan2, chan3, chan4;

    if( (dev == 0) || (++nexttime < SAMPLE_TIME(sampleRate)) || !isInitialized || isStopped )
    {
        return;
    }

    if( dev && rateChanged )
    {
        SDL_PauseAudioDevice(dev, 1);
        SDL_CloseAudioDevice(dev);
        dev = 0;
        openAudio();
        SDL_PauseAudioDevice(dev, isStopped);
        rateChanged = false;
    }

    nexttime = 0;
    ++nsamples;

    // filter each channel
    chan1 = lowPassFilter(&voice1,(pdp->pf & 0x20)?HIVAL:LOWVAL);
    chan2 = lowPassFilter(&voice2,(pdp->pf & 0x10)?HIVAL:LOWVAL);
    chan3 = lowPassFilter(&voice3,(pdp->pf & 0x08)?HIVAL:LOWVAL);
    chan4 = lowPassFilter(&voice4,(pdp->pf & 0x04)?HIVAL:LOWVAL);

    // and downmix quad to stereo, map to s16
    i = (int)(mixSamples(chan1, chan2, mixerGain) * 32767.0);
    // Accumulate some statistics for param setting
    if( i > 32768 )
    {
        ++overflows;
        if(i > posOverflow )
        {
            posOverflow = i;
        }
    }

    if( i < -32767 )
    {
        ++overflows;
        if(i < negOverflow )
        {
            negOverflow = i;
        }
    }

    buf[0] = (int16_t)i;

    i = (int)(mixSamples(chan3, chan4, mixerGain) * 32767.0);
    if( i > 32768 )
    {
        ++overflows;
        if(i > posOverflow )
        {
            posOverflow = i;
        }
    }

    if( i < -32767 )
    {
        ++overflows;
        if(i < negOverflow )
        {
            negOverflow = i;
        }
    }

    buf[1] = (int16_t)i;

    SDL_QueueAudio(dev, buf, sizeof(buf));
}

// Set the sampling rate for SDB.
// Oversampling is ok.
// Requires a teardown and reopen.
void
setSampleRate(int perSec)
{
    sampleRate = perSec;
    rateChanged = true;
}

// Get the sampling rate for SDB.
// Oversampling is ok.
int
getSampleRate()
{
    return(sampleRate);
}

// Set all filters to the same alpha
void
setFilterAlpha(float newAlpha)
{
float alpha;

    alpha = boundValue(newAlpha);
    voice1.alpha = alpha;
    voice2.alpha = alpha;
    voice3.alpha = alpha;
    voice4.alpha = alpha;
}

// Individual channel filter adjustment
void
setFilter1Alpha(float newAlpha)
{
float alpha;

    alpha = boundValue(newAlpha);
    voice1.alpha = alpha;
}

void
setFilter2Alpha(float newAlpha)
{
float alpha;

    alpha = boundValue(newAlpha);
    voice2.alpha = alpha;
}

void
setFilter3Alpha(float newAlpha)
{
float alpha;

    alpha = boundValue(newAlpha);
    voice3.alpha = alpha;
}

void
setFilter4Alpha(float newAlpha)
{
float alpha;

    alpha = boundValue(newAlpha);
    voice4.alpha = alpha;
}

float
getFilter1Alpha()
{
    return( voice1.alpha );
}

float
getFilter2Alpha()
{
    return( voice2.alpha );
}

float
getFilter3Alpha()
{
    return( voice3.alpha );
}

float
getFilter4Alpha()
{
    return( voice4.alpha );
}

void
setMixerGain(float newGain)
{
    if( newGain < 0.0 )
    {
        newGain = 0.0;          // negative is useless
    }

    mixerGain = newGain;
}

float
getMixerGain()
{
    return( mixerGain );
}

// 1.0 is no tuning, >1.0 raises pitch, <1.0 lowers pitch
// SDL2 doesn't have a tuning offset, so this doesn't actually do anything.
void
setAudioTuning(float newTuning)
{
    if( newTuning > 0.0 )
    {
        tuning = newTuning;
    }
}

float
getAudioTuning()
{
    return( tuning );
}

// Expects an int[2], returns current overflow count, if rsltP is not null,
// puts max max value seen, min value seen in the array and resets the values.
int
getOverflowData(int *rsltP)
{
int i;

    i = overflows;

    if( rsltP )
    {
        *rsltP++ = posOverflow;
        *rsltP++ = negOverflow;
        *rsltP = nsamples;
    }

    nsamples = overflows = posOverflow = negOverflow = 0;
    return( i );
}
