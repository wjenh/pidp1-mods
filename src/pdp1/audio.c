/*
 * Audio output for the pidp-1, built around Peter Samson's music program. The original
 * implementation sent the raw pf1-4 bits straight to SDL2. This version puts each of the four
 * flags through its own RC low-pass (lowpass.c), as the music interface did, before downmixing to
 * stereo. The filters run on simtime: each flag change is applied at the simtime it happened and
 * each sample reads the filters at its own instant, so the sample rate neither moves the cutoffs
 * nor moves an edge.
 *
 * svc_audio() runs on the emulator thread, once per main-loop pass while the machine runs. The
 * sample clock is simtime, so the rate is exact for any sample rate and counts the extra time of
 * mul and div. SDL's queue is held near a set depth (audiodepth, in ms): it is preloaded at each
 * start and after a halt, and steered by stretching or shrinking the sample period slightly. The
 * depth is the delay between the program flags and the sound, and the longest emulator stall
 * that cannot be heard.
 *
 * 21-Jun-2026 wje (Claude) - fix svc_audio()'s overflow detector. It compared an int16_t against
 *    32768, a value int16_t cannot hold, so the check could never fire.
 *
 * 21-Jun-2026 wje (Claude) - add SDL3 support, selected at build time by sdl3.h, which the
 *    Makefile generates. SDL2's API queues raw samples straight to a device.
 *    SDL3's API instead binds an SDL_AudioStream to a device and pushes samples into the stream.
 *    Under SDL3 tuning can be used; SDL2 does not have the ability and the tuning is ignored.
 * 14-Jul-2026 wje some cleanup done
 * 25-Sep-2026 wje (Claude) - sample clock on simtime; the queue is preloaded, batched and steered
 *    to the audiodepth setting, with a once-a-second depth line when pidp1timing is on.
 * 26-Sep-2026 wje (Claude) - the filters run on simtime and are set by cutoff in Hz, defaulting to
 *    the CHM music interface's; the per-sample alphas are gone. The default rate is 48,000.
*/

#include <stdbool.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>

#include "common.h"
#include "pdp1.h"
#include "audio.h"
#include "lowpass.h"
#include "configuration.h"

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

#define FRAME_BYTES (2 * sizeof(int16_t))   // one stereo S16 sample
#define BATCH_FRAMES 64                      // per SDL call: 1.3 ms at 48,000 Hz, well under the depth

// Queue depth control. A stall of the emulator thread shorter than the depth is not heard,
// because the device plays from the queue; the depth is also how far the sound trails the lamps
// (on top of SDL's device buffer and the OS). 40 ms covers the throttle's 20 ms lag cap twice.
#define DEPTH_DEFAULT_MS 40
#define DEPTH_MIN_MS 10
#define DEPTH_MAX_MS 500
#define STEER_BAND_MS 10                     // no steering while the smoothed depth is this close
#define STEER_RATIO 0.005                    // 0.5% of the period, about 9 cents: not heard as pitch
#define STEER_SMOOTHING 32.0                 // flushes averaged, ~90 ms: rides out the device's pulls
#define CEILING_MS 250                       // at least; a queue above this is cut back to the depth

// The device's own rate is measured over windows this long. Depth noise of a millisecond over
// 10 s is 0.01%, well under any real device's error; the limit only guards against nonsense.
#define RATE_WINDOW_NS 10000000000ull
#define RATE_LIMIT 0.02

// Simtime passing between two calls with no lag-cap firing means the machine was not running
// (halted or stepped). Well above any one instruction, mul and div included.
#define REANCHOR_GAP_NS 1000000ull

#define AUDIO_TIMING_FILE "/tmp/pidp1-audiotiming.txt"

// The values we use for the square wave
#define HIVAL   1.0
#define LOWVAL  -HIVAL

// The CHM music interface (its 2023 rebuild drawing): each flag drives 5K into a capacitor, with a
// 20K pot across the capacitor, so the capacitor sees 4K. Measured capacitors, in farads.
#define CHM_R 4000.0
static const double chmC[4] = { 0.039e-6, 0.043e-6, 0.094e-6, 0.181e-6 };
#define CUTOFF_MIN_HZ 1.0
#define CUTOFF_MAX_HZ 100000.0

// And output scaling.
// This is only the pre-config fallback used in the brief window before loadConfigFile() runs, or
// if a deployment somehow has no gain= line and no config file at all. The real operating default
// is set in configuration.c (Configuration.gain) and in pidp1.config.example, both 0.95.
// Warning - SDL will clip if you set the gain too high. You'll have to experiment.
#define MIXGAIN 0.5

// The device and everything that steers its queue belong to the emulator thread. The console and
// the command port run on their own threads, so what they ask for is posted: the value is stored,
// then its generation and requestGen are bumped, and svc_audio() applies it at its next pass.
// Until isInitialized is seen, the emulator thread touches nothing here.
static AudioHandle dev;
static bool isStopped = true;
static int sampleRate = 48000;               // the open device's rate
static float tuning = 1.0;                   // the open device's tuning
static atomic_bool isInitialized;
static pthread_mutex_t initLock = PTHREAD_MUTEX_INITIALIZER;

static atomic_uint requestGen;
static unsigned appliedRequestGen;
static atomic_uint rateGen;
static unsigned appliedRateGen;
static atomic_int requestedRate = 48000;
static atomic_uint tuningGen;
static unsigned appliedTuningGen;
static _Atomic float requestedTuning = 1.0f;
static atomic_uint runGen;
static unsigned appliedRunGen;
static atomic_bool wantRun;

// Counted on the emulator thread; the overflow query reads and clears them from another.
static atomic_int nsamples;
static atomic_int overflows;
static atomic_int negOverflow;
static atomic_int posOverflow;

// The sample clock: simtime of the next sample, with the fraction of a nanosecond carried apart
// so no rate drifts. needAnchor asks the next pass to restart it and preload the queue.
static uint64_t dueNs;
static double dueFrac;
static double periodNs;
static uint64_t lastSimNs;
static long capSeen;
static bool needAnchor = true;

static int16_t batch[BATCH_FRAMES * 2];
static int batchFrames;

static int depthMs = DEPTH_DEFAULT_MS;
static int targetFrames;
static int bandFrames;
static int ceilingFrames;
static double smoothFrames;
static double steerAdj;                      // 0, or +/-STEER_RATIO while correcting
static bool tuned;                           // tuning is not 1.0: the rate is left alone

// The device's measured rate as a ratio to sampleRate, and the window it is measured over.
static double deviceRatio = 1.0;
static bool deviceMeasured;
static uint64_t winStartNs;
static long winFrames;
static double winStartDepth;
static bool winClean;

// pidp1timing: one line a second of simtime, counts since the last line.
static bool timingOn;
static FILE *timingFP;
static uint64_t timingNextNs;
static long tFrames;
static long tPreloaded;
static long tHalts;
static long tCaps;
static long tCeilings;
static long tSteered;
static long tFlushes;
static int tMinFrames = INT_MAX;
static int tMaxFrames;

// Set default values
static _Atomic float mixerGain = MIXGAIN;

// The four filters, PF1-PF4. cutoffHz is what was asked for, from any thread (the command port
// runs on its own); a setter then bumps cutoffGen, and the emulator thread applies the cutoffs at
// its next pass. Each pass costs one load and, for the flags, one compare unless something changed.
static RCFilter voice[4];
static const int voiceBit[4] = { PF_1, PF_2, PF_3, PF_4 };
#define VOICE_BITS (PF_1 | PF_2 | PF_3 | PF_4)
static _Atomic float cutoffHz[4];
static atomic_uint cutoffGen = 1;
static unsigned appliedGen;
static int lastFlags = -1;

static void openAudio(void);
static void postRequest(atomic_uint *genP);
static void applyRequests(void);
static void startPlaying(void);
static void stopPlaying(void);

// The seven functions below are the only places SDL2 and SDL3 differ. Everything else in this
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

// Returns the bytes queued and not yet played, or -1 if SDL cannot say.
static int
audioQueuedBytes(AudioHandle h)
{
#ifdef HAVE_SDL3
    return( SDL_GetAudioStreamQueued(h) );
#else
    return( (int)SDL_GetQueuedAudioSize(h) );
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

// Initializes SDL's audio and the filters, and opens the device. Any thread; SDL_Init() and the
// open can take tens of milliseconds, so they are not done on the emulator thread. That thread
// reads nothing set here until it sees isInitialized, and owns the device from then on.
void
initaudio(void)
{
    pthread_mutex_lock(&initLock);
    if( atomic_load_explicit(&isInitialized, memory_order_acquire) )
    {
        pthread_mutex_unlock(&initLock);
        return;
    }

    SDL_Init(SDL_INIT_AUDIO);

    // Be careful with the gain, SDL will clip if the sample value sent to it is outside the range of -1.0 to 1.0.
    // HIVAL, LOWVAL are the maximum ranges for SDL input, so the gain should generally not be greater than 1.
    // The first svc_audio() sets each filter's cutoff and input.
    for( int i = 0; i < 4; ++i )
    {
        rcInit(&voice[i], LOWVAL, 0);
    }
    appliedGen = 0;
    lastFlags = -1;
    isStopped = true;

    // The rate and tuning asked for so far are this open's. One asked for after these loads
    // moves its generation again, and the emulator thread applies it.
    appliedRateGen = atomic_load_explicit(&rateGen, memory_order_acquire);
    appliedTuningGen = atomic_load_explicit(&tuningGen, memory_order_acquire);
    tuning = atomic_load_explicit(&requestedTuning, memory_order_relaxed);
    openAudio();

    atomic_store_explicit(&isInitialized, true, memory_order_release);
    pthread_mutex_unlock(&initLock);
}

// Opens the device at the rate asked for and reads audiodepth and pidp1timing. configure() calls
// setSampleRate() on every load and reload of the config file, and that reopens the device here,
// so both settings follow a reload. initaudio()'s first open, then the emulator thread's.
static void
openAudio()
{
ConfigurationSettingP settingP;
int ceilingMs;

    sampleRate = atomic_load_explicit(&requestedRate, memory_order_relaxed);
    dev = audioOpenDevice(sampleRate);
    audioApplyTuning(dev, tuning);   // re-apply in case it was set before this open, or on reopen
    atomic_store_explicit(&overflows, 0, memory_order_relaxed);
    atomic_store_explicit(&negOverflow, 0, memory_order_relaxed);
    atomic_store_explicit(&posOverflow, 0, memory_order_relaxed);

    depthMs = DEPTH_DEFAULT_MS;
    if( (settingP = findConfigurationSetting(getConfiguration(), "audiodepth")) && (settingP->ivalue > 0) )
    {
        depthMs = settingP->ivalue;
    }

    if( depthMs < DEPTH_MIN_MS )
    {
        depthMs = DEPTH_MIN_MS;
    }
    else if( depthMs > DEPTH_MAX_MS )
    {
        depthMs = DEPTH_MAX_MS;
    }

    ceilingMs = (4 * depthMs);
    if( ceilingMs < CEILING_MS )
    {
        ceilingMs = CEILING_MS;
    }

    periodNs = (1000000000.0 / (double)sampleRate);
    targetFrames = ((sampleRate * depthMs) / 1000);
    bandFrames = ((sampleRate * STEER_BAND_MS) / 1000);
    ceilingFrames = ((sampleRate * ceilingMs) / 1000);

    settingP = findConfigurationSetting(getConfiguration(), "pidp1timing");
    timingOn = (settingP && settingP->onOff);

    tuned = (fabsf(tuning - 1.0f) > 0.0001f);
    deviceRatio = 1.0;
    deviceMeasured = false;
    needAnchor = true;
}

// Resets the queue control for a start from an empty queue. The device's measured rate is kept.
static void
resetQueueControl(void)
{
    batchFrames = 0;
    smoothFrames = 0.0;
    steerAdj = 0.0;
    winStartNs = 0;
    needAnchor = true;
}

int
isAudioInitialized()
{
    return( atomic_load_explicit(&isInitialized, memory_order_acquire) );
}

// Initializes the audio if it is not, and asks for the sound to start. Any thread.
void
startaudio(void)
{
    initaudio();
    continueaudio();
}

// Asks for the sound to start (on true) or stop, at the emulator thread's next svc_audio(). Any
// thread. The machine only makes sound while it runs, and main() only calls svc_audio() while
// audio is enabled, so with audio turned off the queue plays out and the device runs dry.
void
postaudio(bool on)
{
    atomic_store_explicit(&wantRun, on, memory_order_relaxed);
    postRequest(&runGen);
}

// Asks for the sound to start. Any thread.
void
continueaudio(void)
{
    postaudio(true);
}

// Stops the sound now. Emulator thread only: main() calls it every pass while the power is off.
// A start or stop asked for and not yet applied is dropped; main() asks for the sound again from
// audioEnabled when the power comes back.
void
stopaudio(void)
{
    if( !atomic_load_explicit(&isInitialized, memory_order_acquire) )
    {
        return;
    }

    appliedRunGen = atomic_load_explicit(&runGen, memory_order_acquire);
    if( !isStopped )
    {
        stopPlaying();
    }
}

// Stores nothing itself: the caller stores the value, then this bumps its generation and
// requestGen, in that order, so a pass that sees requestGen move sees the value too.
static void
postRequest(atomic_uint *genP)
{
    atomic_fetch_add_explicit(genP, 1, memory_order_release);
    atomic_fetch_add_explicit(&requestGen, 1, memory_order_release);
}

// Applies what was asked for since the last pass: the rate, which reopens the device, then the
// tuning, then a start or stop. A pass with nothing asked for costs one load. Emulator thread.
static void
applyRequests(void)
{
unsigned gen;

    gen = atomic_load_explicit(&requestGen, memory_order_acquire);
    if( gen == appliedRequestGen )
    {
        return;
    }
    appliedRequestGen = gen;

    gen = atomic_load_explicit(&rateGen, memory_order_acquire);
    if( gen != appliedRateGen )
    {
        appliedRateGen = gen;
        if( dev != AUDIO_HANDLE_INVALID )
        {
            audioPauseDevice(dev, true);
            audioCloseDevice(dev);
            dev = AUDIO_HANDLE_INVALID;
        }

        openAudio();
        resetQueueControl();
        if( dev != AUDIO_HANDLE_INVALID )
        {
            audioPauseDevice(dev, isStopped);
        }
    }

    gen = atomic_load_explicit(&tuningGen, memory_order_acquire);
    if( gen != appliedTuningGen )
    {
        appliedTuningGen = gen;
        tuning = atomic_load_explicit(&requestedTuning, memory_order_relaxed);
        tuned = (fabsf(tuning - 1.0f) > 0.0001f);
        audioApplyTuning(dev, tuning);
    }

    gen = atomic_load_explicit(&runGen, memory_order_acquire);
    if( gen != appliedRunGen )
    {
        appliedRunGen = gen;
        if( atomic_load_explicit(&wantRun, memory_order_relaxed) )
        {
            startPlaying();
        }
        else
        {
            stopPlaying();
        }
    }
}

// Starts a stopped device from an empty queue. Emulator thread.
static void
startPlaying(void)
{
    if( (dev == AUDIO_HANDLE_INVALID) || !isStopped )
    {
        return;
    }

    // The next svc_audio() preloads the queue before the program's sound.
    audioClearQueue(dev);
    atomic_store_explicit(&nsamples, 0, memory_order_relaxed);
    resetQueueControl();
    isStopped = false;
    // Start playing.
    audioPauseDevice(dev, false);
}

// Pauses the device and empties its queue. Emulator thread.
static void
stopPlaying(void)
{
    if( dev == AUDIO_HANDLE_INVALID )
    {
        return;
    }

    audioPauseDevice(dev, true);
    audioClearQueue(dev);
    atomic_store_explicit(&nsamples, 0, memory_order_relaxed);
    resetQueueControl();
    isStopped = true;
}

// Reads the filters at simtime t into one stereo frame at frameP. A t at or before the filters'
// own time reads them as they are.
static void
computeFrame(uint64_t t, int16_t *frameP)
{
int16_t scaledMix1, scaledMix2;
float chan1, chan2, chan3, chan4;
float mix1, mix2;
float gainedMix1, gainedMix2;  // gain applied, still float, not yet clamped or narrowed
int peak1, peak2;              // the actual peak seen this sample, for overflow reporting
int peakHi, peakLo;
float gain;

    atomic_fetch_add_explicit(&nsamples, 1, memory_order_relaxed);

    for( int i = 0; i < 4; ++i )
    {
        rcAdvance(&voice[i], t);
    }
    chan1 = (float)voice[0].y;
    chan2 = (float)voice[1].y;
    chan3 = (float)voice[2].y;
    chan4 = (float)voice[3].y;

    // Downmix quad to stereo, map to s16.
    // Use 0.50 because we are combining 2 channels.
    mix1 = mixSamples(chan1, chan2, 0.50) * 32767.0;
    mix2 = mixSamples(chan3, chan4, 0.50) * 32767.0;

    // Apply the adjustable gain here, in float, before any clamping or narrowing happens.
    gain = atomic_load_explicit(&mixerGain, memory_order_relaxed);
    gainedMix1 = mix1 * gain;
    gainedMix2 = mix2 * gain;
    peak1 = (int)gainedMix1;
    peak2 = (int)gainedMix2;

    // Accumulate some statistics for param setting.
    // Only this thread raises the peaks, so a plain compare and store does; a query clearing one
    // between the two can at worst leave this sample's peak in place.
    if( (gainedMix1 > 32767.0) || (gainedMix2 > 32767.0) )
    {
        atomic_fetch_add_explicit(&overflows, 1, memory_order_relaxed);
        peakHi = ((peak1 > peak2) ? peak1 : peak2);
        if( peakHi > atomic_load_explicit(&posOverflow, memory_order_relaxed) )
        {
            atomic_store_explicit(&posOverflow, peakHi, memory_order_relaxed);
        }
    }

    if( (gainedMix1 < -32768.0) || (gainedMix2 < -32768.0) )
    {
        atomic_fetch_add_explicit(&overflows, 1, memory_order_relaxed);
        peakLo = ((peak1 < peak2) ? peak1 : peak2);
        if( peakLo < atomic_load_explicit(&negOverflow, memory_order_relaxed) )
        {
            atomic_store_explicit(&negOverflow, peakLo, memory_order_relaxed);
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

    frameP[0] = scaledMix1;
    frameP[1] = scaledMix2;
}

// Hands the batch to SDL.
static void
queueBatch(void)
{
    if( batchFrames > 0 )
    {
        audioQueueSamples(dev, batch, (uint32_t)(batchFrames * FRAME_BYTES));
        batchFrames = 0;
    }
}

// Adds the frame for simtime t to the batch. Returns true when that filled it and it went to SDL.
static bool
appendFrame(uint64_t t)
{
    computeFrame(t, &batch[batchFrames * 2]);
    ++batchFrames;
    ++tFrames;
    ++winFrames;

    if( batchFrames < BATCH_FRAMES )
    {
        return( false );
    }

    queueBatch();
    return( true );
}

// Returns the frames SDL holds that have not been played; 0 if it cannot say.
static int
queuedFrames(void)
{
int bytes;

    bytes = audioQueuedBytes(dev);
    if( bytes < 0 )
    {
        return( 0 );
    }

    return( bytes / (int)FRAME_BYTES );
}

// Fills the queue to the target depth with the filters' output held as it stands at simtime t.
// Used where the sound has stopped anyway, a start or a halt, so the level it adds is not heard as
// a gap. No simtime passes in it.
static void
preload(uint64_t t)
{
int want;

    queueBatch();
    want = (targetFrames - queuedFrames());
    if( want > 0 )
    {
        tPreloaded += want;
        while( want-- > 0 )
        {
            appendFrame(t);
        }
        queueBatch();
    }

    smoothFrames = (double)targetFrames;
    steerAdj = 0.0;
    winClean = false;
}

// Once a window of simtime has passed, measures how fast the device really plays: what was
// produced, less what the queue gained. Producing at that rate keeps the pitch right, since the
// device plays the samples at its own rate, and leaves steering only the depth to correct. A
// window with a clear, preload, lag-cap firing or empty queue in it measures nothing.
static void
measureDevice(uint64_t now)
{
double seconds;
double ratio;

    if( winStartNs == 0 )
    {
        winStartNs = now;
        winFrames = 0;
        winStartDepth = smoothFrames;
        winClean = true;
        return;
    }

    if( (now - winStartNs) < RATE_WINDOW_NS )
    {
        return;
    }

    seconds = ((double)(now - winStartNs) / 1e9);
    if( winClean && !tuned )
    {
        ratio = (((double)winFrames - (smoothFrames - winStartDepth)) / ((double)sampleRate * seconds));
        if( ratio > (1.0 + RATE_LIMIT) )
        {
            ratio = (1.0 + RATE_LIMIT);
        }
        else if( ratio < (1.0 - RATE_LIMIT) )
        {
            ratio = (1.0 - RATE_LIMIT);
        }
        // The first measurement since the device opened is taken whole; later ones are averaged.
        if( !deviceMeasured )
        {
            deviceRatio = ratio;
            deviceMeasured = true;
        }
        else
        {
            deviceRatio += ((ratio - deviceRatio) / 2.0);
        }
    }

    winStartNs = now;
    winFrames = 0;
    winStartDepth = smoothFrames;
    winClean = true;
}

// Called after each batch goes to SDL. Averages the depth over the device's pulls, then stretches
// the sample period while it is too deep and shrinks it while too shallow, until it crosses the
// target. Far too deep is cut back at once: only an off-nominal tuning, which plays at a rate the
// program's time cannot follow, gets there. With a tuning, nothing else is steered, so its pitch
// is kept.
static void
steer(uint64_t now)
{
int queued;

    queued = queuedFrames();
    ++tFlushes;
    if( queued < tMinFrames )
    {
        tMinFrames = queued;
    }
    if( queued > tMaxFrames )
    {
        tMaxFrames = queued;
    }
    if( queued == 0 )
    {
        winClean = false;
    }

    smoothFrames += (((double)queued - smoothFrames) / STEER_SMOOTHING);

    if( smoothFrames > (double)ceilingFrames )
    {
        audioClearQueue(dev);
        ++tCeilings;
        preload(now);
        return;
    }

    measureDevice(now);

    if( tuned )
    {
        steerAdj = 0.0;
    }
    else if( steerAdj == 0.0 )
    {
        if( smoothFrames > (double)(targetFrames + bandFrames) )
        {
            steerAdj = STEER_RATIO;
        }
        else if( smoothFrames < (double)(targetFrames - bandFrames) )
        {
            steerAdj = -STEER_RATIO;
        }
    }
    else if( ((steerAdj > 0.0) && (smoothFrames <= (double)targetFrames)) ||
             ((steerAdj < 0.0) && (smoothFrames >= (double)targetFrames)) )
    {
        steerAdj = 0.0;
    }

    if( steerAdj != 0.0 )
    {
        ++tSteered;
    }
}

// With pidp1timing on, appends one line a second of simtime and starts the next second's counts.
// The file stays open, so the emulator thread pays a write and a flush a second, not an open.
static void
timingLine(uint64_t now)
{
double msPerFrame;

    if( timingNextNs == 0 )
    {
        timingNextNs = (now + 1000000000ull);
        return;
    }

    if( now < timingNextNs )
    {
        return;
    }

    if( !timingFP )
    {
        timingFP = fopen(AUDIO_TIMING_FILE, "a");
    }

    if( timingFP )
    {
        msPerFrame = (1000.0 / (double)sampleRate);
        fprintf(timingFP,
            "audio depth %.1f ms smoothed %.1f min %.1f max %.1f target %d steer %+.1f%% "
            "device %+.3f%% frames %ld preloaded %ld halts %ld capfirings %ld ceilings %ld "
            "steered %ld/%ld\n",
            ((double)queuedFrames() * msPerFrame), (smoothFrames * msPerFrame),
            (tFlushes ? ((double)tMinFrames * msPerFrame) : 0.0),
            (tFlushes ? ((double)tMaxFrames * msPerFrame) : 0.0),
            depthMs, (steerAdj * 100.0), ((deviceRatio - 1.0) * 100.0), tFrames, tPreloaded,
            tHalts, tCaps, tCeilings, tSteered, tFlushes);
        fflush(timingFP);
    }

    tFrames = tPreloaded = tHalts = tCaps = tCeilings = tSteered = tFlushes = 0;
    tMinFrames = INT_MAX;
    tMaxFrames = 0;
    timingNextNs = (now + 1000000000ull);
}

// Returns voice i's (0-3) cutoff on the CHM interface, 1 / (2 pi R C).
static float
chmCutoff(int i)
{
    return( (float)(1.0 / (2.0 * M_PI * CHM_R * chmC[i])) );
}

// Applies any cutoff asked for since the last pass. Until one is set, a voice has the CHM cutoff.
static void
applyCutoffs(void)
{
unsigned gen;
float hz;

    gen = atomic_load_explicit(&cutoffGen, memory_order_acquire);
    if( gen == appliedGen )
    {
        return;
    }
    appliedGen = gen;

    for( int i = 0; i < 4; ++i )
    {
        hz = atomic_load_explicit(&cutoffHz[i], memory_order_relaxed);
        rcSetCutoff(&voice[i], ((hz > 0.0f) ? hz : chmCutoff(i)));
    }
}

// Applies the flags as they are at simtime t to the filters' inputs. A flag that changed since the
// last pass changed in the instruction that ended at t, so its edge is placed there.
static void
applyFlags(PDP1 *pdp, uint64_t t)
{
double level;
int flags;

    flags = (pdp->pf & VOICE_BITS);
    if( flags == lastFlags )
    {
        return;
    }
    lastFlags = flags;

    for( int i = 0; i < 4; ++i )
    {
        level = ((pdp->pf & voiceBit[i]) ? HIVAL : LOWVAL);
        if( level != voice[i].x )
        {
            rcSetInput(&voice[i], t, level);
        }
    }
}

// Runs once per main-loop pass while the machine runs: makes every sample that simtime has
// reached, one per period of the sample rate, then applies this pass's flag changes. The samples
// come first because a flag seen changed now changed at now, after any sample due before it.
void
svc_audio(PDP1 *pdp)
{
uint64_t now;
uint64_t whole;

    if( !atomic_load_explicit(&isInitialized, memory_order_acquire) )
    {
        return;
    }

    applyRequests();
    if( (dev == AUDIO_HANDLE_INVALID) || isStopped )
    {
        return;
    }

    now = pdp->simtime;
    applyCutoffs();

    // A lag-cap firing moves simtime over time the program never ran, so no samples are owed
    // for it. The queue is left shallower and steering refills it: filling the gap with the held
    // level would put a flat stretch into sound the queue was still playing without a break.
    // Only a rise counts: the timing report sets the counter back to 0 at a halt.
    if( throttleCapFirings > capSeen )
    {
        capSeen = throttleCapFirings;
        dueNs = now;
        dueFrac = 0.0;
        winClean = false;
        ++tCaps;
    }
    else
    {
        capSeen = throttleCapFirings;
        if( needAnchor || ((now - lastSimNs) > REANCHOR_GAP_NS) )
        {
            // A start, or the first pass after a halt or a step: the queue has drained while
            // the machine was stopped, so it is filled to the target before the program's sound.
            if( !needAnchor )
            {
                ++tHalts;
            }
            needAnchor = false;
            dueNs = now;
            dueFrac = 0.0;
            applyFlags(pdp, now);
            preload(now);
        }
    }
    lastSimNs = now;

    while( now >= dueNs )
    {
        if( appendFrame(dueNs) )
        {
            steer(now);
        }

        dueFrac += ((periodNs * (1.0 + steerAdj)) / deviceRatio);
        whole = (uint64_t)dueFrac;
        dueNs += whole;
        dueFrac -= (double)whole;
    }

    applyFlags(pdp, now);

    if( timingOn )
    {
        timingLine(now);
    }
}

// Set the sampling rate for SDB.
// Oversampling is ok.
// Requires a teardown and reopen, which the emulator thread does at its next pass. Any thread.
void
setSampleRate(int perSec)
{
    atomic_store_explicit(&requestedRate, perSec, memory_order_relaxed);
    postRequest(&rateGen);
}

// Get the sampling rate for SDB, as last asked for.
// Oversampling is ok.
int
getSampleRate()
{
    return( atomic_load_explicit(&requestedRate, memory_order_relaxed) );
}

// Sets a voice's cutoff, in Hz: voice 1-4, or 0 for all four. 0 Hz or less puts back the CHM
// interface's own cutoff for that voice. Takes effect at the next pass; any thread may call it.
void
setFilterCutoff(int voiceNum, float hz)
{
    if( hz <= 0.0f )
    {
        hz = 0.0f;
    }
    else if( hz < CUTOFF_MIN_HZ )
    {
        hz = CUTOFF_MIN_HZ;
    }
    else if( hz > CUTOFF_MAX_HZ )
    {
        hz = CUTOFF_MAX_HZ;
    }

    for( int i = 0; i < 4; ++i )
    {
        if( (voiceNum == 0) || (voiceNum == (i + 1)) )
        {
            atomic_store_explicit(&cutoffHz[i], ((hz > 0.0f) ? hz : chmCutoff(i)), memory_order_relaxed);
        }
    }
    atomic_fetch_add_explicit(&cutoffGen, 1, memory_order_release);
}

// Returns voice 1-4's cutoff in Hz, as asked for or the CHM default.
float
getFilterCutoff(int voiceNum)
{
float hz;

    if( (voiceNum < 1) || (voiceNum > 4) )
    {
        return( 0.0f );
    }

    hz = atomic_load_explicit(&cutoffHz[voiceNum - 1], memory_order_relaxed);
    return( (hz > 0.0f) ? hz : chmCutoff(voiceNum - 1) );
}

// Converts an old per-sample alpha, a = 1 - exp(-1/(tau fs)), to the cutoff it gave at the current
// sample rate, so the command port's alpha words still work. The alpha is held inside (0, 1).
float
alphaToCutoff(float alpha)
{
    if( alpha < 0.0001f )
    {
        alpha = 0.0001f;
    }
    else if( alpha > 0.9999f )
    {
        alpha = 0.9999f;
    }

    return( (float)((-log(1.0 - (double)alpha) * (double)getSampleRate()) / (2.0 * M_PI)) );
}

void
setMixerGain(float newGain)
{
    if( newGain < 0.0 )
    {
        newGain = 0.0;          // negative is useless
    }

    atomic_store_explicit(&mixerGain, newGain, memory_order_relaxed);
}

float
getMixerGain()
{
    return( atomic_load_explicit(&mixerGain, memory_order_relaxed) );
}

// 1.0 is no tuning. Greater than 1.0 raises pitch. Less than 1.0 lowers pitch.
// This takes effect at the emulator thread's next pass when built with SDL3. Any thread.
// SDL2 has no/ pitch-ratio concept on a plain queued device, so under SDL2 this is saved but ignored.
void
setAudioTuning(float newTuning)
{
    if( newTuning > 0.0 )
    {
        atomic_store_explicit(&requestedTuning, newTuning, memory_order_relaxed);
        postRequest(&tuningGen);
    }
}

// The tuning as last asked for.
float
getAudioTuning()
{
    return( atomic_load_explicit(&requestedTuning, memory_order_relaxed) );
}

// Expects an int[3], returns current overflow count, if rsltP is not null,
// puts max max value seen, min value seen and the samples made in the array and resets the values.
// Any thread.
int
getOverflowData(int *rsltP)
{
int i;
int pos, neg, samples;

    i = atomic_exchange_explicit(&overflows, 0, memory_order_relaxed);
    pos = atomic_exchange_explicit(&posOverflow, 0, memory_order_relaxed);
    neg = atomic_exchange_explicit(&negOverflow, 0, memory_order_relaxed);
    samples = atomic_exchange_explicit(&nsamples, 0, memory_order_relaxed);

    if( rsltP )
    {
        *rsltP++ = pos;
        *rsltP++ = neg;
        *rsltP = samples;
    }

    return( i );
}
