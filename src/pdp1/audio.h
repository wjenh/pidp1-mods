#ifndef AUDIO_H
#define AUDIO_H
// The audio output's settings and counters (audio.c), for callers off the emulator thread.
// The emulator thread's calls, initaudio() and the rest, are in pdp1.h.

#include <stdbool.h>

extern bool audioEnabled;       // the audio on/off setting; defined in pdp1.c

void setSampleRate(int perSec);
int getSampleRate(void);
void setFilterCutoff(int voiceNum, float hz);
float getFilterCutoff(int voiceNum);
float alphaToCutoff(float alpha);
void setMixerGain(float newGain);
float getMixerGain(void);
void setAudioTuning(float newTuning);
float getAudioTuning(void);
int getOverflowData(int *rsltP);
void postaudio(bool on);
#endif
