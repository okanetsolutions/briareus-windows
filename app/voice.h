// One voice note at a time: recorded from the microphone, transcribed by the server, its text handed to a composer.
#ifndef BRIAREUS_VOICE_H
#define BRIAREUS_VOICE_H
#include <windows.h>
#include <stdbool.h>

typedef enum { VOICE_IDLE, VOICE_STARTING, VOICE_RECORDING, VOICE_TRANSCRIBING } VoiceState;
typedef struct VoiceNote VoiceNote;
/// Called on the UI thread whenever the state, the text or the error changes.
typedef void (*VoiceChanged)(void *ctx);

VoiceNote *voice_new(VoiceChanged changed, void *ctx);
void voice_free(VoiceNote *note);
VoiceState voice_state(const VoiceNote *note);
/// Seconds recorded so far.
int voice_elapsed(const VoiceNote *note);
/// Past this a note stops by itself and is transcribed: a forgotten microphone would record on.
#define VOICE_LIMIT_SECONDS 300
/// Asks the server whether it transcribes, then records. Errors are reported through the callback.
void voice_record(VoiceNote *note);
/// Ends the recording; its text is on the way once encoded and transcribed.
void voice_stop(VoiceNote *note);
/// Throws the note away, recorded or on its way to the server.
void voice_drop(VoiceNote *note);
/// Stops a recording that has reached the limit; call once a second while recording.
void voice_tick(VoiceNote *note);
/// The transcribed text, once; NULL until it arrives.
char *voice_take_text(VoiceNote *note);
char *voice_take_error(VoiceNote *note);

#endif
