/*
 * Audio output for the pidp-1, built around Peter Samson's music program. The original
 * implementation sent the raw pf1-4 bits straight to SDL2. This version filters each of the
 * four channels through its own IIR low-pass (lowpass.c) before downmixing to stereo, since the
 * real hardware's RC low-pass filters on the bit outputs shape the sound substantially.
 *
 * 21-Jun-2026 wje (Claude) - fix svc_audio()'s overflow detector. It compared an int16_t against
 *    32768, a value int16_t cannot hold, so the check could never fire.
 *
 * 21-Jun-2026 wje (Claude) - add SDL3 support, selected at build time by sdl3.h, which the
 *    Makefile generates. SDL2's API queues raw samples straight to a device.
 *    SDL3's API instead binds an SDL_AudioStream to a device and pushes samples into the stream.
 *    Under SDL3 tuning can be used; SDL2 does not have the ability and the tuning is ignored.
*/

#include <stdbool.h>

#include "common.h"
#include "pdp1.h"
#include "lowpass.h"

#if defined(__has_include)
#if __has_include("sdl3.h")
#include "sdl3.h"
#endif
#endif

#ifdef HAVE_SDL3
#include <SDL3/SDL.h>
typedef SDL_AudioStream *AudioHandle;
#define AUDIO_HANDLE_INVALID NULL
#else
#include <SDL2/SDL.h>
typedef SDL_AudioDeviceID AudioHandle;
#define AUDIO_HANDLE_INVALID 0
#endif

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

// And output scaling.
// This is only the pre-config fallback used in the brief window before loadConfigFile() runs, or
// if a deployment somehow has no gain= line and no config file at all. The real operating default
// is set in configuration.c (Configuration.gain) and in pidp1.config.example, both 0.95.
// Warning - SDL will clip if you set the gain too high. You'll have to experiment.
#define MIXGAIN 0.5

static AudioHandle dev;

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

// The six functions below are the only places SDL2 and SDL3 differ. Everything else in this
// file calls these and is otherwise identical between the two builds.

// Opens the default playback device at the given sample rate, stereo, signed 16-bit.
// Returns AUDIO_HANDLE_INVALID on failure.
static AudioHandle
audioOpenDevice(int rate)
{
#ifdef HAVE_SDL3
SDL_AudioSpec spec = { .format = SDL_AUDIO_S16, .channels = 2, .freq = rate };

    return( SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nil, nil) );
#else
SDL_AudioSpec spec;

    memset(&spec, 0, sizeof(spec));
    spec.freq = rate;
    spec.format = AUDIO_S16;
    spec.channels = 2;
    spec.samples = 256;            // SDL2's buffer size hint. SDL3 has no equivalent field.
    spec.callback = nil;
    return( SDL_OpenAudioDevice(nil, 0, &spec, nil, 0) );
#endif
}

// Releases a handle opened by audioOpenDevice().
static void
audioCloseDevice(AudioHandle h)
{
#ifdef HAVE_SDL3
    SDL_DestroyAudioStream(h);
#else
    SDL_CloseAudioDevice(h);
#endif
}

// Pauses or resumes playback on an open handle.
static void
audioPauseDevice(AudioHandle h, bool pause)
{
#ifdef HAVE_SDL3
    if( pause )
    {
        SDL_PauseAudioStreamDevice(h);
    }
    else
    {
        SDL_ResumeAudioStreamDevice(h);
    }
#else
    SDL_PauseAudioDevice(h, pause);
#endif
}

// Discards any audio queued but not yet played.
static void
audioClearQueue(AudioHandle h)
{
#ifdef HAVE_SDL3
    SDL_ClearAudioStream(h);
#else
    SDL_ClearQueuedAudio(h);
#endif
}

// Queues raw PCM samples for playback. bytes is the size of data in bytes.
static void
audioQueueSamples(AudioHandle h, const void *data, uint32_t bytes)
{
#ifdef HAVE_SDL3
    SDL_PutAudioStreamData(h, data, (int)bytes);
#else
    SDL_QueueAudio(h, data, bytes);
#endif
}

// Sets the playback pitch ratio. 1.0 is normal pitch.
// Has no effect under SDL2, which has no equivalent concept on a plain queued device.
static void
audioApplyTuning(AudioHandle h, float ratio)
{
#ifdef HAVE_SDL3
    if( h )
    {
        SDL_SetAudioStreamFrequencyRatio(h, ratio);
    }
#endif
}

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
    dev = audioOpenDevice(sampleRate);
    audioApplyTuning(dev, tuning);   // re-apply in case it was set before this open, or on reopen
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
    if( (dev == AUDIO_HANDLE_INVALID) || !isInitialized )
    {
        return;
    }

    audioPauseDevice(dev, true);
    audioClearQueue(dev);
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

    audioClearQueue(dev);                   // clean things up, svc_audio() will unpause
    nsamples = 0;
    nexttime = 0;
    isStopped = false;
    // Start playing.
    audioPauseDevice(dev, false);
}

void
svc_audio(PDP1 *pdp)
{
int16_t scaledMix1, scaledMix2;
float chan1, chan2, chan3, chan4;
float mix1, mix2;
float gainedMix1, gainedMix2;  // gain applied, still float, not yet clamped or narrowed
int peak1, peak2;              // the actual peak seen this sample, for overflow reporting

int16_t buf[2];  // our converted result

    if( (dev == AUDIO_HANDLE_INVALID) || (++nexttime < SAMPLE_TIME(sampleRate)) || !isInitialized || isStopped )
    {
        return;
    }

    if( dev && rateChanged )
    {
        audioPauseDevice(dev, true);
        audioCloseDevice(dev);
        dev = AUDIO_HANDLE_INVALID;
        openAudio();
        audioPauseDevice(dev, isStopped);
        rateChanged = false;
    }

    nexttime = 0;
    ++nsamples;

    // filter each channel
    chan1 = lowPassFilter(&voice1,(pdp->pf & 0x20)?HIVAL:LOWVAL);
    chan2 = lowPassFilter(&voice2,(pdp->pf & 0x10)?HIVAL:LOWVAL);
    chan3 = lowPassFilter(&voice3,(pdp->pf & 0x08)?HIVAL:LOWVAL);
    chan4 = lowPassFilter(&voice4,(pdp->pf & 0x04)?HIVAL:LOWVAL);

    // Downmix quad to stereo, map to s16.
    // Use 0.50 because we are combining 2 channels.
    mix1 = mixSamples(chan1, chan2, 0.50) * 32767.0;
    mix2 = mixSamples(chan3, chan4, 0.50) * 32767.0;

    // Apply the adjustable gain here, in float, before any clamping or narrowing happens.
    gainedMix1 = mix1 * mixerGain;
    gainedMix2 = mix2 * mixerGain;
    peak1 = (int)gainedMix1;
    peak2 = (int)gainedMix2;

    // Accumulate some statistics for param setting.
    if( (gainedMix1 > 32767.0) || (gainedMix2 > 32767.0) )
    {
        ++overflows;
        if( peak1 > posOverflow )
        {
            posOverflow = peak1;
        }

        if( peak2 > posOverflow )
        {
            posOverflow = peak2;
        }
    }

    if( (gainedMix1 < -32768.0) || (gainedMix2 < -32768.0) )
    {
        ++overflows;
        if( peak1 < negOverflow )
        {
            negOverflow = peak1;
        }

        if( peak2 < negOverflow )
        {
            negOverflow = peak2;
        }
    }

    // Clamp before narrowing so a hot sample produces a clean clip in the queued audio.
    if( gainedMix1 > 32767.0 )
    {
        gainedMix1 = 32767.0;
    }
    else if( gainedMix1 < -32768.0 )
    {
        gainedMix1 = -32768.0;
    }

    if( gainedMix2 > 32767.0 )
    {
        gainedMix2 = 32767.0;
    }
    else if( gainedMix2 < -32768.0 )
    {
        gainedMix2 = -32768.0;
    }

    scaledMix1 = (int16_t)gainedMix1;
    scaledMix2 = (int16_t)gainedMix2;

    buf[0] = scaledMix1;
    buf[1] = scaledMix2;

    audioQueueSamples(dev, buf, sizeof(buf));
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

// 1.0 is no tuning. Greater than 1.0 raises pitch. Less than 1.0 lowers pitch.
// This takes effect immediately when built with SDL3.
// SDL2 has no/ pitch-ratio concept on a plain queued device, so under SDL2 this is saved but ignored.
void
setAudioTuning(float newTuning)
{
    if( newTuning > 0.0 )
    {
        tuning = newTuning;
        audioApplyTuning(dev, tuning);
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
