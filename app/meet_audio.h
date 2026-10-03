// The meeting assistant's sound on WASAPI. It hears the meeting app (per-app loopback, app/loopback.c, Windows 10 2004
// and later) and the user's microphone (miniaudio); it speaks into VB-Audio's virtual cable, which the meeting app takes as its
// microphone, mixed with the user's own voice so the meeting still hears them. Its voice never plays on this computer's
// speakers, where the microphone would hear it. Everything runs at 24 kHz mono 16-bit, the rate both OpenAI voice APIs use.
#ifndef BRIAREUS_MEET_AUDIO_H
#define BRIAREUS_MEET_AUDIO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// A running app the assistant can listen to: a meeting app or a browser, by its root process.
typedef struct { char *label; unsigned pid; } MeetApp;
MeetApp *meet_apps_running(size_t *count);
void meet_apps_free(MeetApp *apps, size_t count);

/// The virtual cable's input as Windows names it, or NULL when VB-Cable is not installed.
char *meet_cable_name(void);

typedef struct MeetAudio MeetAudio;
/// Starts hearing app `pid` (0: every app but Briareus) and the microphone, and speaking into the cable. NULL with
/// `*error` when a device cannot open. `*note` says what
/// works without being fatal (no microphone found), or stays NULL.
MeetAudio *meet_audio_start(unsigned pid, char **error, char **note);
void meet_audio_stop(MeetAudio *a);
/// The next `samples` of what the assistant hears, the meeting and the microphone mixed; silence where nothing came.
void meet_audio_take_input(MeetAudio *a, int16_t *out, size_t samples);
/// Queues the assistant's speech for the cable.
void meet_audio_play(MeetAudio *a, const int16_t *pcm, size_t samples);
/// Drops speech not yet played, when someone starts talking over it.
void meet_audio_flush(MeetAudio *a);
/// A muted assistant is not heard in the meeting; its speech is dropped.
void meet_audio_set_muted(MeetAudio *a, bool muted);
/// Whether its speech is still playing.
bool meet_audio_speaking(MeetAudio *a);

#endif
