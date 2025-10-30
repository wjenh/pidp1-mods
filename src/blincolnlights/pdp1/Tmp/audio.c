#include "common.h"
#include "pdp1.h"
#include "lowpass.h"

#include <SDL2/SDL.h>

#define SAMPLE_RATE (5714*1)          // one sample every 175 us, it's what the original did, oversample if desired
#define SAMPLE_TIME (1000000000/SAMPLE_RATE)  // one sample every 175 us, it's what the original did, oversample by 6
#define PRELOAD 64                          // number of samples to accumulate in SDL buffer before playing

// The values we use for the square wave
#define HIVAL   1.0
#define LOWVAL  -HIVAL

// Filter settings
#define ALPHA 0.10                           // initial value, generally works well

// A FILTERGAIN of 0.0 is the same as 1.0, but avoids one floating multiply per cycle.
// Normaally, use 0.0 and adjust the gain with MIXGAIN.
#define FILTERGAIN 0.0

// And output scaling
// Warning - SDL will clip any audio value <-1.0 or >1.0, so don't set the gain too high, you'll have to experiment.
#define MIXGAIN 1.5                         // works with the default alpha of 0.1

static SDL_AudioDeviceID dev;
static int nsamples;
static u64 nexttime;
static int isStopped = 1;
static int isInitialized = 0;

static float alpha = ALPHA;
static float mixerGain = MIXGAIN;

static FilterSpec voice1;
static FilterSpec voice2;
static FilterSpec voice3;
static FilterSpec voice4;

void
initaudio(void)
{
	SDL_AudioSpec spec;

    if( isInitialized )
        return;

	SDL_Init(SDL_INIT_AUDIO);

    // Be careful with the gain, SDL will clip if the sample value sent to it is outside the range of -1.0 to 1.0.
    // HIVAL, LOWVAL are the maximum ranges for SDL input, so the gain should generally not be greater than 1.
    initializeFilter(&voice1, alpha, FILTERGAIN, LOWVAL);
    initializeFilter(&voice2, alpha, FILTERGAIN, LOWVAL);
    initializeFilter(&voice3, alpha, FILTERGAIN, LOWVAL);
    initializeFilter(&voice4, alpha, FILTERGAIN, LOWVAL);

	memset(&spec, 0, sizeof(spec));
	spec.freq = SAMPLE_RATE;         // the original did one sample every 175 us, replicaate with possible oversamplng
	spec.format = AUDIO_F32;
	spec.channels = 2;
	spec.samples = 1024;            // SDL's buffer size
	spec.callback = nil;
	dev = SDL_OpenAudioDevice(nil, 0, &spec, nil, 0);

    isInitialized = 1;
    isStopped = 1;
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
		return;

	SDL_PauseAudioDevice(dev, 1);
	SDL_ClearQueuedAudio(dev);
	nsamples = 0;
	nexttime = 0;
    isStopped = 1;
}

void
continueaudio(void)
{
	if( (dev == 0) || !isStopped || !isInitialized )
		return;

	SDL_ClearQueuedAudio(dev);              // clean things up, svc_audio() will unpause
	nsamples = 0;
	nexttime = 0;
    isStopped = 0;
}

void
svc_audio(PDP1 *pdp)
{
float chan1, chan2, chan3, chan4;
float buffer[2];

	u8 s;

	if( (dev == 0) || (nexttime >= pdp->simtime) || !isInitialized || isStopped )
		return;

	if(nexttime == 0)
		nexttime = pdp->simtime + SAMPLE_TIME;
	else
		nexttime += SAMPLE_TIME;

	// queue up a reasonable number of samples, power of 2 is preferred
	if(nsamples < PRELOAD) {
		nsamples++;
	}
    else
    {
		// then start playing
        SDL_PauseAudioDevice(dev, 0);
    }

    // filter each channel
    chan1 = lowPassFilter(&voice1,(pdp->pf & 0x20)?HIVAL:LOWVAL);
    chan2 = lowPassFilter(&voice2,(pdp->pf & 0x10)?HIVAL:LOWVAL);
    chan3 = lowPassFilter(&voice3,(pdp->pf & 0x08)?HIVAL:LOWVAL);
    chan4 = lowPassFilter(&voice4,(pdp->pf & 0x04)?HIVAL:LOWVAL);
    // and downmix quad to stereo
    buffer[0] = mixSamples(chan1, chan2, mixerGain);
    buffer[1] = mixSamples(chan3, chan4, mixerGain);

	SDL_QueueAudio(dev, buffer, 2 * sizeof(float));
}

void
setFilterAlpha(float newAlpha)
{
    alpha = boundValue(newAlpha);
    voice1.alpha = alpha;
    voice2.alpha = alpha;
    voice3.alpha = alpha;
    voice4.alpha = alpha;
}

float
getFilterAlpha()
{
    return( alpha );
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
