#include "screen_new_session_internal.h"

static char *chip_label(NewSessionScreen *s, int chip);
static bool chip_shown(NewSessionScreen *s, int chip);
static bool chip_picker(int chip);
static int chip_width(Canvas *cv, const char *label, bool picker);
static int chips_layout(NewSessionScreen *s, Canvas *cv, int width, RECT *out);
static int composer_height(NewSessionScreen *s, Canvas *cv);
static RECT footer_column(const RECT *rc);

// MARK: - The composer's text

char *screen_new_session_composer_text(NewSessionScreen *s) {
    int n = GetWindowTextLengthW(s->composer);
    wchar_t *w = xmalloc(((size_t)n + 1) * sizeof *w); GetWindowTextW(s->composer, w, n + 1);
    char *text = wide_to_utf8(w); free(w);
    char *lf = str_replace(text, "\r\n", "\n"); free(text);
    return lf;
}

void screen_new_session_set_composer_text(NewSessionScreen *s, const char *text) {
    char *crlf = str_replace(text ? text : "", "\n", "\r\n");
    wchar_t *w = utf8_to_wide(crlf);
    SetWindowTextW(s->composer, w);
    SendMessageW(s->composer, EM_SETSEL, (WPARAM)wcslen(w), (LPARAM)wcslen(w));
    free(w); free(crlf);
    pane_footer_changed(s->base.pane);
}

bool screen_new_session_composer_empty(NewSessionScreen *s) { char *t = screen_new_session_composer_text(s); char *trimmed = str_trim(t); bool e = !*trimmed; free(t); free(trimmed); return e; }

/// A session starts on a prompt, on files, or on both; never while a file is still uploading.
bool screen_new_session_can_start(NewSessionScreen *s) {
    return !s->busy && !s->uncertain && screen_new_session_project(s) && (!screen_new_session_composer_empty(s) || attach_list_count(s->files)) && !attach_list_uploading(s->files);
}

LRESULT CALLBACK screen_new_session_composer_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    NewSessionScreen *s = (NewSessionScreen *)ref;
    if (msg == WM_KEYDOWN && wp == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_CONTROL) & 0x8000)) { screen_new_session_start(s); return 0; }
    if (msg == WM_CHAR && wp == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_CONTROL) & 0x8000)) return 0;
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); return 0; }
    // Ctrl+V and Shift+Insert both reach the control as WM_PASTE; a file or image becomes an attachment, text pastes as ever.
    if (msg == WM_PASTE && attach_list_paste(s->files)) return 0;
    if (msg == WM_DROPFILES) { attach_list_drop(s->files, (HDROP)wp); return 0; }
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) { s->focused = msg == WM_SETFOCUS; pane_footer_changed(s->base.pane); }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, screen_new_session_composer_proc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// MARK: - Voice

void screen_new_session_voice_changed(void *ctx) {
    NewSessionScreen *s = ctx;
    char *text = voice_take_text(s->voice);
    if (text) {
        if (*text) {
            char *current = screen_new_session_composer_text(s);
            size_t n = strlen(current);
            const char *gap = (!n || current[n - 1] == ' ' || current[n - 1] == '\n') ? "" : " ";
            char *joined = xstrfmt("%s%s%s", current, gap, text);
            screen_new_session_set_composer_text(s, joined);
            free(joined); free(current);
        }
        free(text);
    }
    char *error = voice_take_error(s->voice);
    if (error) { app_alert("Voice note", error); free(error); }
    if (voice_state(s->voice) == VOICE_RECORDING) SetTimer(pane_hwnd(s->base.pane), TIMER_VOICE, 1000, NULL); else KillTimer(pane_hwnd(s->base.pane), TIMER_VOICE);
    pane_footer_changed(s->base.pane);
}

// MARK: - Footer: the chips and the composer

static char *chip_label(NewSessionScreen *s, int chip) {
    const Project *p = screen_new_session_project(s);
    RuntimeChoice eff; bool has = screen_new_session_effective_choice(s, &eff);
    char *out = NULL;
    switch (chip) {
    case CHIP_WORKSPACE: out = xstrdup(s->workspace == WORKSPACE_ORCHESTRATOR ? "\xF0\x9F\xA7\xAD Orchestrator" : s->workspace == WORKSPACE_LOCAL ? "\xE2\x8C\x82 Local" : "\xE2\x8C\x97 Worktree"); break;
    case CHIP_PROJECT: out = xstrdup(p ? project_title(p) : "Project"); break;
    case CHIP_BRANCH: out = s->branch ? xstrdup(s->branch) : screen_new_session_no_branch_label(s); break;
    case CHIP_PROVIDER: {
        const RuntimeProvider *pr = has ? runtime_catalog_provider(&s->catalog, eff.provider_id) : NULL;
        out = xstrdup(pr ? pr->label : "Provider");
        break;
    }
    case CHIP_MODEL: {
        const RuntimeModel *m = has ? runtime_catalog_model(&s->catalog, &eff) : NULL;
        out = xstrdup(m ? runtime_model_title(m) : has && eff.model ? eff.model : "Model");
        break;
    }
    case CHIP_EFFORT: out = xstrdup(has && eff.effort ? eff.effort : "effort"); break;
    case CHIP_LOOP: out = xstrdup(s->review_loop ? "\xF0\x9F\x94\x81 Review loop: on" : "\xF0\x9F\x94\x81 Review loop"); break;
    }
    if (has) runtime_choice_free(&eff);
    return out;
}

static bool chip_shown(NewSessionScreen *s, int chip) {
    switch (chip) {
    case CHIP_PROVIDER: case CHIP_MODEL: return s->has_catalog && s->catalog.provider_count > 0;
    case CHIP_EFFORT: { RuntimeChoice eff; if (!screen_new_session_effective_choice(s, &eff)) return false; size_t n = 0; runtime_catalog_efforts(&s->catalog, &eff, &n); runtime_choice_free(&eff); return n > 0; }
    case CHIP_BRANCH: return s->workspace != WORKSPACE_ORCHESTRATOR && store_supports("branches");
    case CHIP_LOOP: return s->workspace == WORKSPACE_WORKTREE && store_supports("review_loop");
    default: return true;
    }
}

static bool chip_picker(int chip) { return chip != CHIP_LOOP; }

static int chip_width(Canvas *cv, const char *label, bool picker) {
    int w = px(6) * 2 + text_width(cv, label, FONT_CAPTION) + px(4);   // a glyph from a fallback font measures a touch narrow
    if (picker) w += px(4) + text_width(cv, "\xE2\x96\xBE", FONT_TINY_SEMIBOLD);
    if (w > px(150)) w = px(150);
    return w;
}

/// Places the chips in rows 4px apart, wrapping at `width`; returns the rows' height. NULL `out` measures only.
static int chips_layout(NewSessionScreen *s, Canvas *cv, int width, RECT *out) {
    int x = 0, y = 0, h = px(24), gap = px(4);
    for (int c = 0; c < CHIP_COUNT; c++) {
        if (out) SetRectEmpty(&out[c]);
        if (!chip_shown(s, c)) continue;
        char *label = chip_label(s, c);
        int w = chip_width(cv, label, chip_picker(c));
        free(label);
        if (x > 0 && x + w > width) { x = 0; y += h + gap; }
        if (out) { RECT r = { x, y, x + w, y + h }; out[c] = r; }
        x += w + gap;
    }
    return y + h;
}

static int composer_height(NewSessionScreen *s, Canvas *cv) {
    int line_h = px(24);
    int lines = s->composer_lines < 1 ? 1 : s->composer_lines > 8 ? 8 : s->composer_lines;
    (void)cv;
    return lines * line_h;
}

/// `#composer-wrap`: 8px above, the chips, 8px, the box (10px, the files, the text, 6px, the 30px buttons, 8px), 6px, a
/// 16px note, 14px below.
int screen_new_session_new_session_footer_height(Screen *base, int width) {
    NewSessionScreen *s = (NewSessionScreen *)base;
    Canvas *cv = NULL;   // measuring only
    int inner = width - px(24) * 2; if (inner > px(860)) inner = px(860);
    int chips = chips_layout(s, cv, inner, NULL);
    int files = attach_list_height(s->files, cv, inner - px(24));
    int h = px(8) + chips + px(8) + (px(10) + files + composer_height(s, cv) + px(6) + px(30) + px(8) + 2) + px(6) + px(16) + px(14);
    return h;
}

static RECT footer_column(const RECT *rc) {
    int w = rc->right - rc->left - px(24) * 2; if (w > px(860)) w = px(860);
    int x = rc->left + (rc->right - rc->left - w) / 2;
    RECT r = { x, rc->top, x + w, rc->bottom };
    return r;
}

void screen_new_session_new_session_footer_layout(Screen *base, const RECT *rc) {
    NewSessionScreen *s = (NewSessionScreen *)base;
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

void screen_new_session_new_session_footer_paint(Screen *base, Canvas *cv, const RECT *rc) {
    NewSessionScreen *s = (NewSessionScreen *)base;
    fill_rect(cv, rc, theme.canvas);
    RECT col = footer_column(rc);
    int width = col.right - col.left;
    // The chips.
    RECT rects[CHIP_COUNT];
    int chips = chips_layout(s, cv, width, rects);
    int y0 = col.top + px(8);
    for (int c = 0; c < CHIP_COUNT; c++) {
        SetRectEmpty(&s->chip_rc[c]);
        if (IsRectEmpty(&rects[c])) continue;
        RECT r = { col.left + rects[c].left, y0 + rects[c].top, col.left + rects[c].right, y0 + rects[c].bottom };
        s->chip_rc[c] = r;
        bool on = (c == CHIP_LOOP && s->review_loop);
        bool dim = s->busy;
        fill_round_rect(cv, &r, px(6), theme.raise, on ? theme.accent : theme.line);
        char *label = chip_label(s, c);
        bool picker = chip_picker(c);
        int cw = picker ? px(4) + text_width(cv, "\xE2\x96\xBE", FONT_TINY_SEMIBOLD) : 0;
        RECT t = { r.left + px(6), r.top, r.right - px(6) - cw, r.bottom };
        draw_text(cv, label, &t, FONT_CAPTION, on ? theme.accent : dim ? theme.muted : theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (picker) { RECT a = { r.right - px(6) - cw + px(4), r.top, r.right - px(6), r.bottom }; draw_text(cv, "\xE2\x96\xBE", &a, FONT_TINY_SEMIBOLD, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE); }
        free(label);
    }
    // The box: `rounded-2xl border border-line bg-raise px-3 pt-2.5 pb-2 focus-within:border-line-strong`.
    int top = y0 + chips + px(8);
    int ch = composer_height(s, cv);
    int files = attach_list_height(s->files, cv, width - px(24));
    RECT box = { col.left, top, col.right, top + px(10) + files + ch + px(6) + px(30) + px(8) + 2 };
    s->box_rc = box;
    fill_round_rect(cv, &box, px(16), theme.raise, s->focused ? theme.line_strong : theme.line);
    attach_list_paint(s->files, cv, box.left + px(12), box.top + px(10) + 1, width - px(24));
    int row_y = box.top + px(10) + 1 + files + ch + px(6), bh = px(30), bw = px(32);
    memset(&s->mic_rc, 0, sizeof s->mic_rc); memset(&s->attach_rc, 0, sizeof s->attach_rc);
    int x = box.left + px(12);
    // `#btn-attach`: the 📎 left of the microphone, for files the clipboard or a drop cannot bring.
    if (attach_list_supported(s->files)) {
        RECT attach = { x, row_y, x + bw, row_y + bh };
        s->attach_rc = attach;
        attach_button_paint(cv, &attach, !s->busy && attach_list_count(s->files) < ATTACHMENTS_MAX);
        x += bw + px(6);
    }
    VoiceState vs = s->voice ? voice_state(s->voice) : VOICE_IDLE;
    if (store_can_transcribe()) {
        RECT mic = { x, row_y, x + bw, row_y + bh };
        s->mic_rc = mic;
        fill_round_rect(cv, &mic, px(8), vs == VOICE_RECORDING ? theme.danger : theme.raise, vs == VOICE_RECORDING ? theme.danger : theme.line);
        draw_text(cv, vs == VOICE_RECORDING ? "\xE2\x96\xA0" : "\xF0\x9F\x8E\xA4", &mic, FONT_EMOJI_LARGE, vs == VOICE_RECORDING ? theme.white : theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (vs == VOICE_RECORDING) { char *clock = format_clock(voice_elapsed(s->voice)); RECT cr = { mic.right + px(8), row_y, mic.right + px(80), row_y + bh }; draw_text(cv, clock, &cr, FONT_CAPTION, theme.danger, DT_LEFT | DT_VCENTER | DT_SINGLELINE); free(clock); }
    }
    RECT send = { box.right - px(12) - bw, row_y, box.right - px(12), row_y + bh };
    s->send_rc = send;
    bool enabled = screen_new_session_can_start(s);
    fill_round_rect(cv, &send, px(8), enabled ? theme.accent : theme.field, enabled ? theme.accent : theme.field);
    draw_text(cv, "\xE2\x86\xB5", &send, FONT_BODY, enabled ? theme.on_accent : theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    // The note under the box.
    RECT note = { col.left, box.bottom + px(6), col.right, box.bottom + px(6) + px(16) };
    const char *text = s->busy ? "Starting the session\xE2\x80\xA6" : s->error ? s->error : attach_list_uploading(s->files) ? "Uploading\xE2\x80\xA6" : "";
    draw_text(cv, text, &note, FONT_FOOTNOTE, s->error && !s->busy ? theme.danger : theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void screen_new_session_new_session_footer_click(Screen *base, POINT pt) {
    NewSessionScreen *s = (NewSessionScreen *)base;
    for (int c = 0; c < CHIP_COUNT; c++) if (!IsRectEmpty(&s->chip_rc[c]) && PtInRect(&s->chip_rc[c], pt)) { screen_new_session_pick_chip(s, c); return; }
    if (PtInRect(&s->send_rc, pt)) { screen_new_session_start(s); return; }
    if (!s->busy && attach_list_click(s->files, pt)) return;
    if (PtInRect(&s->attach_rc, pt)) { if (!s->busy) attach_list_pick(s->files); return; }
    if (PtInRect(&s->mic_rc, pt) && s->voice) {
        VoiceState vs = voice_state(s->voice);
        if (vs == VOICE_RECORDING) voice_stop(s->voice); else if (vs == VOICE_IDLE) voice_record(s->voice);
        pane_footer_changed(base->pane);
        return;
    }
    if (PtInRect(&s->box_rc, pt)) SetFocus(s->composer);
}

void screen_new_session_new_session_command(Screen *base, int id, int code, HWND control) {
    NewSessionScreen *s = (NewSessionScreen *)base;
    if (id == ID_COMPOSER && code == EN_CHANGE) {
        int lines = (int)SendMessageW(control, EM_GETLINECOUNT, 0, 0);
        if (lines != s->composer_lines) { s->composer_lines = lines; pane_footer_changed(base->pane); }
        else { RECT rc; GetClientRect(pane_hwnd(base->pane), &rc); rc.top = rc.bottom - screen_new_session_new_session_footer_height(base, rc.right); InvalidateRect(pane_hwnd(base->pane), &rc, FALSE); }
    }
}
