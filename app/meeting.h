// The meeting assistant: one meeting at a time, joined from a project, held by an ElevenLabs agent speaking in the user's
// voice, which looks the project up with read-only tools (its conversations, pull requests, findings and issues). It
// runs on until left, whatever screen is shown. Its settings and the ElevenLabs API key are this computer's, and every
// meeting is recorded here with its time and cost.
#ifndef BRIAREUS_MEETING_H
#define BRIAREUS_MEETING_H
#include "meet.h"
#include "models.h"
#include <windows.h>
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
    char *wake_words;    // what makes an addressed assistant answer
    char *eleven_voice;  // the ElevenLabs voice ID it speaks with
    bool introduce;      // it says who it is as it joins
    bool independent;    // it takes part on its own
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
/// Joins a meeting about `project` with the ElevenLabs agent, listening to app `pid` (0: every app but Briareus) called `source`.
/// Says why in an alert and returns false when it cannot start.
bool meeting_join(const Project *project, unsigned pid, const char *source);
void meeting_leave(void);
MeetingState meeting_state(void);
/// Whether the meeting is about this project.
bool meeting_for(const char *repo);
void meeting_set_muted(bool muted);
bool meeting_muted(void);
/// Asks the assistant to answer what was just said.
void meeting_answer_now(void);
/// "🎙 ElevenLabs agent · Zoom · 3:12 · $0.26 · listening", for the project's header. New string.
char *meeting_status(void);
/// The meeting's transcript so far. New string.
char *meeting_transcript(void);
/// 🎙 Meet's menu at `pt`: join a meeting about `project`, listening to a meeting app; or, in a meeting, mute, answer
/// now, copy the transcript or leave. Returns whether anything was chosen.
bool meeting_menu(const Project *project, HWND owner, POINT pt);
/// The app quits: the meeting is left at once.
void meeting_shutdown(void);

#endif
