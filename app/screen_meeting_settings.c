#include "screen_meeting_settings_internal.h"

static void reload(MeetingForm *s);
static void show_message(MeetingForm *s, const char *text, bool ok);
static void changed(MeetingForm *s);
static void form_destroy(Screen *base);
static void form_action(Screen *base, int action, intptr_t arg, POINT pt);
static void form_command(Screen *base, int id, int code, HWND control);
static bool form_key(Screen *base, WPARAM vk, bool ctrl, bool shift);
static void form_visible(Screen *base, bool shown);
static bool form_can_leave(Screen *base);

static void reload(MeetingForm *s) {
    meeting_settings_free(&s->saved);
    meeting_settings_load(&s->saved);
    s->toggles[T_INDEPENDENT] = s->saved.independent;
    s->toggles[T_INTRODUCE] = s->saved.introduce;
    s->has_key = meeting_has_key(MEETING_KEY_ELEVENLABS);
    s->has_openai_key = meeting_has_key(MEETING_KEY_OPENAI);
    Json *history = meeting_history();
    meet_totals(history, s->totals);
    json_free(history);
}

// MARK: - Saving

static void show_message(MeetingForm *s, const char *text, bool ok) {
    set_string(&s->message, text); s->message_ok = ok;
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}

static void changed(MeetingForm *s) { if (s->filling) return; if (!s->dirty) { s->dirty = true; pane_header_changed(s->base.pane); } }

void screen_meeting_settings_form_save(MeetingForm *s) {
    if (!s->edits[F_KEY]) return;
    char *key = screen_meeting_settings_edit_text(s->edits[F_KEY]), *trimmed_key = str_trim(key);
    SecureZeroMemory(key, strlen(key)); free(key);
    if (*trimmed_key) {
        bool saved = meeting_key_save(MEETING_KEY_ELEVENLABS, trimmed_key);
        SecureZeroMemory(trimmed_key, strlen(trimmed_key));
        if (!saved) { show_message(s, "The key could not be saved in Credential Manager.", false); free(trimmed_key); return; }
    }
    free(trimmed_key);
    MeetingSettings next = { 0 };
    char *name = screen_meeting_settings_edit_text(s->edits[F_NAME]), *wake = screen_meeting_settings_edit_text(s->edits[F_WAKE]), *voice = screen_meeting_settings_edit_text(s->edits[F_VOICE]);
    next.name = str_trim(name); next.wake_words = str_trim(wake); next.eleven_voice = str_trim(voice);
    free(name); free(wake); free(voice);
    next.independent = s->toggles[T_INDEPENDENT]; next.introduce = s->toggles[T_INTRODUCE];
    meeting_settings_save(&next);
    meeting_settings_free(&next);
    reload(s);
    screen_meeting_settings_form_fill(s);
    show_message(s, meeting_state() != MEETING_OFF ? "Saved. The meeting under way keeps its settings until you join again." : "Saved.", true);
}

// MARK: - The screen

static void form_destroy(Screen *base) {
    MeetingForm *s = (MeetingForm *)base;
    for (int f = 0; f < F_COUNT; f++) if (s->edits[f]) DestroyWindow(s->edits[f]);
    meeting_settings_free(&s->saved);
    free(s->cable); free(s->message);
    screen_release(base);
}

static void form_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    MeetingForm *s = (MeetingForm *)base;
    switch (action) {
    case ACT_SAVE: screen_meeting_settings_form_save(s); break;
    case ACT_REMOVE_KEY:
        if (!app_confirm("Remove the ElevenLabs API key?", "Meetings cannot be joined from this computer until a key is saved again. The key itself stays valid at ElevenLabs.", "Remove", true)) break;
        if (meeting_key_remove(MEETING_KEY_ELEVENLABS)) { reload(s); screen_meeting_settings_form_fill(s); show_message(s, "The key was removed from this computer.", true); }
        else show_message(s, "The key could not be removed from Credential Manager.", false);
        break;
    case ACT_REMOVE_OPENAI_KEY:
        if (meeting_key_remove(MEETING_KEY_OPENAI)) { reload(s); pane_relayout(base->pane); show_message(s, "The OpenAI key was removed from this computer.", true); }
        else show_message(s, "The OpenAI key could not be removed from Credential Manager.", false);
        break;
    case ACT_TOGGLE: if (arg >= 0 && arg < T_COUNT) { s->toggles[arg] = !s->toggles[arg]; changed(s); pane_relayout(base->pane); } break;
    case ACT_FOCUS: if (arg >= 0 && arg < F_COUNT && s->edits[arg]) SetFocus(s->edits[arg]); break;
    case ACT_CHECK_CABLE: free(s->cable); s->cable = meet_cable_name(); pane_relayout(base->pane); break;
    case ACT_CLEAR_HISTORY:
        if (!app_confirm("Clear the meeting history?", "The times and costs of every meeting recorded on this computer are deleted.", "Clear", true)) break;
        meeting_history_clear(); reload(s); pane_relayout(base->pane);
        break;
    }
}

static void form_command(Screen *base, int id, int code, HWND control) {
    (void)control;
    MeetingForm *s = (MeetingForm *)base;
    int f = id - ID_FIELD;
    if (f < 0 || f >= F_COUNT || !s->edits[f]) return;
    switch (code) {
    case EN_CHANGE: changed(s); break;
    case EN_SETFOCUS: s->focused = f; pane_repaint(base->pane); break;
    case EN_KILLFOCUS: if (s->focused == f) s->focused = -1; pane_repaint(base->pane); break;
    }
}

static bool form_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)shift;
    if (ctrl && vk == 'S') { screen_meeting_settings_form_save((MeetingForm *)base); return true; }
    return false;
}

static void form_visible(Screen *base, bool shown) {
    MeetingForm *s = (MeetingForm *)base;
    s->shown = shown;
    if (shown) {
        screen_meeting_settings_form_ensure_controls(s);
        // The history grows with each meeting left meanwhile.
        if (!s->dirty) { reload(s); pane_relayout(base->pane); }
    } else for (int f = 0; f < F_COUNT; f++) if (s->edits[f]) ShowWindow(s->edits[f], SW_HIDE);
}

static bool form_can_leave(Screen *base) {
    MeetingForm *s = (MeetingForm *)base;
    if (!s->dirty) return true;
    bool leave = app_confirm("Discard unsaved changes?", "The meeting assistant's settings have not been saved.", "Discard", true);
    if (leave) s->dirty = false;
    return leave;
}

static const ScreenVTable form_vt = {
    .destroy = form_destroy, .layout = screen_meeting_settings_form_layout, .header = screen_meeting_settings_form_header, .action = form_action, .place = screen_meeting_settings_form_place,
    .visible = form_visible, .command = form_command, .key = form_key, .can_leave = form_can_leave,
};

Screen *meeting_settings_screen_new(void) {
    MeetingForm *s = xcalloc(1, sizeof *s);
    s->base.vt = &form_vt;
    s->base.id = xstrdup("settings-meeting");
    s->focused = -1;
    reload(s);
    s->cable = meet_cable_name();
    return &s->base;
}
