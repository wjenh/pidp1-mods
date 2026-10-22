/*
 * IOT 71: four 12-bit D/A converters that play samples straight from core, a hypothetical
 * PDP-1 addition built from DEC's converters and a Type 19 high speed channel. IOT 72
 * (IOT_72.c) is an alias that sends wavetable mode's commands here. Instruction bits 6-9 are
 * the command, bits 10-11 the converter.
 *
 * Each word a converter fetches comes through high speed channel 2 and steals one
 * machine cycle; when two are due in the same cycle the lower numbered converter goes first.
 * A window timer in the multiplexer keeps leaving the program three free cycles in a row, so
 * the reads cannot stop a deferred instruction for good. The modes and abilities:
 * - Buffer mode: a buffer of samples at a fixed period, with latches that load as each
 *   buffer begins, when the flag sets.
 * - Wavetable mode: a power-of-two table at any pitch through an envelope, timed by an
 *   event counter, with a queue one note deep behind the latches.
 * - Sample mode: a sampled note, an attack once then a loop; up to four timbres that follow
 *   the envelope's level; interpolation, and packing of three samples in two words.
 * - Per converter: a volume and a stereo place that slew toward their settings, the voicing
 *   (edge tone, speech, céleste; edge.h), a slow counter for vibrato and Leslie, and a
 *   percussion read.
 * After the mixer comes the room (room.h); the onset delays each wavetable note's load by a
 * pseudo-random time up to a bound. Both start as the configuration sets them (dacroom,
 * daconset, with dacgain for the level), a program can set either (IOT 72's command 017),
 * and both are host work: they steal nothing.
 *
 * Everything runs on the emulator thread: iotHandler() for the commands, iotIOPoll() once
 * per machine cycle for the clocks, the channel and the mix. Audio goes to SDL 2 or 3 at
 * 50,000 frames a second of machine time.
 *
 * ../../FunStuff/Dacmusic/Implementation.md, "The device", describes every command, mode and
 * timing rule in full. room.h and edge.h are compiled into the renderer too
 * (FunStuff/Dacmusic/render/dacrender.c), so the device and the renderer compute the same
 * integers.
 *
 * 24-Sep-2026 Claude written.
 * 14-Oct-2026 wje allow modifiying room and onset via the IOT, not just the config file
 * 20-Oct-2026 wje use iotCondlong, not stderr
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "iotHandler.h"
#include "highSpeedChannels.h"
#include "configuration.h"
#include "room.h"
#include "edge.h"

//#define DOLOGGING
#include "iotLogger.h"
#ifndef LOG_DAC_SETUP
#define LOG_DAC_SETUP 0          // the start-up lines: stream, gain, room, onset
#endif
#ifndef LOG_DAC_CHANGES
#define LOG_DAC_CHANGES 0        // the room or onset changed by the voices or the program
#endif
#ifndef LOG_DAC_COUNTS
#define LOG_DAC_COUNTS 1         // the counts at stop: late samples, lost fetches, clipping
#endif

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

#define CONVERTERS 4
#define HSC_CHAN 2              // drum 1, 340 3: a late sample is heard, a late vector barely seen

// The window timer. A break granted in cycle 0 or a defer cycle starts the instruction again
// (PDP-1 Maintenance Manual F-17, 6-18), so reads that never leave three free cycles in a row
// stop the program at its next deferred instruction for good. Once WINDOW_BUSY polls have
// passed without WINDOW_FREE free in a row, no read starts until there have been.
#define WINDOW_FREE 3           // cycle 0, one defer, and the poll a request waits for its steal
#define WINDOW_BUSY 40          // polls without a window before the timer holds the channel

// What startFetch() did.
#define FETCH_NONE 0            // no read due, or a word still on the channel
#define FETCH_STARTED 1
#define FETCH_HELD 2            // a read is due and the timer held it
#define OUT_RATE 50000          // audio samples a second, machine time
#define CYCLES_PER_OUT (200000 / OUT_RATE)
#define BATCH_FRAMES 250        // 5 ms of audio per SDL call: a call per frame slows the emulator
#define PRELOAD_FRAMES 2500     // 50 ms queued before playback starts, to ride out host jitter
#define CORE16MASK 0177777
#define WORDMASK 0777777
#define SIGNBIT 0400000
#define FULLSCALE 2047.0f       // largest 12-bit ones' complement magnitude

// IOT 71's commands, instruction bits 6-9.
#define CMD_ADDRESS 0           // latch the next buffer's address, IO bits 2-17
#define CMD_LENGTH 1            // latch its length in words, IO bits 6-17, 0 = 4096
#define CMD_PERIOD 2            // latch its sample period in cycles, IO bits 6-17, 0 = 4096
#define CMD_GO 3                // load the latches and start
#define CMD_HALT 4              // stop; the output falls to 0
#define CMD_SKIP 5              // skip if the flag (a new buffer has begun) is set, and clear it
#define CMD_BREAK 6             // IO bit 0 enables a break at each new buffer, IO bits 14-17 its channel
#define CMD_STATUS 7            // IO = running, flag, words left in the buffer

// IOT 72's commands, instruction bits 6-9: wavetable mode's latches, and the queue.
#define WAVE_DEV 072
#define CMD_STEP 0              // the step, IO 6 integer and 12 fraction bits of a table word
#define CMD_EVENT 1             // the event length in machine cycles, IO 18 bits, 0 = 262144
#define CMD_ATTACK 2            // the attack rate, IO 18 bits
#define CMD_DECAY 3             // the decay rate
#define CMD_RELEASE 4           // the release rate
#define CMD_LEVELS 5            // IO bits 0-8 the start level, bits 9-17 the peak, 1/8 dB
#define CMD_SUSTAIN 6           // IO bits 0-8 the sustain level, bits 11-17 the actions
#define CMD_WAVESTATUS 7        // IO = envelope stage (bits 0-2), attenuation (bits 6-17)
#define CMD_COMMIT 010          // instruction bit 6 set: the latches to the queue
#define CMD_SLOWRATE 011        // the slow counter's rate, IO 18 bits
#define CMD_VIBRATO 012         // IO bits 0-8 the vibrato's depth, 9-16 the delay, 17 free running
#define CMD_LESLIE 013          // IO bits 0-8 the tremolo's depth, 9-17 the place's, 1/8 dB
#define CMD_PERCLEVEL 014       // IO bits 0-8 the percussion's level, 1/8 dB, 15-17 its multiple
#define CMD_PERCDECAY 015       // the percussion's decay rate
#define CMD_PERCTABLE 016       // the percussion's sine, 256 words, IO bits 2-17
#define CMD_SETTINGS 017        // the device's own settings, the converter field choosing which

// IOT 72's command 017: the converter field picks one of these, for the whole device.
#define SET_ROOM 0              // the room: IO bits 16-17, 0 none, 1 hall, 2 Busch Hall, 3 voices
#define SET_ONSET 1             // the onset's bound: IO bits 9-17 in ONSET_UNITs
#define SET_READ 2              // IO = the room set, the room playing, the onset's bound
#define SET_CONFIGURED SIGNBIT  // IO bit 0, to SET_ROOM or SET_ONSET: the configuration's
#define ROOM_CODE_VOICES 3      // SET_ROOM's code for the voices' choice
#define ONSET_UNIT 25           // cycles in 1/8 ms
#define ONSET_UNITS_MAX 0777

// IOT 71's commands with instruction bit 6 set: sample mode, the timbre, the volume and the place.
#define CMD_END 010             // the sample's end in words (packed: groups), IO bits 2-17, 0 = 65536
#define CMD_LOOP 011            // the loop's length in words (packed: groups), IO bits 2-17
#define CMD_STRIDE 012          // the timbres' stride in words (packed: groups), IO bits 2-17, 0 = one timbre
#define CMD_THRESH12 013        // IO bits 0-8 threshold 1, bits 9-17 threshold 2, 1/8 dB
#define CMD_THRESH3 014         // IO bits 0-8 threshold 3
#define CMD_VOLUME 015          // the volume, IO bits 9-17 in 1/8 dB, 0777 mute; not latched
#define CMD_PLACE 016           // the place: IO bits 0-8 left, 9-17 right, 1/8 dB, 0777 off; not latched
#define CMD_PHASESTATUS 017     // IO = the phase's integer part (bits 2-17)

// The actions, IO bits 11-17 of CMD_SUSTAIN; the held ones are kept, the rest apply once.
#define ACT_WAVE 01             // wavetable mode (clear: buffer mode)
#define ACT_KEYON 02            // the attenuation to the start level, and ATTACK
#define ACT_KEYOFF 04           // RELEASE, unless OFF
#define ACT_PHASE 010           // the phase to 0
#define ACT_SAMPLE 020          // sample mode, with ACT_WAVE
#define ACT_INTERP 040          // the interpolation
#define ACT_PACK 0100           // the packing, with ACT_SAMPLE: three samples in two words
#define ACT_HELD (ACT_WAVE | ACT_SAMPLE | ACT_INTERP | ACT_PACK)
#define ACT_MASK 0177

#define EVENT_MAX 262144
#define FLOOR 0xFFFFFF          // the largest attenuation, 63.99 dB: OFF
#define NO_SUSTAIN 0777         // a sustain level: the decay runs on to the floor
#define PHASE_MASK 0xFFFFFFF    // 16 integer and 12 fraction bits
#define THRESHOLDS 3
#define NO_THRESHOLD 0777
#define GAIN_STEPS 4096         // the exponential converter's 1/64 dB steps: a sum past them is silence
#define MUTE 0777               // a volume: heads for GAIN_STEPS
#define CYCLES_PER_MS 200       // machine cycles, for the onset's bound
#define ONSET_SEED 2654435769u  // a converter's sequence starts at its number + 1 times this
#define ROOM_VOICES (-1)        // dacroom=voices: the room the running notes' voicing chooses
#define EDGE_UNIT (1.0f / (float)(1 << EDGE_OUT_SHIFT))
#define CELESTE_MIX ((float)EDGE_CELESTE_MIX)
#define SLOW_TICK 64            // the slow counter clocks at every 64th cycle from the go
#define SLOW_PHASE 0xFFFFFF     // its phase, 24 bits
#define SLOW_HOLD 16            // the delay's unit, in clocks
#define SLOW_QUARTERS 0xC00000  // the second triangle is read three quarters on
#define PERC_MASK 0377          // the percussion's sine, 256 words
#define PERC_PHASE 0xFFFFF      // its phase, 8 integer and 12 fraction bits
#define NO_ADDRESS (-1)         // a pair's word that holds nothing

// The reads with a pair, and what a word on the channel is for: the first read's and the
// céleste's words without the interpolation or packing, then each pair's two.
#define READ_FIRST 0
#define READ_SECOND 1
#define READ_PERC 2
#define READS 3
#define KIND_FIRST 0
#define KIND_SECOND 1
#define KIND_PAIR 2             // + 2 x the read + the word

typedef enum { STAGE_OFF, STAGE_ATTACK, STAGE_DECAY, STAGE_SUSTAIN, STAGE_RELEASE } Stage;

// A read's pair: the last two words it read, each with its core address, and the phase's
// fraction at the sample time they are for, whether it interpolates, and a packed read's places.
typedef struct
{
    int address[2];             // NO_ADDRESS: nothing held
    int value[2];               // the word as read, 18 bits
    bool due[2];                // waiting for the channel
    bool landed[2];             // in, so it can be kept at the next sample time
    int fraction;
    bool interp;                // the sample is the two samples interpolated
    int place;                  // packed: sample i's place in its group, 0-2; unpacked: -1
    int nextPlace;              // packed: the next sample's
    bool form;                  // the sample is formed at the next cycle
} Pair;

// What the end of an event or buffer loads besides the event's length: the note.
typedef struct
{
    int address;
    int length;
    int period;
    int step;
    int attack;
    int decay;
    int release;
    int start;
    int peak;
    int sustain;
    int actions;
    int end;
    int loop;
    int stride;
    int thresholds[THRESHOLDS];
    int voicing;                // dzv's low bits: the edge, the speech, the room
    int slowRate;               // the slow counter's
    int vibratoDepth;
    int vibratoDelay;
    bool slowFree;
    int tremoloDepth;
    int panDepth;
    int percLevel;              // the percussion's
    int percMultiple;
    int percDecay;
    int percTable;
} Note;

typedef struct
{
    bool running;
    bool flag;
    bool due;                   // a sample is waiting for the channel
    bool breakEnabled;
    int breakChannel;
    int nextAddress;
    int nextLength;
    int nextPeriod;
    int address;
    int remaining;
    int period;
    int countdown;
    int late;                   // samples replaced while still waiting for the channel
    float level;                // the analog output, -1 to 1

    // Wavetable mode: the latches, loaded with the others.
    int nextStep;
    int nextEvent;
    int nextAttack;
    int nextDecay;
    int nextRelease;
    int nextStart;
    int nextPeak;
    int nextSustain;
    int nextActions;
    int nextEnd;
    int nextLoop;
    int nextStride;
    int nextThresholds[THRESHOLDS];
    int nextVoicing;
    Note nextSlow;              // the slow counter's and the percussion's latches, in these fields alone
    // ... and what they load.
    bool wave;
    bool sampled;               // sample mode
    int end;
    int loop;
    int stride;
    int thresholds[THRESHOLDS];
    int mask;
    int step;
    int eventLength;
    int eventLeft;
    int attackRate;
    int decayRate;
    int releaseRate;
    int start;
    int peak;
    int sustain;
    uint32_t phase;
    int32_t attenuation;
    Stage stage;
    int fetchAddress;           // the table word a due sample fetches
    float sample;               // the last word fetched, -1 to 1
    bool dropFetch;             // the word on the channel is the old note's, dropped at a key on

    // The céleste: the detune k (0: none), the second phase and its step, whether this sample
    // time is the odd one (no second read), and the second read's slot and word.
    int celeste;
    uint32_t phase2;
    int step2;
    bool celesteOdd;
    bool due2;
    int fetchAddress2;
    float sample2;

    // The queue: a commit's latches, the event length with them, whether it holds them, and a
    // commit waiting for it to load.
    Note queued;
    int queuedEvent;
    bool queueFull;
    bool commitWaiting;

    // The volume, 1/64 dB, and the value it heads for; never latched.
    int volume;
    int volumeTarget;

    // The place: the mixer's attenuation into each channel, left then right, 1/64 dB, and
    // the values they head for; never latched.
    int place[2];
    int placeTarget[2];

    // The onset: the note taken at an event's end, the cycles until it loads (0: none
    // waiting), and the converter's pseudo-random sequence.
    Note held;
    int onsetLeft;
    uint32_t onsetRandom;

    // The voicing: the edge tone's and the speech's state, and the cycles since the go,
    // whose every fourth is a noise sample.
    EdgeState edge;
    int ticks;

    // The interpolation and the packing: whether the note has them, and each read's pair.
    bool interp;
    bool packed;
    Pair pairs[READS];

    // The slow counter: the note's latches, its phase, the clocks left of its delay, and the
    // terms formed from it: the vibrato's m, the steps taken, the tremolo, the place's offsets.
    int slowRate;
    int vibratoDepth;
    int vibratoDelay;
    bool slowFree;
    int tremoloDepth;
    int panDepth;
    uint32_t slowPhase;
    int slowHold;
    int64_t slowM;
    int stepTaken;
    int step2Taken;
    int tremolo;
    int panOffset[2];

    // The percussion: the note's latches, its phase, its attenuation (FLOOR: none) and word.
    int percLevel;
    int percMultiple;
    int percDecay;
    int percTable;
    uint32_t percPhase;
    int32_t percAtt;
    float percSample;
} Converter;

static Converter converters[CONVERTERS];
static HSCChannelP hscP;
static HSCRequest request;
static uint32_t fetchWord;
static int fetching = -1;       // the converter whose word is on the channel, or -1
static int fetchingKind;        // what that word is for: KIND_FIRST, KIND_SECOND, or a pair's
static int fetchingAddress;     // and its address
static int lostFetches;         // requests the channel refused or did not complete
static int freePolls;           // polls in a row at which no read started
static int busyPolls;           // polls since the last WINDOW_FREE free in a row
static bool heldDue;            // the timer has held a read that was due, in this hold
static int windows;             // holds that kept a due read waiting, since the last halt

static AudioHandle audioDev = AUDIO_HANDLE_INVALID;
static bool isPlaying;
static bool isPrimed;           // PRELOAD_FRAMES have been queued and playback has begun
static int16_t batch[2 * BATCH_FRAMES];
static int batchFrames;
static int queuedFrames;
static float mixGain = 1.0f;
static float gains[4096];       // the exponential converter: the gain of each 1/64 dB
static float sumLeft;
static float sumRight;
static int outCycles;
static int clippedFrames;       // frames with a channel past full scale, since the last halt
static int roomKind = ROOM_OFF; // the room set: a room, or ROOM_VOICES
static Room room;               // its kind the room playing
static int onsetCycles;         // the onset's bound set, in machine cycles; 0: off
static int configRoomKind = ROOM_OFF; // the configuration's dacroom, for SET_CONFIGURED
static int configOnsetCycles;   // and its daconset

// Opens the default playback device at OUT_RATE, stereo, signed 16-bit.
// Returns AUDIO_HANDLE_INVALID on failure.
static AudioHandle
audioOpenDevice(void)
{
#ifdef HAVE_SDL3
SDL_AudioSpec spec = { .format = SDL_AUDIO_S16, .channels = 2, .freq = OUT_RATE };

    return( SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL) );
#else
SDL_AudioSpec spec;

    memset(&spec, 0, sizeof(spec));
    spec.freq = OUT_RATE;
    spec.format = AUDIO_S16;
    spec.channels = 2;
    spec.samples = 256;
    spec.callback = NULL;
    return( SDL_OpenAudioDevice(NULL, 0, &spec, NULL, 0) );
#endif
}

// Pauses or resumes playback.
static void
audioPauseDevice(bool pause)
{
#ifdef HAVE_SDL3
    if( pause )
    {
        SDL_PauseAudioStreamDevice(audioDev);
    }
    else
    {
        SDL_ResumeAudioStreamDevice(audioDev);
    }
#else
    SDL_PauseAudioDevice(audioDev, pause);
#endif
}

// Discards any audio queued but not yet played.
static void
audioClearQueue(void)
{
#ifdef HAVE_SDL3
    SDL_ClearAudioStream(audioDev);
#else
    SDL_ClearQueuedAudio(audioDev);
#endif
}

// Queues count stereo frames.
static void
audioQueueFrames(int16_t *framesP, int count)
{
#ifdef HAVE_SDL3
    SDL_PutAudioStreamData(audioDev, framesP, (int)(count * 2 * sizeof(int16_t)));
#else
    SDL_QueueAudio(audioDev, framesP, (uint32_t)(count * 2 * sizeof(int16_t)));
#endif
}

// Logs the stream's rate and the device's, which SDL converts between; a capture through
// SDL's disk driver is written at the device's rate.
static void
reportDevice(void)
{
#ifdef HAVE_SDL3
SDL_AudioSpec spec;
int frames;

    if( (audioDev != AUDIO_HANDLE_INVALID) &&
        SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(audioDev), &spec, &frames) )
    {
        iotCondLog(LOG_DAC_SETUP, "IOT 71: %d Hz stream, device freq=%d\n", OUT_RATE, spec.freq);
        return;
    }
#endif
    iotCondLog(LOG_DAC_SETUP, "IOT 71: %d Hz stream%s\n", OUT_RATE,
        (audioDev == AUDIO_HANDLE_INVALID) ? ", no audio device" : "");
}

// Starts or restarts from an empty queue; playback itself waits for PRELOAD_FRAMES.
static void
startPlaying(void)
{
    if( audioDev == AUDIO_HANDLE_INVALID )
    {
        return;
    }

    audioPauseDevice(true);
    audioClearQueue();
    sumLeft = 0.0f;
    sumRight = 0.0f;
    outCycles = 0;
    batchFrames = 0;
    queuedFrames = 0;
    isPrimed = false;
    // The room the voices chose last stays; the hall before any.
    roomInit(&room, ((roomKind != ROOM_VOICES) ? roomKind : ((room.kind == ROOM_BUSCH) ? ROOM_BUSCH : ROOM_HALL)));
    isPlaying = true;
}

// A setting's number. The configuration parses a number itself and keeps no string for
// it: "8" is in ivalue, "8.5" in fvalue.
static double
settingNumber(ConfigurationSettingP settingP)
{
    return( isnan(settingP->fvalue) ? (double)settingP->ivalue : (double)settingP->fvalue );
}

// First use: the channel, the audio device and the configured gain.
// Returns false if the channel cannot be had, which makes the IOT unknown to the emulator.
static bool
setUp(void)
{
ConfigurationSettingP settingP;
int i;

    if( hscP )
    {
        return( true );
    }

    if( !(hscP = HSCallocateChannel(HSC_CHAN)) )
    {
        return( false );
    }

    for( i = 0; i < 4096; i++ )
    {
        gains[i] = (float)pow(10.0, -(double)i / 1280.0);
    }

    // The mix before the place: converters 1 and 2 left, 3 and 4 right.
    for( i = 0; i < CONVERTERS; i++ )
    {
        converters[i].place[0] = ((i < 2) ? 0 : GAIN_STEPS);
        converters[i].place[1] = ((i < 2) ? GAIN_STEPS : 0);
        converters[i].placeTarget[0] = converters[i].place[0];
        converters[i].placeTarget[1] = converters[i].place[1];
    }

    // "dacgain=" scales the mix; the emulator's own audio setting does not apply.
    if( (settingP = findConfigurationSetting(getConfiguration(), "dacgain")) )
    {
        mixGain = (float)settingNumber(settingP);
    }
    iotCondLog(LOG_DAC_SETUP, "IOT 71: gain %.3f\n", mixGain);

    // "dacroom=" is the room after the mixer: hall, busch, voices (as the notes' voicing
    // chooses), or off (anything else).
    if( (settingP = findConfigurationSetting(getConfiguration(), "dacroom")) && settingP->strvalueP )
    {
        if( strcmp(settingP->strvalueP, "hall") == 0 )
        {
            roomKind = ROOM_HALL;
        }
        else if( strcmp(settingP->strvalueP, "busch") == 0 )
        {
            roomKind = ROOM_BUSCH;
        }
        else if( strcmp(settingP->strvalueP, "voices") == 0 )
        {
            roomKind = ROOM_VOICES;
        }
    }
    iotCondLog(LOG_DAC_SETUP, "IOT 71: room %s\n",
            ((roomKind == ROOM_BUSCH) ? "busch" : ((roomKind == ROOM_HALL) ? "hall" :
             ((roomKind == ROOM_VOICES) ? "from the voices, hall until one chooses busch" : "off"))));

    // "daconset=" is the onset's bound in milliseconds; off when absent or 0.
    if( (settingP = findConfigurationSetting(getConfiguration(), "daconset")) )
    {
        onsetCycles = (int)lrint(settingNumber(settingP) * CYCLES_PER_MS);
        if( onsetCycles < 0 )
        {
            onsetCycles = 0;
        }
    }
    iotCondLog(LOG_DAC_SETUP, "IOT 71: onset %d cycles\n", onsetCycles);
    configRoomKind = roomKind;
    configOnsetCycles = onsetCycles;

    SDL_InitSubSystem(SDL_INIT_AUDIO);
    audioDev = audioOpenDevice();
    reportDevice();
    startPlaying();
    return( true );
}

// Takes the note the latches hold. The one-shot actions are consumed, so the program can
// latch the next event's at once.
static void
takeNote(Converter *cP, Note *nP)
{
int i;

    nP->address = cP->nextAddress;
    nP->length = cP->nextLength;
    nP->period = cP->nextPeriod;
    nP->step = cP->nextStep;
    nP->attack = cP->nextAttack;
    nP->decay = cP->nextDecay;
    nP->release = cP->nextRelease;
    nP->start = cP->nextStart;
    nP->peak = cP->nextPeak;
    nP->sustain = cP->nextSustain;
    nP->actions = cP->nextActions;
    nP->end = cP->nextEnd;
    nP->loop = cP->nextLoop;
    nP->stride = cP->nextStride;
    for( i = 0; i < THRESHOLDS; i++ )
    {
        nP->thresholds[i] = cP->nextThresholds[i];
    }
    nP->voicing = cP->nextVoicing;
    nP->slowRate = cP->nextSlow.slowRate;
    nP->vibratoDepth = cP->nextSlow.vibratoDepth;
    nP->vibratoDelay = cP->nextSlow.vibratoDelay;
    nP->slowFree = cP->nextSlow.slowFree;
    nP->tremoloDepth = cP->nextSlow.tremoloDepth;
    nP->panDepth = cP->nextSlow.panDepth;
    nP->percLevel = cP->nextSlow.percLevel;
    nP->percMultiple = cP->nextSlow.percMultiple;
    nP->percDecay = cP->nextSlow.percDecay;
    nP->percTable = cP->nextSlow.percTable;
    cP->nextActions &= ACT_HELD;
}

// A read's pair holds nothing, and asks for nothing.
static void
pairClear(Pair *pP)
{
int k;

    for( k = 0; k < 2; k++ )
    {
        pP->address[k] = NO_ADDRESS;
        pP->due[k] = false;
        pP->landed[k] = false;
    }
    pP->form = false;
}

// A converter that stops asks the channel for nothing more.
static void
pairsIdle(Converter *cP)
{
int r;

    for( r = 0; r < READS; r++ )
    {
        cP->pairs[r].due[0] = false;
        cP->pairs[r].due[1] = false;
    }
}

// A slow counter's triangle of its phase p, -16,384 to 16,384: 0 at 0, the peak at a quarter.
static int
triangle(uint32_t p)
{
int q;

    q = (int)(p >> 8);
    if( q < 16384 )
    {
        return( q );
    }
    if( q < 49152 )
    {
        return( 32768 - q );
    }
    return( q - 65536 );
}

// Forms the slow counter's terms from its phase now: the vibrato's m and the steps taken, the
// tremolo, and the place's offsets. With every depth 0 the steps are the note's own.
static void
slowTerms(Converter *cP)
{
int64_t t1;
int64_t t2;
int64_t a;
int64_t p;

    t1 = triangle(cP->slowPhase);
    t2 = triangle((cP->slowPhase + SLOW_QUARTERS) & SLOW_PHASE);
    cP->slowM = ((cP->vibratoDepth * t1) >> 9);
    a = ((int64_t)cP->tremoloDepth << 3);
    p = ((int64_t)cP->panDepth << 3);
    cP->tremolo = (int)((a * (16384 + t2)) >> 15);
    cP->panOffset[0] = (int)((p * (16384 + t2)) >> 15);
    cP->panOffset[1] = (int)((p * (16384 - t2)) >> 15);
    cP->stepTaken = (int)(cP->step + (((int64_t)cP->step * cP->slowM) >> 19));
    cP->step2Taken = (int)(cP->step2 + (((int64_t)cP->step2 * cP->slowM) >> 19));
}

// The word on the channel is the old note's: dropped when it lands. reads1 is whether the first
// read's and the céleste's are dropped, perc the percussion's.
static void
dropOnChannel(Converter *cP, bool reads1, bool perc)
{
bool isPerc;

    if( fetching != (int)(cP - converters) )
    {
        return;
    }

    isPerc = (fetchingKind >= (KIND_PAIR + (2 * READ_PERC)));
    if( (isPerc && perc) || (!isPerc && reads1) )
    {
        cP->dropFetch = true;
    }
}

// Loads a note and applies its one-shot actions, then its voicing: with the speech, the
// attack is held to the rise's rate while it lasts. A key on restarts the slow counter unless
// it runs free, and on a detached note triggers the percussion; the slow counter's terms are
// formed again either way. A converter IOT 72 never touched stays in buffer mode.
static void
loadNote(Converter *cP, const Note *nP)
{
int i;
int32_t rise;
bool detached;

    cP->address = nP->address;
    cP->remaining = nP->length;
    cP->period = nP->period;

    cP->wave = ((nP->actions & ACT_WAVE) != 0);
    cP->sampled = ((nP->actions & ACT_SAMPLE) != 0);
    cP->mask = (nP->length - 1);
    cP->packed = (cP->sampled && ((nP->actions & ACT_PACK) != 0));
    cP->end = (cP->packed ? (3 * nP->end) : nP->end);     // in samples; packed, given in groups
    cP->loop = (cP->packed ? (3 * nP->loop) : nP->loop);
    cP->stride = nP->stride;
    for( i = 0; i < THRESHOLDS; i++ )
    {
        cP->thresholds[i] = nP->thresholds[i];
    }
    cP->step = nP->step;
    cP->attackRate = nP->attack;
    cP->decayRate = nP->decay;
    cP->releaseRate = nP->release;
    cP->start = nP->start;
    cP->peak = nP->peak;
    cP->sustain = nP->sustain;
    cP->celeste = edgeCeleste(nP->voicing);
    cP->step2 = edgeCelesteStep(nP->step, cP->celeste);
    cP->interp = ((nP->actions & ACT_INTERP) != 0);
    cP->slowRate = nP->slowRate;
    cP->vibratoDepth = nP->vibratoDepth;
    cP->vibratoDelay = nP->vibratoDelay;
    cP->slowFree = nP->slowFree;
    cP->tremoloDepth = nP->tremoloDepth;
    cP->panDepth = nP->panDepth;
    cP->percLevel = nP->percLevel;
    cP->percMultiple = nP->percMultiple;
    cP->percDecay = nP->percDecay;
    cP->percTable = nP->percTable;
    // The percussion triggers on a detached note only, as the Hammond's single trigger does.
    detached = ((cP->stage == STAGE_OFF) || (cP->stage == STAGE_RELEASE));

    if( nP->actions & ACT_PHASE )
    {
        cP->phase = 0;
        cP->phase2 = 0;
    }

    if( nP->actions & ACT_KEYON )
    {
        cP->attenuation = (cP->start << 15);
        cP->stage = STAGE_ATTACK;
        // The old note's samples would sound at the new note's level until the first words of
        // the new one land: they go, and so does a word of it still to come.
        cP->sample = 0.0f;
        cP->sample2 = 0.0f;
        cP->due = false;
        cP->due2 = false;
        cP->celesteOdd = false;
        pairClear(&cP->pairs[READ_FIRST]);
        pairClear(&cP->pairs[READ_SECOND]);
        dropOnChannel(cP, true, false);
        if( !cP->slowFree )
        {
            cP->slowPhase = 0;
            cP->slowHold = (cP->vibratoDelay * SLOW_HOLD);
        }
        if( (cP->percMultiple != 0) && detached )
        {
            cP->percPhase = 0;
            cP->percAtt = (cP->percLevel << 15);
            cP->percSample = 0.0f;
            pairClear(&cP->pairs[READ_PERC]);
            dropOnChannel(cP, false, true);
        }
    }

    if( (nP->actions & ACT_KEYOFF) && (cP->stage != STAGE_OFF) )
    {
        cP->stage = STAGE_RELEASE;
    }
    slowTerms(cP);

    rise = edgeLoad(&cP->edge, nP->voicing, nP->step, nP->period, nP->start, nP->peak, nP->attack,
                    ((nP->actions & ACT_KEYON) != 0));
    if( (rise > 0) && (cP->stage == STAGE_ATTACK) && (rise < cP->attackRate) )
    {
        cP->attackRate = rise;
    }
}

// A commit: the latches as they stand, the event length with them, into the queue.
static void
queueLatches(Converter *cP)
{
    cP->queuedEvent = cP->nextEvent;
    takeNote(cP, &cP->queued);
    cP->queueFull = true;
}

// What an end or a go loads: the event's length into eventLength and the note into *nP, from
// the queue if it is full (and then a waiting commit fills it), else from the latches.
static void
nextNote(Converter *cP, Note *nP)
{
    if( cP->queueFull )
    {
        cP->eventLength = (cP->queuedEvent ? cP->queuedEvent : EVENT_MAX);
        *nP = cP->queued;
        cP->queueFull = false;
        if( cP->commitWaiting )
        {
            cP->commitWaiting = false;
            queueLatches(cP);
        }
    }
    else
    {
        cP->eventLength = (cP->nextEvent ? cP->nextEvent : EVENT_MAX);
        takeNote(cP, nP);
    }
}

// Loads a converter's latches, at a go, at the start of a buffer or at the end of an event:
// the event's length, then the note.
static void
loadLatches(Converter *cP)
{
Note note;

    nextNote(cP, &note);
    cP->eventLeft = cP->eventLength;
    loadNote(cP, &note);
}

// The attenuation reaches the floor: the gain is 0, and the held samples are dropped, with the
// first read's and the céleste's pairs.
static void
envelopeOff(Converter *cP)
{
    cP->attenuation = FLOOR;
    cP->stage = STAGE_OFF;
    cP->sample = 0.0f;
    cP->sample2 = 0.0f;
    pairClear(&cP->pairs[READ_FIRST]);
    pairClear(&cP->pairs[READ_SECOND]);
}

// The percussion reaches the floor: it stops, its word and pair dropped.
static void
percOff(Converter *cP)
{
    cP->percAtt = FLOOR;
    cP->percSample = 0.0f;
    pairClear(&cP->pairs[READ_PERC]);
}

// A read's sample at a sample time: the word at the phase and, with the interpolation, the
// next (in sample mode back by the loop at the end), offset its table's address. A word the
// pair holds, landed, is kept; any other is due. Without the interpolation only the first is
// read, and always fetched. A packed read (not the percussion's) needs sample i's first word
// and, with the interpolation, the next sample's last, or without it i's own words, and keeps
// what it holds either way. The sample is formed at the next cycle.
static void
pairRead(Converter *cP, int r, int offset, uint32_t phase)
{
Pair *pP;
Pair old;
int index;
int next;
int need[2];
int words;
bool keep;
int k;
int j;

    pP = &cP->pairs[r];
    old = *pP;
    words = (cP->interp ? 2 : 1);
    keep = cP->interp;
    pP->interp = cP->interp;
    pP->place = -1;
    if( r == READ_PERC )
    {
        index = (int)((phase >> 12) & PERC_MASK);
        next = ((index + 1) & PERC_MASK);
        need[0] = ((offset + index) & CORE16MASK);
        need[1] = ((offset + next) & CORE16MASK);
    }
    else if( cP->packed )
    {
        index = (int)(phase >> 12);
        next = (((index + 1) >= cP->end) ? ((index + 1) - cP->loop) : (index + 1));
        pP->place = (index % 3);
        pP->nextPlace = (next % 3);
        need[0] = ((offset + (2 * (index / 3)) + ((pP->place == 2) ? 1 : 0)) & CORE16MASK);
        if( cP->interp )
        {
            need[1] = ((offset + (2 * (next / 3)) + ((pP->nextPlace != 0) ? 1 : 0)) & CORE16MASK);
        }
        else
        {
            need[1] = ((offset + (2 * (index / 3)) + 1) & CORE16MASK);
            words = ((pP->place == 1) ? 2 : 1);
        }
        keep = true;
    }
    else
    {
        index = (int)(phase >> 12);
        if( cP->sampled )
        {
            next = (((index + 1) >= cP->end) ? ((index + 1) - cP->loop) : (index + 1));
        }
        else
        {
            index &= cP->mask;
            next = ((index + 1) & cP->mask);
        }
        need[0] = ((offset + index) & CORE16MASK);
        need[1] = ((offset + next) & CORE16MASK);
    }

    for( k = 0; k < 2; k++ )
    {
        if( old.due[k] )
        {
            cP->late++;             // a word still waiting is simply replaced
        }
        pP->due[k] = false;
        pP->landed[k] = false;
        pP->address[k] = NO_ADDRESS;
        if( k >= words )
        {
            continue;
        }

        pP->address[k] = need[k];
        for( j = 0; keep && (j < 2); j++ )
        {
            if( old.landed[j] && (old.address[j] == need[k]) )
            {
                pP->value[k] = old.value[j];
                pP->landed[k] = true;
                break;
            }
        }
        pP->due[k] = !pP->landed[k];
    }

    pP->fraction = (int)(phase & 07777);
    pP->form = true;
}

// A word's sample, bits 0-11 as a ones' complement number; the low 6 bits never reach the
// converter.
static int
wordNumber(uint32_t word)
{
uint32_t magnitude;

    word &= WORDMASK;
    magnitude = ((word & SIGNBIT) ? ((~word) & WORDMASK) : word) >> 6;
    return( (word & SIGNBIT) ? -(int)magnitude : (int)magnitude );
}

// A packed sample at place p of its group, from the first and last words it lies in: p 0
// the first's bits 0-11, p 1 the first's bits 12-17 and the last's 0-5, p 2 the last's 6-17.
static int
packedNumber(int p, uint32_t first, uint32_t last)
{
uint32_t x;

    if( p == 0 )
    {
        x = (first >> 6);
    }
    else if( p == 1 )
    {
        x = (((first & 077) << 6) | ((last >> 12) & 077));
    }
    else
    {
        x = last;
    }
    x &= 07777;
    return( (x & 04000) ? -(int)((~x) & 03777) : (int)x );
}

// Forms a read's sample from its pair: interpolated, or without the interpolation its word.
// A packed pair holds sample i's first word and, with the interpolation, the next's last,
// which between them hold i's last and the next's first; without it, i's own words.
static void
formRead(Converter *cP, int r)
{
Pair *pP;
int s0;
int s1;
int v;
float value;

    pP = &cP->pairs[r];
    if( pP->place >= 0 )
    {
        s0 = packedNumber(pP->place, (uint32_t)pP->value[0],
                          (uint32_t)pP->value[(pP->place == 1) ? 1 : 0]);
        s1 = (pP->interp ? packedNumber(pP->nextPlace, (uint32_t)pP->value[(pP->nextPlace == 1) ? 0 : 1],
                                        (uint32_t)pP->value[1]) : s0);
    }
    else
    {
        s0 = wordNumber((uint32_t)pP->value[0]);
        s1 = (pP->interp ? wordNumber((uint32_t)pP->value[1]) : s0);
    }
    if( pP->interp )
    {
        v = (s0 + ((((s1 - s0) * pP->fraction) + 2048) >> 12));
    }
    else
    {
        v = s0;
    }

    value = ((float)v / FULLSCALE);
    if( r == READ_FIRST )
    {
        cP->sample = value;
    }
    else if( r == READ_SECOND )
    {
        cP->sample2 = value;
    }
    else
    {
        cP->percSample = value;
    }
}

// One machine cycle of the envelope: the stage moves the attenuation by its rate, and on
// reaching its target passes to the next stage.
static void
envelopeStep(Converter *cP)
{
int32_t target;

    switch( cP->stage )
    {
    case STAGE_ATTACK:
        target = (cP->peak << 15);
        if( (cP->attenuation - cP->attackRate) <= target )
        {
            cP->attenuation = target;
            cP->stage = STAGE_DECAY;
        }
        else
        {
            cP->attenuation -= cP->attackRate;
        }
        break;

    case STAGE_DECAY:
        target = ((cP->sustain == NO_SUSTAIN) ? FLOOR : (cP->sustain << 15));
        if( (cP->attenuation + cP->decayRate) >= target )
        {
            if( cP->sustain == NO_SUSTAIN )
            {
                envelopeOff(cP);
            }
            else
            {
                cP->attenuation = target;
                cP->stage = STAGE_SUSTAIN;
            }
        }
        else
        {
            cP->attenuation += cP->decayRate;
        }
        break;

    case STAGE_RELEASE:
        if( (cP->attenuation + cP->releaseRate) >= FLOOR )
        {
            envelopeOff(cP);
        }
        else
        {
            cP->attenuation += cP->releaseRate;
        }
        break;

    case STAGE_SUSTAIN:
    case STAGE_OFF:
        break;
    }
}

// The timbre at this sample: how many thresholds the attenuation, in the envelope's 1/8 dB,
// has reached; 0 when the stride is 0.
static int
timbre(Converter *cP)
{
int i;
int level;
int count;

    if( cP->stride == 0 )
    {
        return( 0 );
    }

    level = (cP->attenuation >> 15);
    count = 0;
    for( i = 0; i < THRESHOLDS; i++ )
    {
        if( (cP->thresholds[i] != NO_THRESHOLD) && (level >= cP->thresholds[i]) )
        {
            count++;
        }
    }

    return( count );
}

// A phase's table index: its integer part, masked to the table except in sample mode.
static int
phaseIndex(Converter *cP, uint32_t phase)
{
int index;

    index = (int)(phase >> 12);
    if( !cP->sampled )
    {
        index &= cP->mask;
    }
    return( index );
}

// A phase stepped: in sample mode, at or past the end it goes back by the loop's length.
static uint32_t
phaseStep(Converter *cP, uint32_t phase, int step)
{
    phase = ((phase + step) & PHASE_MASK);
    if( cP->sampled && ((int)(phase >> 12) >= cP->end) )
    {
        phase = ((phase - ((uint32_t)cP->loop << 12)) & PHASE_MASK);
    }
    return( phase );
}

// A sample time in wavetable mode: the word the phase points at, in the timbre the level has
// reached, is due, unless the envelope is OFF, and the phase steps either way. With the
// céleste, at every other such sample time from the key on the second phase's word is due
// too, and the second phase steps either way. With the interpolation each read is its pair's,
// and the céleste's at every sample time. Packed, each read is its pair's, a timbre 2 x the
// stride words on, and without the interpolation the céleste's at every other sample time.
// The percussion, while it sounds, reads its sine. The phases step by the steps the slow
// counter's terms give.
static void
waveSample(Converter *cP)
{
int offset;

    if( cP->stage != STAGE_OFF )
    {
        offset = (cP->address + ((cP->packed ? 2 : 1) * timbre(cP) * cP->stride));
        if( cP->interp || cP->packed )
        {
            pairRead(cP, READ_FIRST, offset, cP->phase);
            if( (cP->celeste != 0) && (cP->interp || !cP->celesteOdd) )
            {
                pairRead(cP, READ_SECOND, offset, cP->phase2);
            }
        }
        else
        {
            if( cP->due )
            {
                cP->late++;         // a sample still waiting is simply replaced
            }

            cP->fetchAddress = ((offset + phaseIndex(cP, cP->phase)) & CORE16MASK);
            cP->due = true;

            if( (cP->celeste != 0) && !cP->celesteOdd )
            {
                if( cP->due2 )
                {
                    cP->late++;
                }
                cP->fetchAddress2 = ((offset + phaseIndex(cP, cP->phase2)) & CORE16MASK);
                cP->due2 = true;
            }
        }

        if( cP->percAtt < FLOOR )
        {
            pairRead(cP, READ_PERC, cP->percTable, cP->percPhase);
        }
        cP->celesteOdd = !cP->celesteOdd;
    }

    cP->phase = phaseStep(cP, cP->phase, cP->stepTaken);
    cP->phase2 = phaseStep(cP, cP->phase2, cP->step2Taken);
    cP->percPhase = ((cP->percPhase + ((uint32_t)cP->percMultiple * (uint32_t)cP->stepTaken)) & PERC_PHASE);
}

// One machine cycle of the volume's counter: 1/64 dB toward the value set.
static void
volumeStep(Converter *cP)
{
    if( cP->volume < cP->volumeTarget )
    {
        cP->volume++;
    }
    else if( cP->volume > cP->volumeTarget )
    {
        cP->volume--;
    }
}

// One machine cycle of the place's two counters, as the volume's.
static void
placeStep(Converter *cP)
{
int ch;

    for( ch = 0; ch < 2; ch++ )
    {
        if( cP->place[ch] < cP->placeTarget[ch] )
        {
            cP->place[ch]++;
        }
        else if( cP->place[ch] > cP->placeTarget[ch] )
        {
            cP->place[ch]--;
        }
    }
}

// The attenuation, 1/64 dB, of a 9-bit value in 1/8 dB (a volume, a channel of a place):
// 0777 heads past the exponential converter's range, to silence.
static int
attenuationOf(int value)
{
    return( ((value & 0777) == MUTE) ? GAIN_STEPS : ((value & 0777) << 3) );
}

// The gain of steps 1/64 dB of attenuation: 0 past the exponential converter's range.
static float
gainOf(int steps)
{
    return( (steps < GAIN_STEPS) ? gains[steps] : 0.0f );
}

// A converter's analog output this cycle. Wavetable mode's level has the volume in its gain
// already; buffer mode's is the word, through the volume alone.
static float
output(Converter *cP)
{
    if( cP->wave )
    {
        return( cP->level );
    }

    return( cP->level * gainOf(cP->volume) );
}

// Forms a read's sample once its pair has every word it asked for.
static void
formIfIn(Converter *cP, int r)
{
Pair *pP;
int k;

    pP = &cP->pairs[r];
    if( !pP->form )
    {
        return;
    }
    for( k = 0; k < 2; k++ )
    {
        if( (pP->address[k] != NO_ADDRESS) && !pP->landed[k] )
        {
            return;
        }
    }

    pP->form = false;
    formRead(cP, r);
}

// A word has arrived for converter n, kind saying which read's (KIND_FIRST, KIND_SECOND, or a
// pair's word) from address: convert it, and in buffer mode step the buffer.
static void
convert(int n, uint32_t word, int kind, int address)
{
Converter *cP;
Pair *pP;
uint32_t magnitude;
int number;
int k;
float value;

    cP = &converters[n];
    if( !cP->running )
    {
        return;
    }

    // Bits 0-11 as a ones' complement number; the low 6 bits never reach the converter.
    word &= WORDMASK;
    magnitude = ((word & SIGNBIT) ? ((~word) & WORDMASK) : word) >> 6;
    number = ((word & SIGNBIT) ? -(int)magnitude : (int)magnitude);
    value = ((float)number / FULLSCALE);
    if( cP->wave )
    {
        if( kind >= KIND_PAIR )
        {
            // A pair's word lands if its slot still asks for that address; a read since has
            // replaced it otherwise.
            pP = &cP->pairs[(kind - KIND_PAIR) >> 1];
            k = ((kind - KIND_PAIR) & 1);
            if( !pP->landed[k] && !pP->due[k] && (pP->address[k] == address) )
            {
                pP->value[k] = (int)word;
                pP->landed[k] = true;
                formIfIn(cP, ((kind - KIND_PAIR) >> 1));
            }
        }
        else if( cP->stage != STAGE_OFF )
        {
            if( kind == KIND_SECOND )
            {
                cP->sample2 = value;
            }
            else
            {
                cP->sample = value;
            }
        }
        return;
    }

    if( kind != KIND_FIRST )
    {
        return;                     // a second read's word, after the switch to buffer mode
    }

    cP->level = value;
    cP->address = ((cP->address + 1) & CORE16MASK);
    cP->remaining--;
}

// Collects the word on the channel once it has moved. HSCwait() is only called then: a
// normal transfer advances on this thread, so waiting on a busy one would never end.
static void
collectFetch(void)
{
int status;

    if( fetching < 0 )
    {
        return;
    }

    status = HSCgetStatus(hscP);
    if( status == HSC_BUSY )
    {
        return;
    }

    HSCwait(hscP);
    if( status != HSC_DONE )
    {
        lostFetches++;
    }
    else if( !converters[fetching].dropFetch )
    {
        convert(fetching, fetchWord, fetchingKind, fetchingAddress);
    }

    converters[fetching].dropFetch = false;
    fetching = -1;
}

// The first word a converter has due, in the channel's order: its first read's, the first
// read's pair, the céleste's, the céleste's pair, the percussion's pair. Clears it and returns
// its kind and address, or returns -1 with nothing due.
static int
takeDue(Converter *cP, int *addressP)
{
int r;
int k;

    *addressP = 0;
    if( cP->due )
    {
        cP->due = false;
        *addressP = (cP->wave ? cP->fetchAddress : cP->address);
        return( KIND_FIRST );
    }
    if( !cP->wave )
    {
        return( -1 );
    }

    for( r = 0; r < READS; r++ )
    {
        if( (r == READ_SECOND) && cP->due2 )
        {
            cP->due2 = false;
            *addressP = cP->fetchAddress2;
            return( KIND_SECOND );
        }
        for( k = 0; k < 2; k++ )
        {
            if( cP->pairs[r].due[k] )
            {
                cP->pairs[r].due[k] = false;
                *addressP = cP->pairs[r].address[k];
                return( KIND_PAIR + (2 * r) + k );
            }
        }
    }

    return( -1 );
}

// Whether a converter has a word due.
static bool
anyDue(Converter *cP)
{
int r;

    if( cP->due )
    {
        return( true );
    }
    if( !cP->wave )
    {
        return( false );
    }
    if( cP->due2 )
    {
        return( true );
    }
    for( r = 0; r < READS; r++ )
    {
        if( cP->pairs[r].due[0] || cP->pairs[r].due[1] )
        {
            return( true );
        }
    }

    return( false );
}

// Puts the lowest numbered converter with a read due on the channel, its words in takeDue()'s
// order; with hold, leaves it due.
// Returns FETCH_STARTED (also for a request the channel refused), FETCH_HELD or FETCH_NONE.
static int
startFetch(bool hold)
{
int n;
int address;
int kind;
Converter *cP;

    if( fetching >= 0 )
    {
        return( FETCH_NONE );
    }

    for( n = 0; n < CONVERTERS; n++ )
    {
        cP = &converters[n];
        if( !cP->running || !anyDue(cP) )
        {
            continue;
        }

        if( hold )
        {
            return( FETCH_HELD );
        }

        kind = takeDue(cP, &address);
        request.mode = HSC_MODE_FROMMEM;
        request.count = 1;
        request.memBank = (address >> 12);
        request.memAddr = (address & 07777);
        request.wordTime = 0;
        request.fromBufferP = &fetchWord;
        request.toBufferP = NULL;
        if( HSCexecute(hscP, &request) == HSC_BUSY )
        {
            fetching = n;
            fetchingKind = kind;
            fetchingAddress = address;
        }
        else
        {
            lostFetches++;
        }

        return( FETCH_STARTED );
    }

    return( FETCH_NONE );
}

// The channel's poll through the window timer: a read starts unless WINDOW_BUSY polls have
// passed without WINDOW_FREE free in a row, and then none does until there have been.
// A refused request counts as started: the device does not know it stole nothing.
static void
serveChannel(void)
{
int did;

    did = startFetch((busyPolls >= WINDOW_BUSY));
    if( did == FETCH_HELD )
    {
        heldDue = true;
    }

    freePolls = ((did == FETCH_STARTED) ? 0 : (freePolls + 1));
    if( freePolls >= WINDOW_FREE )
    {
        if( heldDue )
        {
            windows++;
        }
        heldDue = false;
        busyPolls = 0;
    }
    else
    {
        busyPolls++;
    }
}

// Adds this cycle's outputs to the audio sample being built, and queues it when complete.
// Each converter reaches both channels through its place's gains, summed in converter
// order, so at the power-on places (gains of exactly 1 and 0) the extra terms add exact
// zeros. A converter not running is skipped: its level is 0 (a halt and the stop set it
// so), so the sums are the same.
static void
mixOutput(void)
{
float left;
float right;
float out;
float cycleLeft;
float cycleRight;
bool clipped;
int64_t dryLeft;
int64_t dryRight;
int64_t wetLeft;
int64_t wetRight;
int n;

    cycleLeft = 0.0f;
    cycleRight = 0.0f;
    for( n = 0; n < CONVERTERS; n++ )
    {
        if( !converters[n].running )
        {
            continue;
        }
        out = output(&converters[n]);
        if( converters[n].wave )
        {
            // The Leslie's place moves with the slow counter, its offsets added to the place's.
            cycleLeft += (out * gainOf(converters[n].place[0] + converters[n].panOffset[0]));
            cycleRight += (out * gainOf(converters[n].place[1] + converters[n].panOffset[1]));
        }
        else
        {
            cycleLeft += (out * gainOf(converters[n].place[0]));
            cycleRight += (out * gainOf(converters[n].place[1]));
        }
    }
    sumLeft += cycleLeft;
    sumRight += cycleRight;
    if( ++outCycles < CYCLES_PER_OUT )
    {
        return;
    }

    left = ((sumLeft / (float)(2 * CYCLES_PER_OUT)) * mixGain * 32767.0f);
    right = ((sumRight / (float)(2 * CYCLES_PER_OUT)) * mixGain * 32767.0f);
    sumLeft = 0.0f;
    sumRight = 0.0f;
    outCycles = 0;

    // The room takes the frame in 1/256 of a step; the frame heard, dry and wet cut by the
    // room's trim, comes back in the same units before the clip (exact in a float below 2^24,
    // and past that it clips anyway).
    if( roomKind != ROOM_OFF )
    {
        dryLeft = (int64_t)lrintf(left * 256.0f);
        dryRight = (int64_t)lrintf(right * 256.0f);
        roomFrame(&room, dryLeft, dryRight, &wetLeft, &wetRight);
        left = ((float)roomHeard(&room, dryLeft, wetLeft) / 256.0f);
        right = ((float)roomHeard(&room, dryRight, wetRight) / 256.0f);
    }

    clipped = false;
    if( left > 32767.0f )
    {
        left = 32767.0f;
        clipped = true;
    }
    else if( left < -32768.0f )
    {
        left = -32768.0f;
        clipped = true;
    }

    if( right > 32767.0f )
    {
        right = 32767.0f;
        clipped = true;
    }
    else if( right < -32768.0f )
    {
        right = -32768.0f;
        clipped = true;
    }

    if( clipped )
    {
        clippedFrames++;
    }

    batch[2 * batchFrames] = (int16_t)left;
    batch[(2 * batchFrames) + 1] = (int16_t)right;
    if( ++batchFrames < BATCH_FRAMES )
    {
        return;
    }

    audioQueueFrames(batch, batchFrames);
    queuedFrames += batchFrames;
    batchFrames = 0;
    if( !isPrimed && (queuedFrames >= PRELOAD_FRAMES) )
    {
        isPrimed = true;
        audioPauseDevice(false);
    }
}

// IOT 72: latches wavetable mode's values (they load with the others), or reads its status.
static void
waveCommand(PDP1 *pdp1P, Converter *cP, int command)
{
int value;

    value = (IO(pdp1P) & WORDMASK);
    switch( command )
    {
    case CMD_STEP:
        cP->nextStep = value;
        break;

    case CMD_EVENT:
        cP->nextEvent = (value ? value : EVENT_MAX);
        break;

    case CMD_ATTACK:
        cP->nextAttack = value;
        break;

    case CMD_DECAY:
        cP->nextDecay = value;
        break;

    case CMD_RELEASE:
        cP->nextRelease = value;
        break;

    case CMD_LEVELS:
        cP->nextStart = ((value >> 9) & 0777);
        cP->nextPeak = (value & 0777);
        break;

    case CMD_SUSTAIN:
        cP->nextSustain = ((value >> 9) & 0777);
        cP->nextActions = (value & ACT_MASK);
        break;

    case CMD_WAVESTATUS:
        IO(pdp1P) = ((((int)cP->stage) << 15) | ((cP->attenuation >> 12) & 07777));
        break;
    }
}

// IOT 72's commands 011-016: latches the slow counter's and the percussion's values (they load
// with the others).
static void
slowCommand(PDP1 *pdp1P, Converter *cP, int command)
{
int value;
Note *nP;

    value = (IO(pdp1P) & WORDMASK);
    nP = &cP->nextSlow;
    switch( command )
    {
    case CMD_SLOWRATE:
        nP->slowRate = value;
        break;

    case CMD_VIBRATO:
        nP->vibratoDepth = ((value >> 9) & 0777);
        nP->vibratoDelay = ((value >> 1) & 0377);
        nP->slowFree = (value & 1);
        break;

    case CMD_LESLIE:
        nP->tremoloDepth = ((value >> 9) & 0777);
        nP->panDepth = (value & 0777);
        break;

    case CMD_PERCLEVEL:
        nP->percLevel = ((value >> 9) & 0777);
        nP->percMultiple = (value & 07);
        break;

    case CMD_PERCDECAY:
        nP->percDecay = value;
        break;

    case CMD_PERCTABLE:
        nP->percTable = (value & CORE16MASK);
        break;
    }
}

// IOT 71's commands 8-15: latches sample mode's and the timbre's values (they load with the
// others), sets the volume or the place, or reads the phase.
static void
sampleCommand(PDP1 *pdp1P, Converter *cP, int command)
{
int value;

    value = (IO(pdp1P) & WORDMASK);
    switch( command )
    {
    case CMD_END:
        cP->nextEnd = ((value & CORE16MASK) ? (value & CORE16MASK) : (CORE16MASK + 1));
        break;

    case CMD_LOOP:
        cP->nextLoop = (value & CORE16MASK);
        break;

    case CMD_STRIDE:
        cP->nextStride = (value & CORE16MASK);
        break;

    case CMD_THRESH12:
        cP->nextThresholds[0] = ((value >> 9) & 0777);
        cP->nextThresholds[1] = (value & 0777);
        break;

    case CMD_THRESH3:
        cP->nextThresholds[2] = ((value >> 9) & 0777);
        cP->nextVoicing = (value & EDGE_VOICING);
        break;

    case CMD_VOLUME:
        // Not a latch: the counter heads for it now. A converter not running is silent, so
        // its volume is there at once, and a note it starts does not slide from the old one.
        cP->volumeTarget = attenuationOf(value);
        if( !cP->running )
        {
            cP->volume = cP->volumeTarget;
        }
        break;

    case CMD_PLACE:
        // Not a latch, as the volume: each channel's counter heads for its value now, and a
        // converter not running takes it at once.
        cP->placeTarget[0] = attenuationOf(value >> 9);
        cP->placeTarget[1] = attenuationOf(value);
        if( !cP->running )
        {
            cP->place[0] = cP->placeTarget[0];
            cP->place[1] = cP->placeTarget[1];
        }
        break;

    case CMD_PHASESTATUS:
        IO(pdp1P) = (int)((cP->phase >> 12) & CORE16MASK);
        break;
    }
}

// With dacroom=voices, at a go: the room is Busch Hall if a running converter's note in
// wavetable mode has the voicing's bit for it, the hall otherwise; a room changed starts
// empty, and the log names it.
static void
roomFromVoices(void)
{
int n;
int kind;

    kind = ROOM_HALL;
    for( n = 0; n < CONVERTERS; n++ )
    {
        if( converters[n].running && converters[n].wave && ((converters[n].edge.voicing & EDGE_BUSCH) != 0) )
        {
            kind = ROOM_BUSCH;
        }
    }

    if( kind != room.kind )
    {
        roomInit(&room, kind);
        iotCondLog(LOG_DAC_CHANGES, "IOT 71: room %s, from the voices\n", ((kind == ROOM_BUSCH) ? "busch" : "hall"));
    }
}

// The room a program sets, at once: a fixed room changed starts empty and none clears it, as
// a halt's restart does; the voices' choice keeps the room playing, the hall if there was
// none, until the next go chooses. A setting the same as before changes nothing.
static void
setRoom(int kind)
{
    if( kind == roomKind )
    {
        return;
    }

    roomKind = kind;
    if( kind == ROOM_VOICES )
    {
        if( room.kind == ROOM_OFF )
        {
            roomInit(&room, ROOM_HALL);
        }
    }
    else if( kind != room.kind )
    {
        roomInit(&room, kind);
    }
    iotCondLog(LOG_DAC_CHANGES, "IOT 71: room %s, set by the program\n",
            ((kind == ROOM_BUSCH) ? "busch" : ((kind == ROOM_HALL) ? "hall" :
             ((kind == ROOM_VOICES) ? "from the voices" : "off"))));
}

// IOT 72's command 017, SET_ROOM, the onset, SET_ONSET, get the current values, SET_READ.
// If IO bit 0 is set, the setting reverts to the configure file or defaut.
// A changed bound applies from the next event's end.
// SET_READ returns the current settings. packed, in IO.
static void
settingsCommand(PDP1 *pdp1P, int which)
{
static const int roomOfCode[4] = { ROOM_OFF, ROOM_HALL, ROOM_BUSCH, ROOM_VOICES };
int value;
int cycles;
int units;

    value = (IO(pdp1P) & WORDMASK);
    switch( which )
    {
    case SET_ROOM:
        setRoom(((value & SET_CONFIGURED)?configRoomKind:roomOfCode[value & 03]));
        break;

    case SET_ONSET:
        cycles = ((value & SET_CONFIGURED)?configOnsetCycles:((value & ONSET_UNITS_MAX) * ONSET_UNIT));
        if( cycles != onsetCycles )
        {
            onsetCycles = cycles;
            iotCondLog(LOG_DAC_CHANGES, "IOT 71: onset %d cycles, set by the program\n", onsetCycles);
        }
        break;

    case SET_READ:
        units = ((onsetCycles + (ONSET_UNIT / 2)) / ONSET_UNIT);
        if( units > ONSET_UNITS_MAX )
        {
            units = ONSET_UNITS_MAX;
        }

        IO(pdp1P) = ((((roomKind == ROOM_VOICES)?ROOM_CODE_VOICES:roomKind) << 15) |
            (((roomKind == ROOM_OFF)?ROOM_OFF:room.kind) << 12) | units);
        break;
    }
}

// Handles every converter command at TP10, IOT 71's and IOT 72's. Returns 1 (the IOT is
// known), or 0 if the high speed channel could not be had.
int
iotHandler(PDP1 *pdp1P, int dev, int pulse, int completion)
{
int field;
int command;
Converter *cP;

    if( !pulse )
    {
        return( 1 );
    }

    if( !setUp() )
    {
        return( 0 );
    }

    field = ((MB(pdp1P) >> 6) & 077);
    cP = &converters[field & 03];
    command = (field >> 2);
    if( dev == WAVE_DEV )
    {
        // Commands 010-017 have bit 6 set and give no completion, as IOT 71's 8-15 (below).
        if( command >= CMD_COMMIT )
        {
            if( command == CMD_COMMIT )
            {
                if( cP->queueFull )
                {
                    cP->commitWaiting = true;
                }
                else
                {
                    queueLatches(cP);
                }
            }
            else if( command == CMD_SETTINGS )
            {
                settingsCommand(pdp1P, (field & 03));
            }
            else
            {
                slowCommand(pdp1P, cP, command);
            }
            return( 1 );
        }

        waveCommand(pdp1P, cP, command);
        IOCOMPLETE_IFNEEDED(pdp1P, completion);
        return( 1 );
    }

    // Bit 6 is the CPU's completion request too (nac), so a command with it set asks for a
    // completion it does not want: one given would set ios, and a later ioh meant for
    // another device would pass without waiting. With i as well the CPU waits for ever, so
    // dacdefs.ah forbids i on every DAC instruction.
    if( command >= CMD_END )
    {
        sampleCommand(pdp1P, cP, command);
        return( 1 );
    }

    switch( command )
    {
    case CMD_ADDRESS:
        cP->nextAddress = (IO(pdp1P) & CORE16MASK);
        break;

    case CMD_LENGTH:
        cP->nextLength = (IO(pdp1P) & 07777);
        if( cP->nextLength == 0 )
        {
            cP->nextLength = 4096;
        }
        break;

    case CMD_PERIOD:
        cP->nextPeriod = (IO(pdp1P) & 07777);
        if( cP->nextPeriod == 0 )
        {
            cP->nextPeriod = 4096;
        }
        break;

    case CMD_GO:
        edgeInit(&cP->edge, (int)(cP - converters));
        cP->ticks = 0;
        cP->celesteOdd = false;
        cP->due2 = false;
        cP->slowPhase = 0;
        cP->slowHold = 0;
        percOff(cP);
        pairClear(&cP->pairs[READ_FIRST]);
        pairClear(&cP->pairs[READ_SECOND]);
        loadLatches(cP);
        cP->onsetLeft = 0;
        cP->onsetRandom = ((uint32_t)((cP - converters) + 1) * ONSET_SEED);
        cP->countdown = cP->period;
        cP->due = false;
        if( cP->wave )
        {
            waveSample(cP);         // the first sample is fetched at once
        }
        else
        {
            cP->due = true;         // the first word is fetched at once
        }
        cP->flag = false;
        cP->running = true;
        if( roomKind == ROOM_VOICES )
        {
            roomFromVoices();
        }
        break;

    case CMD_HALT:
        cP->running = false;
        cP->due = false;
        cP->due2 = false;
        pairsIdle(cP);
        cP->level = 0.0f;
        cP->queueFull = false;
        cP->commitWaiting = false;
        break;

    case CMD_SKIP:
        if( cP->flag )
        {
            cP->flag = false;
            PC(pdp1P) = ((PC(pdp1P) + 1) & 07777);
        }
        break;

    case CMD_BREAK:
        cP->breakEnabled = ((IO(pdp1P) & SIGNBIT) != 0);
        cP->breakChannel = (IO(pdp1P) & 017);
        break;

    case CMD_STATUS:
        IO(pdp1P) = ((cP->running ? SIGNBIT : 0) | (cP->flag ? 0200000 : 0) | (cP->remaining & 017777));
        break;
    }

    IOCOMPLETE_IFNEEDED(pdp1P, completion);
    return( 1 );
}

// Starts a new buffer or event: the latches load, and the flag sets (with a break if asked).
static void
newBuffer(Converter *cP)
{
    loadLatches(cP);
    cP->flag = true;
    if( cP->breakEnabled )
    {
        initiateBreak(cP->breakChannel);
    }
}

// An event's end with the onset on: the timer reloads and the flag sets now; the note is
// taken now and loads d cycles later, d the sequence's next number up to the bound and never
// past half the new event. A note that is due at once, or that leaves wavetable mode, loads now.
static void
lateEvent(Converter *cP)
{
uint32_t x;
int d;

    nextNote(cP, &cP->held);
    cP->eventLeft = cP->eventLength;

    x = cP->onsetRandom;
    x ^= (x << 13);
    x ^= (x >> 17);
    x ^= (x << 5);
    cP->onsetRandom = x;
    d = (int)(x % (uint32_t)(onsetCycles + 1));
    if( d > (cP->eventLength / 2) )
    {
        d = (cP->eventLength / 2);
    }

    if( (d == 0) || !(cP->held.actions & ACT_WAVE) )
    {
        loadNote(cP, &cP->held);
    }
    else
    {
        cP->onsetLeft = d;
    }

    cP->flag = true;
    if( cP->breakEnabled )
    {
        initiateBreak(cP->breakChannel);
    }
}

// One machine cycle of a converter in wavetable mode: the samples the last sample time's pairs
// formed, the envelope, the percussion's decay and the breath, a note the onset holds, the
// event timer, the sample clock, every fourth cycle the noise, every 64th the slow counter,
// and the output, in that order.
static void
waveCycle(Converter *cP)
{
int base;
int r;

    for( r = 0; r < READS; r++ )
    {
        formIfIn(cP, r);
    }

    envelopeStep(cP);
    if( cP->percAtt < FLOOR )
    {
        if( (cP->percAtt + cP->percDecay) >= FLOOR )
        {
            percOff(cP);
        }
        else
        {
            cP->percAtt += cP->percDecay;
        }
    }
    edgePoll(&cP->edge);
    if( (cP->onsetLeft > 0) && (--cP->onsetLeft == 0) )
    {
        loadNote(cP, &cP->held);
    }

    if( --cP->eventLeft <= 0 )
    {
        if( onsetCycles > 0 )
        {
            lateEvent(cP);
        }
        else
        {
            newBuffer(cP);
        }
    }

    if( --cP->countdown <= 0 )
    {
        cP->countdown = cP->period;
        if( cP->wave )
        {
            waveSample(cP);
        }
        else
        {
            // The event switched the converter to buffer mode: a buffer starts at its sample time.
            if( cP->due )
            {
                cP->late++;
            }
            cP->due = true;
        }
    }

    if( (++cP->ticks % EDGE_TICK) == 0 )
    {
        edgeTick(&cP->edge);
    }

    // The slow counter: the delay's clocks first, then the rate's; the terms follow it.
    if( (cP->ticks % SLOW_TICK) == 0 )
    {
        if( cP->slowHold > 0 )
        {
            cP->slowHold--;
        }
        else
        {
            cP->slowPhase = ((cP->slowPhase + (uint32_t)cP->slowRate) & SLOW_PHASE);
        }
        slowTerms(cP);
    }

    if( cP->wave )
    {
        if( cP->stage == STAGE_OFF )
        {
            cP->level = 0.0f;
            return;
        }

        // The sample, then the edge tone and the breath, each through the envelope and the
        // volume, then its own level (and the breath its decay), each a gain of its own:
        // a band's level alone takes up to 50 dB of the table's 64, so in one index the
        // envelope would silence it while the note still sounds.
        // With the céleste the sample is the two reads' words at equal power.
        base = (((cP->attenuation >> 12) & 07777) + cP->volume + cP->tremolo);
        if( cP->celeste != 0 )
        {
            cP->level = (((cP->sample + cP->sample2) * CELESTE_MIX) * gainOf(base));
        }
        else
        {
            cP->level = (cP->sample * gainOf(base));
        }
        if( (cP->edge.voicing & EDGE_MASK) != 0 )
        {
            cP->level += ((((float)cP->edge.edge.y * EDGE_UNIT) * gainOf(base)) * gainOf(cP->edge.edgeLevel));
        }
        if( (cP->edge.voicing & EDGE_RISE) != 0 )
        {
            cP->level += (((((float)cP->edge.breath.y * EDGE_UNIT) * gainOf(base)) * gainOf(cP->edge.breathLevel))
                          * gainOf(cP->edge.breathAtt >> 12));
        }
        if( cP->percAtt < FLOOR )
        {
            cP->level += ((cP->percSample * gainOf(base)) * gainOf(cP->percAtt >> 12));
        }
    }
}

// Once per machine cycle: finish the word on the channel, clock the converters, put the
// next due one on the channel unless the window timer holds it, and add the outputs to the
// audio.
void
iotIOPoll(PDP1 *pdp1P)
{
int n;
Converter *cP;

    if( !hscP || !pdp1P->run )
    {
        return;
    }

    collectFetch();
    for( n = 0; n < CONVERTERS; n++ )
    {
        cP = &converters[n];
        if( cP->running )
        {
            volumeStep(cP);
            placeStep(cP);
        }

        if( cP->running && cP->wave )
        {
            waveCycle(cP);
            continue;
        }

        if( cP->running && (--cP->countdown <= 0) )
        {
            if( cP->remaining <= 0 )
            {
                newBuffer(cP);
            }

            cP->countdown = cP->period;
            if( cP->wave )
            {
                waveSample(cP);     // the buffer's end switched the converter to wavetable mode
                continue;
            }

            if( cP->due )
            {
                cP->late++;         // a sample still waiting is simply replaced
            }
            cP->due = true;
        }
    }

    serveChannel();
    if( isPlaying )
    {
        mixOutput();
    }
}

// The machine has started or continued: play from an empty queue.
void
iotStart(void)
{
    if( hscP )
    {
        startPlaying();
    }
}

// The machine has stopped. Like a stopped channel transfer, the converters stop, and the
// audio stops with them; their queues empty, as a halt empties them. A word left on the
// channel is collected, and dropped, after the next start.
void
iotStop(void)
{
int n;

    for( n = 0; n < CONVERTERS; n++ )
    {
        converters[n].running = false;
        converters[n].due = false;
        converters[n].due2 = false;
        pairsIdle(&converters[n]);
        converters[n].level = 0.0f;
        converters[n].queueFull = false;
        converters[n].commitWaiting = false;
    }

    iotCondLog(LOG_DAC_COUNTS, "IOT 71: late samples %d %d %d %d, lost fetches %d, clipped frames %d, windows %d\n",
            converters[0].late, converters[1].late, converters[2].late, converters[3].late, lostFetches,
            clippedFrames, windows);
    clippedFrames = 0;
    windows = 0;
    freePolls = 0;
    busyPolls = 0;
    heldDue = false;
    if( audioDev != AUDIO_HANDLE_INVALID )
    {
        audioPauseDevice(true);
        audioClearQueue();
    }

    isPlaying = false;
}
