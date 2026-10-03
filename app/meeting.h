// The meeting assistant: one meeting at a time, joined from a conversation, whose agent answers what the voice model
// cannot. It runs on until left, whatever screen is shown. Its settings and API keys (OpenAI, and ElevenLabs for the
// user's own voice) are this computer's, and every meeting is recorded here with its time and costs so the two models
// can be compared.
#ifndef BRIAREUS_MEETING_H
#define BRIAREUS_MEETING_H
#include "meet.h"
#include "models.h"
#include <stdbool.h>

// MARK: - Settings

typedef enum { MEETING_KEY_OPENAI, MEETING_KEY_ELEVENLABS } MeetingKey;
/// An API key, in Windows Credential Manager; NULL without one (`*failed` when the store could not be read).
char *meeting_key_read(MeetingKey which, bool *failed);
bool meeting_key_save(MeetingKey which, const char *key);
bool meeting_key_remove(MeetingKey which);
/// Whether a key is saved, without reading it out.
bool meeting_has_key(MeetingKey which);

typedef struct {
    char *name;          // who the assistant speaks for
    char *wake_words;    // what makes an addressed Realtime assistant answer
    char *voice;
    char *eleven_voice;  // the ElevenLabs voice ID GPT-Realtime speaks with; empty for the OpenAI voice
    bool introduce;      // it says who it is as it joins
    bool independent;    // it takes part on its own and may have the agent make changes
} MeetingSettings;
void meeting_settings_load(MeetingSettings *s);
void meeting_settings_save(const MeetingSettings *s);
void meeting_settings_free(MeetingSettings *s);
/// The Windows user name, the name's default.
char *meeting_default_name(void);

/// Every meeting recorded on this computer, oldest first, as meet_record_json objects.
Json *meeting_history(void);
void meeting_history_clear(void);

// MARK: - The meeting

typedef enum { MEETING_OFF, MEETING_CONNECTING, MEETING_LIVE, MEETING_LEAVING } MeetingState;
/// Joins with `model`, listening to app `pid` (0: every app but Briareus) called `source`. Says why in an alert and
/// returns false when it cannot start.
bool meeting_join(const Session *session, MeetModel model, unsigned pid, const char *source);
void meeting_leave(void);
MeetingState meeting_state(void);
/// Whether the meeting runs for this conversation.
bool meeting_for(const char *session_id);
MeetModel meeting_model(void);
void meeting_set_muted(bool muted);
bool meeting_muted(void);
/// Asks the assistant to answer what was just said.
void meeting_answer_now(void);
/// "GPT-Live 1 · 3:12 · $0.16 · listening", for the conversation's header. New string.
char *meeting_status(void);
/// The meeting's transcript so far. New string.
char *meeting_transcript(void);
/// The app quits: the meeting is left at once.
void meeting_shutdown(void);

#endif
