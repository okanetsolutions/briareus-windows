#ifndef BRIAREUS_SCREEN_MEETING_SETTINGS_INTERNAL_H
#define BRIAREUS_SCREEN_MEETING_SETTINGS_INTERNAL_H
#include "meet_audio.h"
#include "meeting.h"
#include "screens.h"
#include "str.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ⚙ Settings → Meeting assistant: this computer's settings for the assistant that joins meetings from a project
// (🎙 Meet): the ElevenLabs API key, kept in Windows Credential Manager, the user's ElevenLabs voice, who it speaks for
// and how, whether the virtual microphone is installed, and the time and cost of every meeting recorded here.

enum { F_KEY, F_VOICE, F_NAME, F_WAKE, F_COUNT };

enum { T_INDEPENDENT, T_INTRODUCE, T_COUNT };

enum { ACT_SAVE = 1200, ACT_REMOVE_KEY, ACT_TOGGLE, ACT_FOCUS, ACT_CLEAR_HISTORY, ACT_CHECK_CABLE, ACT_REMOVE_OPENAI_KEY };

enum { ID_FIELD = 2300 };

typedef struct {
    Screen base;
    MeetingSettings saved;
    bool toggles[T_COUNT];
    bool has_key, has_openai_key;   // the ElevenLabs key, and the OpenAI one kept from before the agent
    char *cable;            // the virtual cable's name, or NULL when it is missing
    MeetTotals totals[MEET_MODEL_COUNT];
    HWND edits[F_COUNT];
    RECT rects[F_COUNT];
    bool laid[F_COUNT], clipped[F_COUNT];
    bool dirty, filling, shown;
    int focused;
    char *message; bool message_ok;
} MeetingForm;

char *screen_meeting_settings_edit_text(HWND edit);
void screen_meeting_settings_form_fill(MeetingForm *s);
void screen_meeting_settings_form_layout(Screen *base, Doc *doc);
void screen_meeting_settings_form_header(Screen *base, HeaderInfo *info);
void screen_meeting_settings_form_place(Screen *base, const RECT *content, int scroll_y);
void screen_meeting_settings_form_ensure_controls(MeetingForm *s);
void screen_meeting_settings_form_save(MeetingForm *s);
Screen *meeting_settings_screen_new(void);

#endif
