#include "screen_meeting_settings_internal.h"

typedef struct { const char *label, *cue, *hint; bool secret, mono; } FieldDef;

typedef struct { MeetingForm *s; int t; } CheckData;

static void set_edit_text(HWND edit, const char *text);
static void paint_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void field(MeetingForm *s, Doc *doc, int x, int w, int f);
static void field_pair(MeetingForm *s, Doc *doc, int x, int w, int left, int right);
static void paint_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void check(MeetingForm *s, Doc *doc, int x, int w, int t);
static void note(Doc *doc, int x, int w, const char *text);
static void heading(Doc *doc, int x, int w, const char *text);
static char *money(double dollars);
static char *duration(double seconds);
static void layout_model(MeetingForm *s, Doc *doc, int x, int w, MeetModel m);
static void layout_history(MeetingForm *s, Doc *doc, int x, int w);
static int margin_of(Pane *pane);
static LRESULT CALLBACK field_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);

static const FieldDef FIELDS[F_COUNT] = {
    [F_KEY] = { "ElevenLabs API key", "sk_\xE2\x80\xA6", "Needs the ElevenLabs Agents and Text to Speech permissions. Kept in Windows Credential Manager on this computer only, and sent to api.elevenlabs.io alone.", true, true },
    [F_VOICE] = { "ElevenLabs voice ID", "", "The voice it speaks with, yours: ElevenLabs \xE2\x86\x92 Voices \xE2\x86\x92 your voice \xE2\x86\x92 ID.", false, true },
    [F_NAME] = { "Your name", "", "Who the assistant speaks for." },
    [F_WAKE] = { "Wake words", "Nadin, assistant, Briareus",
                 "Comma-separated. An assistant that does not act independently answers only when one of them is said, or on \xE2\x80\x9C" "Answer now\xE2\x80\x9D." },
};

static const char *const TOGGLES[T_COUNT] = {
    [T_INDEPENDENT] = "Act independently: take part on my behalf and answer what is asked of me",
    [T_INTRODUCE] = "Introduce itself as my AI assistant when it joins",
};

char *screen_meeting_settings_edit_text(HWND edit) {
    int n = GetWindowTextLengthW(edit);
    wchar_t *w = xmalloc(((size_t)n + 1) * sizeof *w);
    GetWindowTextW(edit, w, n + 1);
    char *text = wide_to_utf8(w); free(w);
    return text;
}

static void set_edit_text(HWND edit, const char *text) { wchar_t *w = utf8_to_wide(text ? text : ""); SetWindowTextW(edit, w); free(w); }

void screen_meeting_settings_form_fill(MeetingForm *s) {
    if (!s->edits[F_KEY]) return;
    s->filling = true;
    set_edit_text(s->edits[F_KEY], "");
    set_edit_text(s->edits[F_VOICE], s->saved.eleven_voice);
    set_edit_text(s->edits[F_NAME], s->saved.name);
    set_edit_text(s->edits[F_WAKE], s->saved.wake_words);
    // A saved key is never shown back; the box says it is there.
    wchar_t *cue = utf8_to_wide(s->has_key ? "Saved \xE2\x80\x94 type a new key to replace it" : FIELDS[F_KEY].cue);
    SendMessageW(s->edits[F_KEY], EM_SETCUEBANNER, TRUE, (LPARAM)cue);
    free(cue);
    s->filling = false;
    s->dirty = false;
}

// MARK: - Layout

static void paint_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    MeetingForm *s = it->data;
    (void)doc;
    fill_round_rect(cv, rc, px(6), theme.raise, s->focused == (int)it->arg ? theme.accent_dim : theme.line);
}

static void field(MeetingForm *s, Doc *doc, int x, int w, int f) {
    doc_field_label(doc, x, w, FIELDS[f].label, theme.ink, FIELDS[f].hint);
    doc_space(doc, px(6));
    int fh = edit_line_height(FIELDS[f].mono ? FONT_MONO : FONT_BODY), h = px(36);
    RECT box = { x, doc->y, x + w, doc->y + h };
    Item *it = doc_item(doc, doc_add(doc, &box, paint_box));
    it->data = s; it->arg = f; it->action = ACT_FOCUS;
    s->rects[f] = (RECT){ x + px(10), box.top + (h - fh) / 2, x + w - px(10), box.top + (h - fh) / 2 + fh };
    s->laid[f] = true;
    doc->y = box.bottom;
    doc_space(doc, px(14));
}

static void field_pair(MeetingForm *s, Doc *doc, int x, int w, int left, int right) {
    int gap = px(14), half = (w - gap) / 2, top = doc->y;
    field(s, doc, x, half, left);
    int bottom = doc->y;
    doc->y = top;
    field(s, doc, x + w - half, half, right);
    if (doc->y < bottom) doc->y = bottom;
}

static void paint_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    CheckData *d = it->data;
    bool on = d->s->toggles[d->t], hovered = doc_item_hovered(doc, it);
    int size = px(15), top = rc->top + (rc->bottom - rc->top - size) / 2;
    RECT b = { rc->left, top, rc->left + size, top + size };
    fill_round_rect(cv, &b, px(3), on ? theme.accent : theme.field, on ? theme.accent : hovered ? theme.accent_dim : theme.line_strong);
    if (on) draw_glyph(cv, 0xE73E, &b, FONT_ICON_SMALL, theme.on_accent);
    RECT t = { b.right + px(8), rc->top, rc->right, rc->bottom };
    draw_text(cv, TOGGLES[d->t], &t, FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void check(MeetingForm *s, Doc *doc, int x, int w, int t) {
    CheckData *d = xcalloc(1, sizeof *d);
    d->s = s; d->t = t;
    doc_item(doc, doc_custom(doc, x, w, px(26), paint_check, d, free, ACT_TOGGLE, t))->hand = true;
    doc_space(doc, px(6));
}

static void note(Doc *doc, int x, int w, const char *text) {
    doc_text(doc, x, w, text, FONT_CAPTION, theme.muted, DT_LEFT | DT_WORDBREAK);
    doc_space(doc, px(10));
}

static void heading(Doc *doc, int x, int w, const char *text) {
    doc_space(doc, px(12));
    doc_text(doc, x, w, text, FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_LEFT | DT_SINGLELINE);
    doc_space(doc, px(8));
}

static char *money(double dollars) { return xstrfmt(dollars < 10 ? "$%.3f" : "$%.2f", dollars); }

static char *duration(double seconds) {
    unsigned s = (unsigned)(seconds + 0.5);
    return s >= 3600 ? xstrfmt("%uh %02um", s / 3600, s / 60 % 60) : xstrfmt("%um %02us", s / 60, s % 60);
}

/// One model's meetings: what they took and cost. GPT-Live 1's are those recorded before it was dropped.
static void layout_model(MeetingForm *s, Doc *doc, int x, int w, MeetModel m) {
    const MeetTotals *t = &s->totals[m];
    int box = doc_box_begin(doc, x, w, px(12), theme.raise, theme.line, px(8));
    int iw = w - px(24), ix = x + px(12);
    doc_text(doc, ix, iw, meet_model_label(m), FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_LEFT | DT_SINGLELINE);
    doc_space(doc, px(6));
    char *count = xstrfmt("%d", t->meetings), *time_ = duration(t->seconds), *voice = money(t->voice_cost), *agent = money(t->agent_cost);
    char *total = money(t->voice_cost + t->agent_cost);
    double minutes = t->seconds / 60;
    char *rate = minutes > 0 ? money((t->voice_cost + t->agent_cost) / minutes) : xstrdup("\xE2\x80\x94");
    char *answers = xstrfmt("%d of %d", t->answers, t->requests);
    char *wait = t->answers ? xstrfmt("%.1f s", t->answer_seconds / t->answers) : xstrdup("\xE2\x80\x94");
    doc_labeled(doc, ix, iw, "Meetings", count, theme.ink);
    doc_labeled(doc, ix, iw, "Time in meetings", time_, theme.ink);
    doc_labeled(doc, ix, iw, "Voice", voice, theme.ink);
    // Meetings before the tools asked the conversation's agent, which cost apart.
    if (t->agent_cost > 0) doc_labeled(doc, ix, iw, "Conversation agent", agent, theme.ink);
    doc_labeled(doc, ix, iw, "Total", total, theme.ink);
    doc_labeled(doc, ix, iw, "Per minute", rate, theme.ink);
    doc_labeled(doc, ix, iw, m == MEET_LIVE ? "Agent questions answered" : "Project lookups answered", answers, theme.ink);
    doc_labeled(doc, ix, iw, m == MEET_LIVE ? "Average answer time" : "Average lookup time", wait, theme.ink);
    free(count); free(time_); free(voice); free(agent); free(total); free(rate); free(answers); free(wait);
    doc_box_end(doc, box, px(12));
}

static void layout_history(MeetingForm *s, Doc *doc, int x, int w) {
    heading(doc, x, w, "Meetings on this computer");
    note(doc, x, w, "From joining to leaving. Voice is the ElevenLabs agent at $0.08 a minute, its LLM billed apart; for older meetings, what OpenAI reported using at its listed rates (GPT-Realtime 2.1 mini's text and "
                    "audio tokens plus $0.0045 a minute of transcription), plus ElevenLabs' characters at $0.04 per 1,000.");
    int gap = px(14);
    // GPT-Live 1's box only while meetings recorded with it remain.
    bool live = s->totals[MEET_LIVE].meetings > 0;
    if (live && w >= px(560)) {
        int half = (w - gap) / 2, top = doc->y;
        layout_model(s, doc, x, half, MEET_REALTIME);
        int bottom = doc->y;
        doc->y = top;
        layout_model(s, doc, x + half + gap, half, MEET_LIVE);
        if (doc->y < bottom) doc->y = bottom;
    } else {
        layout_model(s, doc, x, w, MEET_REALTIME);
        if (live) { doc_space(doc, gap); layout_model(s, doc, x, w, MEET_LIVE); }
    }
    doc_space(doc, px(12));
    bool any = live || s->totals[MEET_REALTIME].meetings;
    doc_button(doc, x, 0, "Clear the history", BUTTON_BORDERED, ACT_CLEAR_HISTORY, 0, any);
}

void screen_meeting_settings_form_layout(Screen *base, Doc *doc) {
    MeetingForm *s = (MeetingForm *)base;
    memset(s->laid, 0, sizeof s->laid);
    int w = doc->width, x = 0;
    doc_space(doc, px(16));
    if (s->message) {
        if (s->message_ok) doc_label(doc, x, w, 0xE73E, s->message, FONT_FOOTNOTE, theme.ok);
        else doc_notice_box(doc, x, w, s->message);
        doc_space(doc, px(14));
    }
    note(doc, x, w, "On a project, \xF0\x9F\x8E\x99 Meet joins your meeting: the assistant hears the meeting app, "
                    "speaks through a virtual microphone, and looks up the project's conversations, pull requests, findings and issues. It only reads: it never starts, messages, merges or closes anything.");
    field_pair(s, doc, x, w, F_KEY, F_VOICE);
    field_pair(s, doc, x, w, F_NAME, F_WAKE);
    check(s, doc, x, w, T_INDEPENDENT);
    check(s, doc, x, w, T_INTRODUCE);
    note(doc, x, w, s->toggles[T_INDEPENDENT]
        ? "Acting independently, it decides for itself when to speak. It still says it is an AI if sincerely asked."
        : "Otherwise it speaks only when you or it are addressed by a wake word, or when you choose \xE2\x80\x9C" "Answer now\xE2\x80\x9D.");
    if (s->has_openai_key) {
        note(doc, x, w, "The OpenAI API key saved for the earlier assistant is no longer used.");
        doc_button(doc, x, 0, "Remove the OpenAI key", BUTTON_BORDERED, ACT_REMOVE_OPENAI_KEY, 0, true);
        doc_space(doc, px(10));
    }
    heading(doc, x, w, "Virtual microphone");
    if (s->cable) {
        char *line = xstrfmt("%s is installed. In your meeting app, pick \xE2\x80\x9C" "CABLE Output\xE2\x80\x9D as the microphone and keep your usual speakers. The meeting then hears only the assistant, never your microphone: to speak yourself, switch the meeting app back to your microphone. The assistant speaks only into the meeting, so you will not hear it yourself: its words are in the meeting transcript (\xF0\x9F\x8E\x99 menu).", s->cable);
        doc_label(doc, x, w, 0xE73E, line, FONT_FOOTNOTE, theme.ok);
        free(line);
    } else {
        doc_notice(doc, x, w, "VB-Cable is not installed. Install it (free, from vb-audio.com) and restart Windows: it is the microphone the assistant speaks into.");
        doc_space(doc, px(8));
        doc_button(doc, x, 0, "Check again", BUTTON_BORDERED, ACT_CHECK_CABLE, 0, true);
    }
    layout_history(s, doc, x, w);
    doc_space(doc, px(40));
}

void screen_meeting_settings_form_header(Screen *base, HeaderInfo *info) {
    MeetingForm *s = (MeetingForm *)base;
    snprintf(info->title, sizeof info->title, "Meeting assistant");
    snprintf(info->subtitle, sizeof info->subtitle, "%s \xC2\xB7 settings on this computer", s->has_key ? "ElevenLabs API key saved" : "no ElevenLabs API key yet");
    HeaderButton *b = &info->buttons[info->button_count++];
    snprintf(b->label, sizeof b->label, "Save");
    b->glyph = 0xE74E; b->action = ACT_SAVE; b->prominent = true; b->tip = "Save (Ctrl+S)"; b->enabled = s->dirty;
    if (!s->has_key) return;
    HeaderButton *d = &info->buttons[info->button_count++];
    d->glyph = 0xE74D; d->action = ACT_REMOVE_KEY; d->destructive = true; d->enabled = true; d->tip = "Remove the ElevenLabs API key from this computer";
}

// MARK: - The edits

static int margin_of(Pane *pane) { RECT rc; GetClientRect(pane_hwnd(pane), &rc); return (rc.right - rc.left - pane_content_width(pane)) / 2; }

void screen_meeting_settings_form_place(Screen *base, const RECT *content, int scroll_y) {
    MeetingForm *s = (MeetingForm *)base;
    int m = margin_of(base->pane);
    for (int f = 0; f < F_COUNT; f++) {
        HWND e = s->edits[f];
        if (!e) continue;
        if (!s->shown || !s->laid[f]) { ShowWindow(e, SW_HIDE); continue; }
        RECT r = { content->left + m + s->rects[f].left, content->top + s->rects[f].top - scroll_y, content->left + m + s->rects[f].right, content->top + s->rects[f].bottom - scroll_y };
        RECT visible;
        if (!IntersectRect(&visible, &r, content)) { ShowWindow(e, SW_HIDE); continue; }
        MoveWindow(e, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
        bool clipped = !EqualRect(&visible, &r);
        if (clipped) SetWindowRgn(e, CreateRectRgn(visible.left - r.left, visible.top - r.top, visible.right - r.left, visible.bottom - r.top), TRUE);
        else if (s->clipped[f]) SetWindowRgn(e, NULL, TRUE);
        s->clipped[f] = clipped;
        ShowWindow(e, SW_SHOWNA);
    }
}

static LRESULT CALLBACK field_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    MeetingForm *s = (MeetingForm *)ref;
    int f = (int)id;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_TAB) { int step = (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1; SetFocus(s->edits[((f + step) % F_COUNT + F_COUNT) % F_COUNT]); return 0; }
        if (ctrl && wp == 'S') { screen_meeting_settings_form_save(s); return 0; }
        if (ctrl && wp == 'A') { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        if (wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); return 0; }
        if (wp == VK_RETURN) { screen_meeting_settings_form_save(s); return 0; }
        break;
    case WM_CHAR:
        if (wp == '\t' || wp == 0x13 || wp == 0x01 || wp == 0x1B || wp == '\r') return 0;
        break;
    case WM_MOUSEWHEEL: SendMessageW(GetParent(hwnd), msg, wp, lp); return 0;
    case WM_NCDESTROY: RemoveWindowSubclass(hwnd, field_proc, id); break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void screen_meeting_settings_form_ensure_controls(MeetingForm *s) {
    if (s->edits[F_KEY]) return;
    HWND owner = pane_hwnd(s->base.pane);
    for (int f = 0; f < F_COUNT; f++) {
        DWORD style = WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | (FIELDS[f].secret ? ES_PASSWORD : 0);
        HWND e = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, owner, (HMENU)(INT_PTR)(ID_FIELD + f), GetModuleHandleW(NULL), NULL);
        SendMessageW(e, WM_SETFONT, (WPARAM)font(FIELDS[f].mono ? FONT_MONO : FONT_BODY), TRUE);
        { wchar_t *w = utf8_to_wide(FIELDS[f].cue); SendMessageW(e, EM_SETCUEBANNER, TRUE, (LPARAM)w); free(w); }
        SetWindowSubclass(e, field_proc, (UINT_PTR)f, (DWORD_PTR)s);
        theme_apply_control(e);
        s->edits[f] = e;
    }
    screen_meeting_settings_form_fill(s);
}
