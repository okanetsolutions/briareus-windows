#include "screen_conversation_internal.h"

static int composer_height(ConversationScreen *s, Canvas *cv);
static char *chip_text(ConversationScreen *s, int chip);
static bool chip_shown(ConversationScreen *s, int chip);
static bool chip_live(int chip);
static const char *loop_cost(ConversationScreen *s);
static int loop_cost_height(ConversationScreen *s, Canvas *cv, int width);
static int chips_layout(ConversationScreen *s, Canvas *cv, int width, RECT *out);
static RECT footer_column(const RECT *rc);

static RECT g_chip_rc[CHIP_COUNT];

static int composer_height(ConversationScreen *s, Canvas *cv) {
    int line_h = px(24);
    int lines = s->composer_lines < 1 ? 1 : s->composer_lines > 8 ? 8 : s->composer_lines;
    (void)cv;
    return lines * line_h;
}

static char *chip_text(ConversationScreen *s, int chip) {
    const Session *ss = screen_conversation_session(s);
    switch (chip) {
    case CHIP_WORKSPACE: return xstrdup(json_bool_is(json_get(ss->raw, "local"), true) ? "\xE2\x8C\x82 Local" : json_bool_is(json_get(ss->raw, "orchestrator"), true) ? "\xF0\x9F\xA7\xAD Orchestrator" : "\xE2\x8C\x97 Worktree");
    case CHIP_PROJECT: { const char *repo = session_repo(ss); const char *slash = repo ? strrchr(repo, '/') : NULL; return xstrdup(slash ? slash + 1 : repo ? repo : ""); }
    case CHIP_BRANCH: { const char *b = json_str_nonempty(json_get(ss->raw, "branch")); return xstrdup(b ? b : ""); }
    case CHIP_PROVIDER: return xstrdup(session_provider(ss) ? session_provider(ss) : "");
    case CHIP_MODEL: return xstrdup(session_model(ss) ? session_model(ss) : "");
    case CHIP_EFFORT: { const char *e = json_str_nonempty(json_get(ss->raw, "effort")); return xstrdup(e ? e : ""); }
    case CHIP_LOOP: return xstrdup(session_review_loop_on(ss) ? "\xF0\x9F\x94\x81 Review loop: on" : "\xF0\x9F\x94\x81 Review loop");
    }
    return xstrdup("");
}

static bool chip_shown(ConversationScreen *s, int chip) {
    const Session *ss = screen_conversation_session(s);
    if (chip == CHIP_LOOP) return store_supports("review_loop") && session_can_review_loop(ss);
    char *text = chip_text(s, chip); bool shown = *text != 0; free(text);
    return shown;
}

static bool chip_live(int chip) { return chip == CHIP_LOOP; }

/// What the loop chip costs, since it asks nothing first: under the composer whatever else its note says.
static const char *loop_cost(ConversationScreen *s) {
    if (!chip_shown(s, CHIP_LOOP)) return NULL;
    return session_review_loop_on(screen_conversation_session(s)) ? "Review loop: a paid review round per push; turning it off won't stop a running round"
                                              : "Turning on the review loop may start a paid review round now, then one per push";
}

/// The cost wraps rather than ellipsizes, so a narrow pane still shows all of it.
/// Measured once per text, width and scale: the footer's height is asked for on every keystroke.
static int loop_cost_height(ConversationScreen *s, Canvas *cv, int width) {
    const char *cost = loop_cost(s);
    if (!cost) return 0;
    if (cost != s->cost_text || width != s->cost_width || px(16) != s->cost_unit) {
        s->cost_text = cost; s->cost_width = width; s->cost_unit = px(16);
        s->cost_h = measure_text(cv, cost, width, FONT_FOOTNOTE, DT_CENTER | DT_WORDBREAK);
    }
    return s->cost_h;
}

static int chips_layout(ConversationScreen *s, Canvas *cv, int width, RECT *out) {
    int x = 0, y = 0, h = px(24), gap = px(4);
    for (int c = 0; c < CHIP_COUNT; c++) {
        if (out) SetRectEmpty(&out[c]);
        if (!chip_shown(s, c)) continue;
        char *label = chip_text(s, c);
        int w = px(6) * 2 + text_width(cv, label, FONT_CAPTION) + px(4);
        if (w > px(150)) w = px(150);
        free(label);
        if (x > 0 && x + w > width) { x = 0; y += h + gap; }
        if (out) { RECT r = { x, y, x + w, y + h }; out[c] = r; }
        x += w + gap;
    }
    return y + h;
}

static RECT footer_column(const RECT *rc) {
    int w = rc->right - rc->left - px(24) * 2; if (w > px(860)) w = px(860);
    int x = rc->left + (rc->right - rc->left - w) / 2;
    RECT r = { x, rc->top, x + w, rc->bottom };
    return r;
}

int screen_conversation_conversation_footer_height(Screen *base, int width) {
    ConversationScreen *s = (ConversationScreen *)base;
    Canvas *cv = NULL;   // measuring only
    int inner = width - px(24) * 2; if (inner > px(860)) inner = px(860);
    int chips = chips_layout(s, cv, inner, NULL);
    int files = screen_conversation_can_message(s) ? attach_list_height(s->files, cv, inner - px(24)) : 0;
    int box = screen_conversation_can_message(s) ? px(10) + files + composer_height(s, cv) + px(6) + px(30) + px(8) + 2 : px(30) + px(20) + 2;
    int cost = screen_conversation_can_message(s) ? loop_cost_height(s, cv, inner) : 0;
    int h = px(8) + chips + px(8) + box + px(6) + px(16) + cost + px(14);
    return h;
}

void screen_conversation_conversation_footer_layout(Screen *base, const RECT *rc) {
    ConversationScreen *s = (ConversationScreen *)base;
    if (!screen_conversation_can_message(s)) { ShowWindow(s->composer, SW_HIDE); return; }
    // Made in the detail pane, the composer follows a conversation pushed onto the side panel instead.
    HWND parent = pane_hwnd(base->pane);
    if (GetParent(s->composer) != parent) SetParent(s->composer, parent);
    Canvas *cv = NULL;   // measuring only
    RECT col = footer_column(rc);
    int chips = chips_layout(s, cv, col.right - col.left, NULL);
    int files = attach_list_height(s->files, cv, col.right - col.left - px(24));
    int ch = composer_height(s, cv);
    int top = col.top + px(8) + chips + px(8);
    RECT er = { col.left + px(12) + 1, top + px(10) + 1 + files, col.right - px(12) - 1, top + px(10) + 1 + files + ch };
    s->composer_rc = er;
    pane_place_control(s->composer, &er);
}

void screen_conversation_conversation_footer_paint(Screen *base, Canvas *cv, const RECT *rc) {
    ConversationScreen *s = (ConversationScreen *)base;
    const Session *ss = screen_conversation_session(s);
    fill_rect(cv, rc, theme.canvas);
    RECT col = footer_column(rc);
    int width = col.right - col.left;
    RECT rects[CHIP_COUNT];
    int chips = chips_layout(s, cv, width, rects);
    int y0 = col.top + px(8);
    bool can = !s->busy && !s->uncertain;
    for (int c = 0; c < CHIP_COUNT; c++) {
        SetRectEmpty(&g_chip_rc[c]);
        if (IsRectEmpty(&rects[c])) continue;
        RECT r = { col.left + rects[c].left, y0 + rects[c].top, col.left + rects[c].right, y0 + rects[c].bottom };
        bool live = chip_live(c) && can && !(c == CHIP_LOOP && s->reloading);
        if (live) g_chip_rc[c] = r;
        bool on = (c == CHIP_LOOP && session_review_loop_on(ss));
        // A held chip looks held whatever the loop's state: an accent one would seem to take the click it ignores.
        fill_round_rect(cv, &r, px(6), theme.raise, on && live ? theme.accent : theme.line);
        char *label = chip_text(s, c);
        RECT t = { r.left + px(6), r.top, r.right - px(6) + 2, r.bottom };
        draw_text(cv, label, &t, FONT_CAPTION, live ? (on ? theme.accent : theme.ink) : theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        free(label);
    }
    int top = y0 + chips + px(8);
    memset(&s->mic_rc, 0, sizeof s->mic_rc); memset(&s->send_rc, 0, sizeof s->send_rc); memset(&s->discard_rc, 0, sizeof s->discard_rc); memset(&s->attach_rc, 0, sizeof s->attach_rc);
    if (!screen_conversation_can_message(s)) {
        RECT box = { col.left, top, col.right, top + px(30) + px(20) + 2 };
        fill_round_rect(cv, &box, px(16), theme.raise, theme.line);
        bool reopen = store_supports("reopen") && str_eq(session_status(ss), "closed");
        int bw = reopen ? px(10) * 2 + text_width(cv, "\xE2\x9F\xB3 Reopen", FONT_FOOTNOTE) + 2 : 0;
        RECT t = { box.left + px(12), box.top, box.right - bw - px(24), box.bottom };
        draw_text(cv, store_can_manage() ? "This session is closed." : "Read-only access", &t, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (reopen) {
            RECT b = { box.right - px(12) - bw, (box.top + box.bottom) / 2 - px(15), box.right - px(12), (box.top + box.bottom) / 2 + px(15) };
            s->send_rc = b;
            fill_round_rect(cv, &b, px(7), theme.raise, can ? theme.line : theme.line);
            draw_text(cv, "\xE2\x9F\xB3 Reopen", &b, FONT_FOOTNOTE, can ? theme.ink : theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        return;
    }
    int ch = composer_height(s, cv);
    int files = attach_list_height(s->files, cv, width - px(24));
    RECT box = { col.left, top, col.right, top + px(10) + files + ch + px(6) + px(30) + px(8) + 2 };
    fill_round_rect(cv, &box, px(16), theme.raise, GetFocus() == s->composer ? theme.line_strong : theme.line);
    attach_list_paint(s->files, cv, box.left + px(12), box.top + px(10) + 1, width - px(24));
    int row_y = box.top + px(10) + 1 + files + ch + px(6), bh = px(30), bw = px(32);
    bool trimmed_empty = screen_conversation_composer_empty(s);
    bool uploading = attach_list_uploading(s->files);
    bool active = session_is_active(ss);
    VoiceState vs = s->voice ? voice_state(s->voice) : VOICE_IDLE;
    // `#btn-send`: 32×30, the accent when there is something to send; a stop square while the agent works.
    RECT send = { box.right - px(12) - bw, row_y, box.right - px(12), row_y + bh };
    s->send_rc = send;
    if (active && store_supports("cancel") && trimmed_empty) {
        fill_round_rect(cv, &send, px(8), theme.raise, theme.line);
        draw_text(cv, "\xE2\x96\xA0", &send, FONT_CAPTION, theme.ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        bool enabled = can && !trimmed_empty && !uploading;
        fill_round_rect(cv, &send, px(8), enabled || s->busy ? theme.accent : theme.field, enabled || s->busy ? theme.accent : theme.field);
        draw_text(cv, "\xE2\x86\xB5", &send, FONT_BODY, enabled || s->busy ? theme.on_accent : theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    int x = box.left + px(12);
    // `#btn-attach`: the 📎 left of the microphone, for files the clipboard or a drop cannot bring.
    if (attach_list_supported(s->files)) {
        RECT attach = { x, row_y, x + bw, row_y + bh };
        s->attach_rc = attach;
        attach_button_paint(cv, &attach, attach_list_count(s->files) < ATTACHMENTS_MAX);
        x += bw + px(6);
    }
    if (store_can_transcribe()) {
        RECT mic = { x, row_y, x + bw, row_y + bh };
        s->mic_rc = mic;
        bool rec = vs == VOICE_RECORDING;
        fill_round_rect(cv, &mic, px(8), rec ? theme.danger : theme.raise, rec ? theme.danger : theme.line);
        draw_text(cv, rec ? "\xE2\x96\xA0" : (vs == VOICE_TRANSCRIBING || vs == VOICE_STARTING) ? "\xE2\x80\xA6" : "\xF0\x9F\x8E\xA4", &mic, FONT_EMOJI_LARGE, rec ? theme.white : theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        x += bw + px(6);
        if (rec || vs == VOICE_TRANSCRIBING) {
            RECT discard = { x, row_y, x + bw, row_y + bh };
            s->discard_rc = discard;
            fill_round_rect(cv, &discard, px(8), theme.raise, theme.line);
            draw_text(cv, "\xE2\x9C\x95", &discard, FONT_CAPTION, theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            x += bw + px(6);
            if (rec) { char *clock = format_clock(voice_elapsed(s->voice)); RECT cr = { x, row_y, x + px(60), row_y + bh }; draw_text(cv, clock, &cr, FONT_CAPTION, theme.danger, DT_LEFT | DT_VCENTER | DT_SINGLELINE); free(clock); }
        }
    }
    // `#composer-note`: what a message sent now does, then on its own row what the loop chip costs, so it stays put.
    RECT note = { col.left, box.bottom + px(6), col.right, box.bottom + px(6) + px(16) };
    const char *text = active ? (session_live_input(ss) ? "Sent into the running turn" : "Queued for the next turn")
                      : uploading ? "Uploading\xE2\x80\xA6" : attach_list_count(s->files) && trimmed_empty ? "Add a few words to send the files"
                      : "";
    if (*text) draw_text(cv, text, &note, FONT_FOOTNOTE, theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    // The footer's height already holds the cost, so it is drawn to the bottom margin without measuring it again.
    const char *cost = loop_cost(s);
    if (cost) { RECT cr = { note.left, note.bottom, note.right, rc->bottom - px(14) }; draw_text(cv, cost, &cr, FONT_FOOTNOTE, theme.muted, DT_CENTER | DT_WORDBREAK); }
}

void screen_conversation_conversation_footer_click(Screen *base, POINT pt) {
    ConversationScreen *s = (ConversationScreen *)base;
    if (!IsRectEmpty(&g_chip_rc[CHIP_LOOP]) && PtInRect(&g_chip_rc[CHIP_LOOP], pt)) { screen_conversation_menu_choice(s, session_review_loop_on(screen_conversation_session(s)) ? MENU_LOOP_OFF : MENU_LOOP_ON); return; }
    if (!screen_conversation_can_message(s)) { if (PtInRect(&s->send_rc, pt) && !s->busy && !s->uncertain) screen_conversation_confirm_and_mutate(s, "reopen"); return; }
    if (attach_list_click(s->files, pt)) return;
    if (PtInRect(&s->attach_rc, pt)) { attach_list_pick(s->files); return; }
    if (PtInRect(&s->send_rc, pt)) {
        bool active = session_is_active(screen_conversation_session(s));
        if (active && store_supports("cancel") && screen_conversation_composer_empty(s)) { if (!s->busy && !s->uncertain) screen_conversation_confirm_and_mutate(s, "cancel"); }
        else screen_conversation_send_message(s);
        return;
    }
    if (PtInRect(&s->mic_rc, pt) && s->voice) {
        VoiceState vs = voice_state(s->voice);
        if (vs == VOICE_RECORDING) voice_stop(s->voice); else if (vs == VOICE_IDLE) voice_record(s->voice);
        pane_footer_changed(base->pane);
        return;
    }
    if (PtInRect(&s->discard_rc, pt) && s->voice) { voice_drop(s->voice); pane_footer_changed(base->pane); return; }
    SetFocus(s->composer);
}
