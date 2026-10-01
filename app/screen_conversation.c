// One conversation: its transcript, the composer, and the actions on the session.
#include "dialogs.h"
#include "screens.h"
#include "str.h"
#include "voice.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ACT_ANSWER = 1000, ACT_TOOL_TOGGLE, ACT_DROP_QUEUED, ACT_TRIAGE_DECISION, ACT_TRIAGE_COMPLETE, ACT_TRIAGE_NOTE,
    ACT_REFRESH_OUTCOME, ACT_REOPEN, ACT_FINDING_LINK,
    ACT_MENU_ITEM = 1100,   // plus a MENU_ id: the header's buttons
};
enum { TIMER_POLL = 1, TIMER_WORKING = 2, TIMER_VOICE = 3 };
enum { ID_COMPOSER = 301 };
enum { TAG_REFRESH = 1, TAG_MUTATE = 2 };
enum { MENU_CHANGES = 1, MENU_PULL, MENU_LOOP_ON, MENU_LOOP_OFF, MENU_RENAME, MENU_STOP, MENU_CLOSE, MENU_REOPEN, MENU_DELETE, MENU_COPY };

typedef struct {
    Screen base;
    Session initial, snapshot; bool has_snapshot;
    Transcript transcript;
    bool loaded, restored, unsaved, retimed;
    bool busy, loading, uncertain, pending_full, renaming, dialog_open;
    char *error, *write_error;
    char *pending_mutation;   // the operation a confirmation is up for
    Request *req_refresh, *req_mutate;
    Poller poller;
    HWND composer; int composer_lines; RECT composer_rc;
    bool composer_focused_once;
    int *expanded; size_t expanded_count;   // tool rows opened
    char **decisions; size_t decision_count;  // triage picks, "key=decision"
    char *triage_note;
    int tick;
    VoiceNote *voice;
    char header_title[512], header_subtitle[256];
    // footer hit rects
    RECT mic_rc, send_rc, discard_rc;
    bool at_bottom;
} ConversationScreen;

static const Session *session(ConversationScreen *s) { return s->has_snapshot ? &s->snapshot : &s->initial; }
static char *cache_key(ConversationScreen *s) { return xstrfmt("transcript:%s", session_id(&s->initial)); }
static bool can_message(ConversationScreen *s) { return store_supports("message") && !str_eq(session_status(session(s)), "closed"); }

static void refresh(ConversationScreen *s, bool full);
static void mutate(ConversationScreen *s, const char *name, Json *extra);
static void show_panel(ConversationScreen *s);

static char *composer_text(ConversationScreen *s) {
    int n = GetWindowTextLengthW(s->composer);
    wchar_t *w = xmalloc(((size_t)n + 1) * sizeof *w); GetWindowTextW(s->composer, w, n + 1);
    char *text = wide_to_utf8(w); free(w);
    // Edit controls carry CRLF; the server gets LF.
    char *lf = str_replace(text, "\r\n", "\n"); free(text);
    return lf;
}
static void set_composer_text(ConversationScreen *s, const char *text) {
    char *crlf = str_replace(text ? text : "", "\n", "\r\n");
    wchar_t *w = utf8_to_wide(crlf);
    SetWindowTextW(s->composer, w);
    SendMessageW(s->composer, EM_SETSEL, (WPARAM)wcslen(w), (LPARAM)wcslen(w));
    free(w); free(crlf);
    pane_footer_changed(s->base.pane);
}
static bool composer_empty(ConversationScreen *s) { char *t = composer_text(s); char *trimmed = str_trim(t); bool e = !*trimmed; free(t); free(trimmed); return e; }

static void send_message(ConversationScreen *s) {
    if (s->busy || s->uncertain || composer_empty(s) || !can_message(s)) return;
    char *text = composer_text(s);
    Json *extra = json_object(); json_set_str(extra, "text", text);
    free(text);
    mutate(s, "message", extra);
}

static LRESULT CALLBACK composer_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    ConversationScreen *s = (ConversationScreen *)ref;
    if (msg == WM_KEYDOWN && wp == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_CONTROL) & 0x8000)) { send_message(s); return 0; }
    if (msg == WM_CHAR && wp == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_CONTROL) & 0x8000)) return 0;
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); return 0; }
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) pane_footer_changed(s->base.pane);
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, composer_proc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// MARK: - Reading

static void refresh_done(void *owner, Request *req) {
    ConversationScreen *s = owner;
    s->loading = false;
    bool full = req->arg != 0;
    if (!req->ok) {
        char *text = request_error_text(req); set_string(&s->error, text); free(text);
        s->loaded = true;
        poller_finished(&s->poller, true, req->error.retry_after);
    } else {
        const Json *events = json_get(req->result, "events");
        Session next;
        if (session_parse(json_get(req->result, "session"), &next)) {
            if (s->has_snapshot) session_free(&s->snapshot);
            s->snapshot = next; s->has_snapshot = true;
        }
        if (full) { transcript_free(&s->transcript); transcript_init(&s->transcript); }
        transcript_append(&s->transcript, events);
        set_string(&s->error, NULL); s->loaded = true;
        char *key = cache_key(s);
        if (full || s->unsaved) { Json *all = transcript_json(&s->transcript); s->unsaved = !cache_replace(g_store.cache, all, key); json_free(all); }
        else if (json_count(events)) s->unsaved = !cache_append(g_store.cache, events, key);
        free(key);
        poller_set_base(&s->poller, session_is_active(session(s)) ? 2000 : 7000);
        poller_finished(&s->poller, false, -1);
        if (session_pull_number(session(s)) && pane_top(s->base.pane) == &s->base && !pane_root(app_panel_pane())) show_panel(s);
        if (session_is_active(session(s))) SetTimer(pane_hwnd(s->base.pane), TIMER_WORKING, 120, NULL); else KillTimer(pane_hwnd(s->base.pane), TIMER_WORKING);
    }
    pane_relayout(s->base.pane);
    pane_header_changed(s->base.pane);
    if (s->pending_full) { s->pending_full = false; refresh(s, true); }
}
static void refresh(ConversationScreen *s, bool full) {
    // A poll already reading must not swallow a refresh, which waits its turn.
    if (s->loading) { if (full) s->pending_full = true; return; }
    s->loading = true;
    if (!s->restored) {
        // Saved events show at once and move the cursor, so only what happened since is downloaded.
        s->restored = true;
        char *key = cache_key(s);
        Json *saved = cache_lines(g_store.cache, key);
        transcript_append(&s->transcript, saved);
        json_free(saved); free(key);
        if (s->transcript.count) pane_relayout(s->base.pane);
    }
    // A transcript saved before messages showed their time has none; it is read again once to get them.
    if (!s->retimed && s->transcript.count) {
        bool all_untimed = true;
        for (size_t i = 0; i < s->transcript.count && all_untimed; i++) all_untimed = s->transcript.events[i].t == NULL;
        if (all_untimed) full = true;
    }
    s->retimed = true;
    Json *args = json_object();
    json_set_str(args, "sessionId", session_id(&s->initial));
    json_set_num(args, "since", full ? 0 : s->transcript.cursor);
    Request *r = store_call("session", args, 0, s, refresh_done, TAG_REFRESH, &s->req_refresh);
    r->arg = full;
}

// MARK: - Writing

static void mutate_done(void *owner, Request *req) {
    ConversationScreen *s = owner;
    s->busy = false;
    const char *name = req->operation;
    if (!req->ok) {
        char *text = request_error_text(req); set_string(&s->write_error, text); free(text);
        s->uncertain = true;
        pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
        return;
    }
    if (str_eq(name, "message")) {
        char *sent = composer_text(s);
        if (str_eq(sent, json_str(json_get(req->args, "text")))) { set_composer_text(s, ""); pane_stick_to_bottom(s->base.pane, true); pane_scroll_to_bottom(s->base.pane); }
        free(sent);
    }
    if (str_eq(name, "delete")) {
        // The sidebar drops the row now instead of at its next poll; the transcript goes with it.
        sessions_forget(session_repo(&s->initial), session_id(&s->initial));
        // Beside the list there is nothing to go back to: the right-hand side empties instead.
        Pane *pane = s->base.pane;
        if (pane_depth(pane) > 1) pane_pop(pane); else app_clear_detail();
        return;   // this screen is gone
    }
    if (str_eq(name, "complete_findings")) { str_array_free(s->decisions, s->decision_count); s->decisions = NULL; s->decision_count = 0; set_string(&s->triage_note, NULL); }
    pane_header_changed(s->base.pane);
    refresh(s, false);
}
static void mutate(ConversationScreen *s, const char *name, Json *extra) {
    if (s->busy || s->uncertain) { json_free(extra); return; }
    s->busy = true; set_string(&s->write_error, NULL);
    Json *args = json_object();
    json_set_str(args, "sessionId", session_id(&s->initial));
    if (extra) { json_object_merge(args, extra); json_free(extra); }
    store_call(name, args, 0, s, mutate_done, TAG_MUTATE, &s->req_mutate);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}

static void confirm_and_mutate(ConversationScreen *s, const char *action) {
    const char *title;
    if (str_eq(action, "delete")) title = "Permanently delete this conversation and its transcript?";
    else if (str_eq(action, "cancel")) title = "Stop the running agent?";
    else if (str_eq(action, "close")) title = "Close this conversation?";
    else if (str_eq(action, "review_loop")) title = "Turn on the review loop? Each push gets a paid review round, and may start one now.";
    else title = "Reopen this conversation?";
    s->dialog_open = true;
    bool ok = app_confirm(title, NULL, "Confirm", str_eq(action, "delete") || str_eq(action, "cancel"));
    s->dialog_open = false;
    if (!ok) return;
    Json *extra = NULL;
    if (str_eq(action, "review_loop")) { extra = json_object(); json_set_bool(extra, "on", true); }
    mutate(s, action, extra);
}

// MARK: - Voice

static void voice_changed(void *ctx) {
    ConversationScreen *s = ctx;
    char *text = voice_take_text(s->voice);
    if (text) {
        if (*text) {
            char *current = composer_text(s);
            size_t n = strlen(current);
            const char *gap = (!n || current[n - 1] == ' ' || current[n - 1] == '\n' || current[n - 1] == '\t') ? "" : " ";
            char *joined = xstrfmt("%s%s%s", current, gap, text);
            set_composer_text(s, joined);
            free(joined); free(current);
        }
        free(text);
    }
    char *error = voice_take_error(s->voice);
    if (error) { app_alert("Voice note", error); free(error); }
    if (voice_state(s->voice) == VOICE_RECORDING) SetTimer(pane_hwnd(s->base.pane), TIMER_VOICE, 1000, NULL); else KillTimer(pane_hwnd(s->base.pane), TIMER_VOICE);
    pane_footer_changed(s->base.pane);
}

// MARK: - Layout

static bool is_expanded(ConversationScreen *s, int seq) { for (size_t i = 0; i < s->expanded_count; i++) if (s->expanded[i] == seq) return true; return false; }
static void toggle_expanded(ConversationScreen *s, int seq) {
    for (size_t i = 0; i < s->expanded_count; i++) if (s->expanded[i] == seq) { s->expanded[i] = s->expanded[--s->expanded_count]; return; }
    s->expanded = xrealloc(s->expanded, (s->expanded_count + 1) * sizeof *s->expanded);
    s->expanded[s->expanded_count++] = seq;
}
static const char *decision_for(ConversationScreen *s, const Json *triage, const Json *finding) {
    const char *key = json_str(json_get(finding, "key"));
    if (!key) return "";
    size_t kl = strlen(key);
    for (size_t i = 0; i < s->decision_count; i++) if (strncmp(s->decisions[i], key, kl) == 0 && s->decisions[i][kl] == '=') return s->decisions[i] + kl + 1;
    const char *d = json_str(json_get(json_get(json_get(json_get(triage, "drafts"), "verdicts"), key), "decision"));
    return d ? d : "";
}
static void set_decision(ConversationScreen *s, const char *key, const char *decision) {
    size_t kl = strlen(key);
    char *entry = xstrfmt("%s=%s", key, decision);
    for (size_t i = 0; i < s->decision_count; i++) if (strncmp(s->decisions[i], key, kl) == 0 && s->decisions[i][kl] == '=') { free(s->decisions[i]); s->decisions[i] = entry; return; }
    s->decisions = xrealloc(s->decisions, (s->decision_count + 1) * sizeof *s->decisions);
    s->decisions[s->decision_count++] = entry;
}
static const char *const triage_options[] = { "fix", "optional", "dismissed" };
static const char *const triage_titles[] = { "Fix", "Optional", "Dismiss" };

// The dashboard's `#messages`: `mx-auto max-w-[860px] px-6 pt-[18px] pb-[30px]`.
static int column_x(int width) { int col = width < px(860) ? width : px(860); return (width - col) / 2 + px(24); }
static int column_w(int width) { int col = width < px(860) ? width : px(860); return col - px(48); }

/// `.ev-log`: a 12px mono line in the muted colour.
static void ev_log(Doc *doc, int x, int w, const char *text, COLORREF color) {
    doc_space(doc, px(1));
    doc_text(doc, x, w, text ? text : "", FONT_MONO_SMALL, color, DT_WORDBREAK | DT_EDITCONTROL);
    doc_space(doc, px(1));
}
/// A `<details>` summary: 13px muted, `▸`/`▾` before it, ink on hover.
typedef struct { char *text; bool open; } SummaryData;
static void summary_free(void *p) { SummaryData *d = p; free(d->text); free(d); }
static void paint_summary(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    SummaryData *d = it->data;
    bool hovered = doc->hover >= 0 && doc_item(doc, doc->hover) == it;
    RECT r = *rc;
    draw_text(hdc, d->text, &r, FONT_FOOTNOTE, hovered ? theme.ink : theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void doc_summary(Doc *doc, int x, int w, const char *text, bool open, int action, intptr_t arg) {
    SummaryData *d = xcalloc(1, sizeof *d);
    d->text = xstrfmt("%s %s", open ? "\xE2\x96\xBE" : "\xE2\x96\xB8", text); d->open = open;
    doc_custom(doc, x, w, px(18), paint_summary, d, summary_free, action, arg);
}
static void paint_left_border(Doc *doc, Item *it, HDC hdc, const RECT *rc) { RECT r = { rc->left, rc->top, rc->left + px(2), rc->bottom }; fill_rect(hdc, &r, theme.line); }

/// One step of a tool block: the tool's name in a chip, then what it did, in mono, on one line.
typedef struct { char *name, *summary; bool error; } StepData;
static void step_free(void *p) { StepData *d = p; free(d->name); free(d->summary); free(d); }
static void paint_step(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    StepData *d = it->data;
    int cw = px(6) * 2 + text_width(hdc, d->name, FONT_CAPTION) + 2, ch = px(19);
    RECT chip = { rc->left, rc->top + (rc->bottom - rc->top - ch) / 2, rc->left + cw, rc->top + (rc->bottom - rc->top - ch) / 2 + ch };
    fill_round_rect(hdc, &chip, px(5), theme.raise, d->error ? theme.danger : theme.line);
    RECT n = { chip.left + px(6), chip.top, chip.right - px(6) + 2, chip.bottom };
    draw_text(hdc, d->name, &n, FONT_CAPTION, d->error ? theme.danger : theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT s = { chip.right + px(8), rc->top, rc->right, rc->bottom };
    draw_text(hdc, d->summary, &s, FONT_MONO_SMALL, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static const char *tool_title(const Event *e) {
    if (!str_empty(e->name)) return e->name;
    if (str_eq(e->kind, "cmd")) return "Command";
    if (str_eq(e->kind, "git")) return "Git";
    if (str_eq(e->kind, "tool_error")) return "Tool error";
    return "Tool";
}
static char *first_line(const char *text) {
    char *trimmed = str_trim(text);
    const char *nl = strchr(trimmed, '\n');
    char *first = nl ? xstrndup(trimmed, (size_t)(nl - trimmed)) : xstrdup(trimmed);
    char *out = str_trim(first);
    free(first); free(trimmed);
    return out;
}
static void layout_step(Doc *doc, int x, int w, const Event *e) {
    StepData *d = xcalloc(1, sizeof *d);
    d->name = xstrdup(tool_title(e)); d->summary = first_line(event_detail(e)); d->error = str_eq(e->kind, "tool_error");
    doc_space(doc, px(3));
    doc_custom(doc, x, w, px(19), paint_step, d, step_free, 0, 0);
    doc_space(doc, px(3));
}
static bool is_tool(const Event *e) { return str_eq(e->kind, "tool") || str_eq(e->kind, "tool_error"); }
static bool is_prep_line(const Event *e) { return (str_eq(e->kind, "info") || str_eq(e->kind, "cmd") || str_eq(e->kind, "git") || str_eq(e->kind, "stdout")) && e->text; }
/// Info lines that belong to the conversation rather than to workspace preparation.
static bool prep_closed(const Event *e) {
    const char *t = e->text ? e->text : "";
    if (strncmp(t, "Starting ", 9) == 0 || strncmp(t, "Started worker ", 15) == 0 || strncmp(t, "Worker ", 7) == 0) return true;
    return str_icontains(t, "session started");
}

static void add_time(Doc *doc, int x, int w, const Event *e, bool right) {
    time_t when;
    if (!event_time(e, &when)) return;
    char *text = format_event_time(when);
    doc_space(doc, px(6));
    doc_text(doc, x, w, text, FONT_CAPTION2, theme.muted, (right ? DT_RIGHT : DT_LEFT) | DT_SINGLELINE);
    free(text);
}

static void paint_queued_box(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    HPEN pen = CreatePen(PS_DOT, 1, theme.line);
    HGDIOBJ old_pen = SelectObject(hdc, pen), old_brush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    RoundRect(hdc, rc->left, rc->top, rc->right, rc->bottom, px(24), px(24));
    SelectObject(hdc, old_pen); SelectObject(hdc, old_brush); DeleteObject(pen);
}

static void paint_working(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    ConversationScreen *s = it->data;
    const char *status = session_status(session(s));
    const char *verb = str_eq(status, "queued") ? "Queued" : (str_eq(status, "preparing") || str_eq(status, "starting")) ? "Starting up" : working_verb(s->tick);
    RECT g = { rc->left, rc->top, rc->left + px(16), rc->bottom };
    draw_textw(hdc, working_glyph(s->tick), &g, FONT_BODY_SEMIBOLD, theme.accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    char *text = xstrfmt("%s\xE2\x80\xA6", verb);
    RECT t = { rc->left + px(24), rc->top, rc->right, rc->bottom };
    draw_text(hdc, text, &t, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    free(text);
}

/// "— $2.9565 · 455s · 57 turns · 5.6M in / 36.4k out · 151.6k context", above a dashed rule.
static void paint_turn_footer(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    draw_dashed_line(hdc, rc->left, rc->top, rc->right, rc->top, theme.line);
    RECT t = { rc->left, rc->top + px(8), rc->right, rc->bottom };
    draw_text(hdc, it->text, &t, FONT_CAPTION, it->color, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void layout_turn_footer(Doc *doc, int x, int w, const Event *e) {
    Str bits; str_init(&bits);
    if (e->has_cost) { char *c = xstrfmt("$%.4f", e->cost_usd); str_appendz(&bits, c); free(c); }
    if (e->has_duration) str_appendf(&bits, "%s%ds", bits.len ? " \xC2\xB7 " : "", (int)(e->duration_ms / 1000));
    double n;
    if (json_num(json_get(e->raw, "numTurns"), &n)) str_appendf(&bits, "%s%d turns", bits.len ? " \xC2\xB7 " : "", (int)n);
    double in = 0, out = 0;
    bool has_in = json_num(json_get(e->raw, "inputTokens"), &in), has_out = json_num(json_get(e->raw, "outputTokens"), &out);
    if (has_in || has_out) { char *a = format_tokens(in), *b = format_tokens(out); str_appendf(&bits, "%s%s in / %s out", bits.len ? " \xC2\xB7 " : "", a, b); free(a); free(b); }
    if (json_num(json_get(e->raw, "tokens"), &n)) { char *c = format_tokens(n); str_appendf(&bits, "%s%s context", bits.len ? " \xC2\xB7 " : "", c); free(c); }
    char *text = xstrfmt("\xE2\x80\x94 %s", bits.len ? bits.data : "turn done");
    str_free(&bits);
    doc_space(doc, px(10));
    int i = doc_custom(doc, x, w, px(8) + px(20), paint_turn_footer, NULL, NULL, 0, 0);
    Item *it = doc_item(doc, i);
    it->text = text; it->color = e->is_error == 1 ? theme.danger : theme.muted;
    doc_space(doc, px(4));
}

static void layout_event(ConversationScreen *s, Doc *doc, int x, int w, const Event *e) {
    if (str_eq(e->kind, "user")) {
        // `rounded-xl border border-line bg-raise px-3.5 py-2.5`, the time right-aligned under the text.
        doc_space(doc, px(18));
        int box = doc_box_begin(doc, x, w, px(10), theme.raise, theme.line, px(12));
        doc_item(doc, box)->hover_fill = false;
        doc_text(doc, x + px(14), w - px(28), e->text, FONT_BODY, theme.ink, DT_WORDBREAK | DT_EDITCONTROL);
        size_t an = json_count(e->attachments);
        for (size_t i = 0; i < an; i++) {
            const char *name = json_str(json_get(json_at(e->attachments, i), "name"));
            doc_space(doc, px(6));
            char *label = xstrfmt("\xF0\x9F\x93\x8E %s", name ? name : "Attachment");
            doc_text(doc, x + px(14), w - px(28), label, FONT_CAPTION, theme.muted, DT_SINGLELINE | DT_END_ELLIPSIS);
            free(label);
        }
        time_t when;
        if (event_time(e, &when)) { char *tm = format_event_time(when); doc_space(doc, px(6)); doc_text(doc, x + px(14), w - px(28), tm, FONT_CAPTION2, theme.muted, DT_RIGHT | DT_SINGLELINE); free(tm); }
        doc_box_end(doc, box, px(10));
        doc_space(doc, px(14));
    } else if (str_eq(e->kind, "text")) {
        doc_space(doc, px(10));
        doc_markdown(doc, x, w, e->text ? e->text : "", FONT_BODY);
        add_time(doc, x, w, e, false);
        doc_space(doc, px(10));
    } else if (str_eq(e->kind, "ask")) {
        doc_space(doc, px(10));
        int box = doc_box_begin(doc, x, w, px(12), theme.raise, theme.accent, px(12));
        doc_item(doc, box)->hover_fill = false;
        int ix = x + px(14), iw = w - px(28);
        doc_text(doc, ix, iw, "Your input is needed", FONT_CAPTION, theme.accent, DT_SINGLELINE);
        doc_space(doc, px(8));
        doc_markdown(doc, ix, iw, e->question ? e->question : (e->text ? e->text : ""), FONT_BODY);
        if (can_message(s)) {
            size_t on = json_count(e->options);
            if (on) doc_space(doc, px(8));
            ButtonSpec *buttons = xcalloc(on ? on : 1, sizeof *buttons); size_t bn = 0;
            for (size_t i = 0; i < on; i++) {
                const char *label = json_str(json_get(json_at(e->options, i), "label"));
                if (!label) continue;
                ButtonSpec b = { 0, label, BUTTON_BORDERED, ACT_ANSWER, (intptr_t)((e->seq << 8) | (int)i), true };
                buttons[bn++] = b;
            }
            if (bn) doc_button_row(doc, ix, iw, buttons, bn);
            free(buttons);
        }
        doc_box_end(doc, box, px(12));
        doc_space(doc, px(10));
    } else if (str_eq(e->kind, "result")) {
        layout_turn_footer(doc, x, w, e);
    } else if (e->text) {
        if (str_eq(e->kind, "cmd")) { char *line = xstrfmt("$ %s", e->text); ev_log(doc, x, w, line, theme.muted); free(line); }
        else ev_log(doc, x, w, e->text, str_eq(e->kind, "stderr") ? theme.danger : theme.muted);
    }
}

static void layout_triage(ConversationScreen *s, Doc *doc, int x, int w, const Json *triage) {
    bool takes_verdicts = json_bool_tristate(json_get(triage, "mine")) != 0;
    int box = doc_box_begin(doc, x, w, px(10), theme.raise, theme.line, px(8));
    doc_item(doc, box)->hover_fill = false;
    int ix = x + px(12), iw = w - px(24);
    Str title; str_init(&title);
    double round, pr;
    if (json_num(json_get(triage, "round"), &round)) str_appendf(&title, "Round %d findings", (int)round); else str_appendz(&title, "Review findings");
    if (json_num(json_get(triage, "prNumber"), &pr)) str_appendf(&title, " \xC2\xB7 PR #%d", (int)pr);
    doc_text(doc, ix, iw, title.data, FONT_BODY_SEMIBOLD, theme.ink, DT_SINGLELINE | DT_END_ELLIPSIS);
    str_free(&title);
    const Json *findings = json_get(triage, "findings");
    size_t n = json_count(findings), fixes = 0;
    for (size_t i = 0; i < n; i++) {
        const Json *f = json_at(findings, i);
        doc_space(doc, px(8)); doc_rule(doc, ix, iw); doc_space(doc, px(8));
        const char *ft = json_str(json_get(f, "title"));
        int ti = doc_text(doc, ix, iw, ft ? ft : "Finding", FONT_FOOTNOTE, theme.ink, DT_WORDBREAK);
        const char *url = json_str(json_get(f, "url"));
        if (safe_web_url(url)) { doc_item(doc, ti)->action = ACT_FINDING_LINK; doc_item(doc, ti)->arg = (intptr_t)i; doc_item(doc, ti)->hand = true; }
        Str meta; str_init(&meta);
        if (json_str(json_get(f, "file"))) { str_appendz(&meta, json_str(json_get(f, "file"))); double line; if (json_num(json_get(f, "line"), &line)) str_appendf(&meta, ":%d", (int)line); }
        if (meta.len) { doc_space(doc, px(3)); doc_text(doc, ix, iw, meta.data, FONT_MONO_CAPTION2, theme.muted, DT_SINGLELINE | DT_END_ELLIPSIS); }
        str_free(&meta);
        const char *why = json_str(json_get(f, "parkedWhy"));
        if (why) { doc_space(doc, px(3)); doc_text(doc, ix, iw, why, FONT_CAPTION, theme.muted, DT_WORDBREAK); }
        if (takes_verdicts && json_str(json_get(f, "key"))) {
            const char *decision = decision_for(s, triage, f);
            int selected = -1;
            for (int k = 0; k < 3; k++) if (str_eq(decision, triage_options[k])) selected = k;
            if (selected == 0) fixes++;
            doc_space(doc, px(6));
            doc_segments(doc, ix, iw, triage_titles, 3, selected, ACT_TRIAGE_DECISION, (intptr_t)(i * 4), !s->busy && !s->uncertain);
        }
    }
    doc_space(doc, px(8)); doc_rule(doc, ix, iw); doc_space(doc, px(8));
    if (takes_verdicts) {
        int nb = doc_box_begin(doc, ix, iw, px(3), theme.field, theme.line, px(4));
        doc_text(doc, ix + px(6), iw - px(12), str_empty(s->triage_note) ? "A note for the pull request and the fix session (optional)" : s->triage_note, FONT_CAPTION2, str_empty(s->triage_note) ? theme.muted : theme.ink, DT_WORDBREAK);
        doc_box_end(doc, nb, px(3));
        doc_box_action(doc, nb, ACT_TRIAGE_NOTE, 0);
        doc_space(doc, px(8));
    }
    char *complete = takes_verdicts ? (fixes ? xstrfmt("Complete \xC2\xB7 send %zu to be fixed", fixes) : xstrdup("Complete \xC2\xB7 nothing to fix, approve and close")) : xstrdup("Complete");
    ButtonSpec b = { 0, complete, BUTTON_PROMINENT, ACT_TRIAGE_COMPLETE, (intptr_t)fixes, !s->busy && !s->uncertain };
    doc_button_row(doc, ix, iw, &b, 1);
    free(complete);
    doc_box_end(doc, box, px(10));
}

static void complete_triage(ConversationScreen *s, size_t fixes) {
    const Json *triage = session_held_triage(session(s));
    if (!triage) return;
    bool takes_verdicts = json_bool_tristate(json_get(triage, "mine")) != 0;
    char *title;
    if (!takes_verdicts) title = xstrdup("Take this review off the queue?");
    else if (!fixes) title = xstrdup("Complete with nothing to fix?");
    else title = xstrfmt("Start a paid fix session for %zu finding%s?", fixes, fixes == 1 ? "" : "s");
    s->dialog_open = true;
    bool ok = app_confirm(title, NULL, "Complete", false);
    s->dialog_open = false;
    free(title);
    if (!ok) return;
    Json *extra = json_object();
    if (takes_verdicts) {
        Json *verdicts = json_array();
        const Json *findings = json_get(triage, "findings");
        for (size_t i = 0; i < json_count(findings); i++) {
            const Json *f = json_at(findings, i);
            const char *key = json_str(json_get(f, "key"));
            if (!key) continue;
            const char *chosen = decision_for(s, triage, f);
            Json *v = json_object();
            json_set_str(v, "key", key); json_set_str(v, "decision", str_empty(chosen) ? "optional" : chosen);
            const char *reason = json_str(json_get(json_get(json_get(json_get(triage, "drafts"), "verdicts"), key), "reason"));
            if (!str_empty(reason)) json_set_str(v, "reason", reason);
            json_array_push(verdicts, v);
        }
        if (json_count(verdicts)) json_object_set(extra, "verdicts", verdicts); else json_free(verdicts);
        char *note = str_trim(s->triage_note);
        if (*note) json_set_str(extra, "note", note);
        free(note);
    }
    mutate(s, "complete_findings", extra);
}

static void conversation_layout(Screen *base, Doc *doc) {
    ConversationScreen *s = (ConversationScreen *)base;
    int x = column_x(doc->width), w = column_w(doc->width);
    doc_space(doc, px(18));
    if (s->error) { doc_notice_box(doc, x, w, s->error); doc_space(doc, px(12)); }
    if (s->write_error) {
        int box = doc_box_begin(doc, x, w, px(10), blend(theme.danger, theme.canvas, 0.15), theme.danger, px(8));
        doc_item(doc, box)->hover_fill = false;
        doc_notice(doc, x + px(12), w - px(24), s->write_error);
        doc_space(doc, px(4));
        doc_text(doc, x + px(12), w - px(24), "The action may have completed. Check the latest conversation before trying again.", FONT_CAPTION, theme.muted, DT_WORDBREAK);
        doc_space(doc, px(8));
        doc_button(doc, x + px(12), 0, "Refresh and check outcome", BUTTON_BORDERED, ACT_REFRESH_OUTCOME, 0, !s->loading && !s->busy);
        doc_box_end(doc, box, px(10));
        doc_space(doc, px(12));
    }
    size_t visible = 0;
    for (size_t i = 0; i < s->transcript.count; i++) if (event_visible(&s->transcript.events[i])) visible++;
    if (!visible && !s->error) doc_text(doc, x, w, s->loaded ? "No messages yet." : "Waiting for the conversation\xE2\x80\xA6", FONT_FOOTNOTE, theme.muted, DT_LEFT);
    // The dashboard folds workspace preparation into one block per turn and each run of tool calls into another.
    bool prep_done = false;
    size_t i = 0;
    while (i < s->transcript.count) {
        const Event *e = &s->transcript.events[i];
        if (str_eq(e->kind, "user") || str_eq(e->kind, "result")) prep_done = false;
        if (!prep_done && is_prep_line(e) && !prep_closed(e)) {
            size_t j = i, n = 0;
            while (j < s->transcript.count && is_prep_line(&s->transcript.events[j]) && !prep_closed(&s->transcript.events[j])) { j++; n++; }
            bool open = is_expanded(s, e->seq);
            char *summary = xstrfmt("Preparing workspace\xE2\x80\xA6 (%zu step%s)", n, n == 1 ? "" : "s");
            doc_space(doc, px(8));
            int top = doc->y;
            int inner_x = open ? x + px(12) : x, inner_w = open ? w - px(12) : w;
            doc_summary(doc, inner_x, inner_w, summary, open, ACT_TOOL_TOGGLE, e->seq);
            free(summary);
            if (open) {
                for (size_t k = i; k < j; k++) {
                    const Event *line = &s->transcript.events[k];
                    char *text = str_eq(line->kind, "cmd") ? xstrfmt("$ %s", line->text) : xstrdup(line->text);
                    ev_log(doc, inner_x, inner_w, text, theme.muted);
                    free(text);
                }
                RECT bar = { x, top, x + px(2), doc->y };
                doc_add(doc, &bar, paint_left_border);
            }
            doc_space(doc, px(8));
            i = j;
            continue;
        }
        if (is_prep_line(e) && prep_closed(e)) prep_done = true;
        if (is_tool(e)) {
            size_t j = i, n = 0;
            while (j < s->transcript.count && is_tool(&s->transcript.events[j])) { j++; n++; }
            const Event *last = &s->transcript.events[j - 1];
            bool open = is_expanded(s, e->seq);
            char *summary = xstrfmt("%zu step%s \xC2\xB7 %s", n, n == 1 ? "" : "s", tool_title(last));
            doc_space(doc, px(8));
            int top = doc->y;
            int inner_x = open ? x + px(12) : x, inner_w = open ? w - px(12) : w;
            doc_summary(doc, inner_x, inner_w, summary, open, ACT_TOOL_TOGGLE, e->seq);
            free(summary);
            if (open) {
                for (size_t k = i; k < j; k++) layout_step(doc, inner_x, inner_w, &s->transcript.events[k]);
                RECT bar = { x, top, x + px(2), doc->y };
                doc_add(doc, &bar, paint_left_border);
            }
            doc_space(doc, px(8));
            i = j;
            continue;
        }
        if (event_visible(e)) layout_event(s, doc, x, w, e);
        i++;
    }
    const Json *queued = session_queued(session(s));
    for (size_t q = 0; q < json_count(queued); q++) {
        const char *text = json_str(json_get(json_at(queued, q), "text"));
        if (!text) text = "Message";
        doc_space(doc, px(10));
        int th = measure_text(doc->hdc, text, w - px(28), FONT_BODY, DT_WORDBREAK);
        RECT br = { x, doc->y, x + w, doc->y + th + px(20) };
        doc_add(doc, &br, paint_queued_box);
        RECT tr = { br.left + px(14), br.top + px(10), br.right - px(14), br.bottom - px(10) };
        doc_text_at(doc, &tr, text, FONT_BODY, theme.muted, DT_WORDBREAK | DT_EDITCONTROL);
        doc->y = br.bottom + px(4);
        bool removable = store_supports("drop_message") && !s->busy && !s->uncertain;
        int lh = px(18);
        RECT qr = { x, doc->y, x + w - px(60), doc->y + lh };
        doc_text_at(doc, &qr, "Queued for the next turn", FONT_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (removable) {
            RECT rr = { x + w - px(60), doc->y, x + w, doc->y + lh };
            int ri = doc_text_at(doc, &rr, "Remove", FONT_CAPTION2, theme.danger, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            doc_item(doc, ri)->action = ACT_DROP_QUEUED; doc_item(doc, ri)->arg = (intptr_t)q; doc_item(doc, ri)->hand = true;
        }
        doc->y += lh;
    }
    const Json *triage = session_held_triage(session(s));
    if (triage && store_supports("complete_findings")) { doc_space(doc, px(14)); layout_triage(s, doc, x, w, triage); }
    if (session_is_active(session(s))) {
        doc_space(doc, px(12));
        int wi = doc_custom(doc, x, w, px(24), paint_working, s, NULL, 0, 0);
        doc_item(doc, wi)->id = 2;
    }
    doc_space(doc, px(30));
    RECT anchor = { x, doc->y, x + w, doc->y + 1 };
    int ai = doc_add(doc, &anchor, NULL); doc_item(doc, ai)->id = 1;
    doc->y += 1;
}

// MARK: - Header and menu

static void header_button(HeaderInfo *info, wchar_t glyph, const char *label, int action, bool enabled, bool destructive) {
    if (info->button_count >= HEADER_BUTTONS) return;
    HeaderButton *b = &info->buttons[info->button_count++];
    b->glyph = glyph; b->action = action; b->enabled = enabled; b->destructive = destructive;
    snprintf(b->label, sizeof b->label, "%s", label);
}
/// `#chat-sub`: provider · model · effort · state · 🔁 loop round · branch · tokens · cost · session id · the pull request.
static char *status_line(const Session *ss) {
    Str sub; str_init(&sub);
    const char *parts[3] = { session_provider(ss), session_model(ss), json_str_nonempty(json_get(ss->raw, "effort")) };
    for (int i = 0; i < 3; i++) if (parts[i]) str_appendf(&sub, "%s%s", sub.len ? " \xC2\xB7 " : "", parts[i]);
    const char *state = str_eq(session_status(ss), "idle") && json_bool_is(json_get(ss->raw, "awaitingAnswer"), true) ? "waiting" : session_status(ss);
    str_appendf(&sub, "%s%s", sub.len ? " \xC2\xB7 " : "", state);
    const Json *loop = json_get(ss->raw, "reviewLoop");
    double round;
    if (json_is_object(loop)) { if (json_num(json_get(loop, "rounds"), &round)) str_appendf(&sub, " \xC2\xB7 \xF0\x9F\x94\x81 loop round %d", (int)round); else str_appendz(&sub, " \xC2\xB7 \xF0\x9F\x94\x81 loop"); }
    const char *branch = json_str_nonempty(json_get(ss->raw, "branch"));
    if (branch) str_appendf(&sub, " \xC2\xB7 %s", branch);
    double in = 0, out = 0, cost;
    bool has_in = json_num(json_get(ss->raw, "inputTokens"), &in), has_out = json_num(json_get(ss->raw, "outputTokens"), &out);
    if (has_in || has_out) { char *tk = format_tokens(in + out); str_appendf(&sub, " \xC2\xB7 %s tok", tk); free(tk); }
    if (json_num(json_get(ss->raw, "costUsd"), &cost)) { char *c = format_cost(cost); str_appendf(&sub, " \xC2\xB7 %s", c); free(c); }
    str_appendf(&sub, " \xC2\xB7 session %s", session_id(ss));
    const Json *pr = json_get(ss->raw, "prStatus");
    double number;
    if (json_is_object(pr) && json_num(json_get(pr, "number"), &number)) {
        const char *pstate = json_str(json_get(pr, "state"));
        bool draft = json_bool_is(json_get(pr, "draft"), true);
        const char *light = str_eq(pstate, "merged") ? "\xF0\x9F\x9F\xA3" : str_eq(pstate, "closed") ? "\xF0\x9F\x94\xB4" : draft ? "\xE2\x9A\xAA" : "\xF0\x9F\x9F\xA2";
        str_appendf(&sub, " \xC2\xB7 %s PR #%d %s", light, (int)number, draft && str_eq(pstate, "open") ? "draft" : pstate ? pstate : "");
        int failed = json_int_or(json_get(json_get(pr, "checks"), "failed"), 0), pending = json_int_or(json_get(json_get(pr, "checks"), "pending"), 0), passed = json_int_or(json_get(json_get(pr, "checks"), "passed"), 0);
        if (failed) str_appendf(&sub, " \xC2\xB7 \xE2\x9C\x97%d", failed);
        else if (pending) str_appendf(&sub, " \xC2\xB7 \xE2\x80\xA6%d", pending);
        else if (passed) str_appendf(&sub, " \xC2\xB7 \xE2\x9C\x93%d", passed);
    }
    return str_detach(&sub);
}
static void conversation_header(Screen *base, HeaderInfo *info) {
    ConversationScreen *s = (ConversationScreen *)base;
    const Session *ss = session(s);
    snprintf(info->title, sizeof info->title, "%s", session_display_title(ss));
    char *sub = status_line(ss);
    snprintf(info->subtitle, sizeof info->subtitle, "%s", sub);
    free(sub);
    if (store_supports("rename")) info->title_action = ACT_MENU_ITEM + MENU_RENAME;
    bool can = !s->busy && !s->uncertain;
    bool closed = str_eq(session_status(ss), "closed");
    if (store_supports("cancel") && session_is_active(ss)) header_button(info, 0xE71A, "\xE2\x8F\xB9 Stop", ACT_MENU_ITEM + MENU_STOP, can, false);
    if (store_supports("reopen") && closed) header_button(info, 0xE7A7, "\xE2\x9F\xB3 Reopen", ACT_MENU_ITEM + MENU_REOPEN, can, false);
    if (store_supports("close") && !closed) header_button(info, 0xE8BB, "Close", ACT_MENU_ITEM + MENU_CLOSE, can, false);
    if (store_supports("delete")) header_button(info, 0xE74D, "\xF0\x9F\x97\x91 Delete", ACT_MENU_ITEM + MENU_DELETE, can, true);
}

static void menu_choice(ConversationScreen *s, int chosen) {
    const Session *ss = session(s);
    int number = session_pull_number(ss);
    const char *repo = session_repo(ss);
    if ((chosen == MENU_CHANGES || chosen == MENU_PULL) && !(number && repo)) return;
    switch (chosen) {
    case MENU_CHANGES: { Project p = { xstrdup(repo), NULL }; app_push_detail(pull_files_screen_new(&p, number)); project_free(&p); break; }
    case MENU_PULL: { Project p = { xstrdup(repo), NULL }; app_push_detail(pull_detail_screen_new(&p, number, NULL, NULL)); project_free(&p); break; }
    case MENU_COPY: break;
    case MENU_LOOP_OFF: { Json *extra = json_object(); json_set_bool(extra, "on", false); mutate(s, "review_loop", extra); break; }
    case MENU_LOOP_ON: confirm_and_mutate(s, "review_loop"); break;
    case MENU_RENAME: {
        s->renaming = true; s->dialog_open = true;
        char *title = dialog_rename(app_window(), session_display_title(ss));
        s->renaming = false; s->dialog_open = false;
        if (title) { char *trimmed = str_trim(title); if (*trimmed) { Json *extra = json_object(); json_set_str(extra, "title", title); mutate(s, "rename", extra); } free(trimmed); free(title); }
        break;
    }
    case MENU_STOP: confirm_and_mutate(s, "cancel"); break;
    case MENU_CLOSE: confirm_and_mutate(s, "close"); break;
    case MENU_REOPEN: confirm_and_mutate(s, "reopen"); break;
    case MENU_DELETE: confirm_and_mutate(s, "delete"); break;
    }
}

static void conversation_action(Screen *base, int action, intptr_t arg, POINT pt) {
    ConversationScreen *s = (ConversationScreen *)base;
    if (action > ACT_MENU_ITEM && action <= ACT_MENU_ITEM + MENU_COPY) { menu_choice(s, action - ACT_MENU_ITEM); return; }
    switch (action) {
    case ACT_ANSWER: {
        int seq = (int)(arg >> 8), index = (int)(arg & 0xFF);
        for (size_t i = 0; i < s->transcript.count; i++) {
            const Event *e = &s->transcript.events[i];
            if (e->seq != seq) continue;
            const char *label = json_str(json_get(json_at(e->options, (size_t)index), "label"));
            if (label) { set_composer_text(s, label); SetFocus(s->composer); }
            break;
        }
        break;
    }
    case ACT_TOOL_TOGGLE: toggle_expanded(s, (int)arg); pane_relayout(base->pane); break;
    case ACT_DROP_QUEUED: { Json *extra = json_object(); json_set_num(extra, "index", (double)arg); mutate(s, "drop_message", extra); break; }
    case ACT_TRIAGE_DECISION: {
        const Json *triage = session_held_triage(session(s));
        size_t index = (size_t)(arg / 4); int decision = (int)(arg % 4);
        const char *key = json_str(json_get(json_at(json_get(triage, "findings"), index), "key"));
        if (key && decision >= 0 && decision < 3) { set_decision(s, key, triage_options[decision]); pane_relayout(base->pane); }
        break;
    }
    case ACT_TRIAGE_NOTE: {
        s->dialog_open = true;
        char *note = dialog_rename(app_window(), s->triage_note ? s->triage_note : "");
        s->dialog_open = false;
        if (note) { set_string(&s->triage_note, note); free(note); pane_relayout(base->pane); }
        break;
    }
    case ACT_TRIAGE_COMPLETE: complete_triage(s, (size_t)arg); break;
    case ACT_FINDING_LINK: {
        const Json *triage = session_held_triage(session(s));
        const char *url = json_str(json_get(json_at(json_get(triage, "findings"), (size_t)arg), "url"));
        open_web_url(url);
        break;
    }
    case ACT_REFRESH_OUTCOME: s->uncertain = false; set_string(&s->write_error, NULL); refresh(s, false); pane_relayout(base->pane); break;
    case ACT_REOPEN: confirm_and_mutate(s, "reopen"); break;
    }
}
// MARK: - Footer (composer)

enum { CHIP_WORKSPACE, CHIP_PROJECT, CHIP_BRANCH, CHIP_PROVIDER, CHIP_MODEL, CHIP_EFFORT, CHIP_LOOP, CHIP_COUNT };
static RECT g_chip_rc[CHIP_COUNT];

static int composer_height(ConversationScreen *s, HDC hdc) {
    int line_h = px(24);
    int lines = s->composer_lines < 1 ? 1 : s->composer_lines > 8 ? 8 : s->composer_lines;
    (void)hdc;
    return lines * line_h;
}
static char *chip_text(ConversationScreen *s, int chip) {
    const Session *ss = session(s);
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
    const Session *ss = session(s);
    if (chip == CHIP_LOOP) return store_supports("review_loop") && session_can_review_loop(ss);
    char *text = chip_text(s, chip); bool shown = *text != 0; free(text);
    return shown;
}
static bool chip_live(int chip) { return chip == CHIP_LOOP; }
static int chips_layout(ConversationScreen *s, HDC hdc, int width, RECT *out) {
    int x = 0, y = 0, h = px(24), gap = px(4);
    for (int c = 0; c < CHIP_COUNT; c++) {
        if (out) SetRectEmpty(&out[c]);
        if (!chip_shown(s, c)) continue;
        char *label = chip_text(s, c);
        int w = px(6) * 2 + text_width(hdc, label, FONT_CAPTION) + px(4);
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
static int conversation_footer_height(Screen *base, int width) {
    ConversationScreen *s = (ConversationScreen *)base;
    HDC hdc = GetDC(pane_hwnd(base->pane));
    int inner = width - px(24) * 2; if (inner > px(860)) inner = px(860);
    int chips = chips_layout(s, hdc, inner, NULL);
    int box = can_message(s) ? px(10) + composer_height(s, hdc) + px(6) + px(30) + px(8) + 2 : px(30) + px(20) + 2;
    int h = px(8) + chips + px(8) + box + px(6) + px(16) + px(14);
    ReleaseDC(pane_hwnd(base->pane), hdc);
    return h;
}
static void conversation_footer_layout(Screen *base, const RECT *rc) {
    ConversationScreen *s = (ConversationScreen *)base;
    if (!can_message(s)) { ShowWindow(s->composer, SW_HIDE); return; }
    HDC hdc = GetDC(pane_hwnd(base->pane));
    RECT col = footer_column(rc);
    int chips = chips_layout(s, hdc, col.right - col.left, NULL);
    int ch = composer_height(s, hdc);
    ReleaseDC(pane_hwnd(base->pane), hdc);
    int top = col.top + px(8) + chips + px(8);
    RECT er = { col.left + px(12) + 1, top + px(10) + 1, col.right - px(12) - 1, top + px(10) + 1 + ch };
    s->composer_rc = er;
    MoveWindow(s->composer, er.left, er.top, er.right - er.left, er.bottom - er.top, TRUE);
    ShowWindow(s->composer, SW_SHOW);
}
static void conversation_footer_paint(Screen *base, HDC hdc, const RECT *rc) {
    ConversationScreen *s = (ConversationScreen *)base;
    const Session *ss = session(s);
    fill_rect(hdc, rc, theme.canvas);
    RECT col = footer_column(rc);
    int width = col.right - col.left;
    RECT rects[CHIP_COUNT];
    int chips = chips_layout(s, hdc, width, rects);
    int y0 = col.top + px(8);
    bool can = !s->busy && !s->uncertain;
    for (int c = 0; c < CHIP_COUNT; c++) {
        SetRectEmpty(&g_chip_rc[c]);
        if (IsRectEmpty(&rects[c])) continue;
        RECT r = { col.left + rects[c].left, y0 + rects[c].top, col.left + rects[c].right, y0 + rects[c].bottom };
        if (chip_live(c) && can) g_chip_rc[c] = r;
        bool on = (c == CHIP_LOOP && session_review_loop_on(ss));
        fill_round_rect(hdc, &r, px(6), theme.raise, on ? theme.accent : theme.line);
        char *label = chip_text(s, c);
        RECT t = { r.left + px(6), r.top, r.right - px(6) + 2, r.bottom };
        draw_text(hdc, label, &t, FONT_CAPTION, on ? theme.accent : chip_live(c) && can ? theme.ink : theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        free(label);
    }
    int top = y0 + chips + px(8);
    memset(&s->mic_rc, 0, sizeof s->mic_rc); memset(&s->send_rc, 0, sizeof s->send_rc); memset(&s->discard_rc, 0, sizeof s->discard_rc);
    if (!can_message(s)) {
        RECT box = { col.left, top, col.right, top + px(30) + px(20) + 2 };
        fill_round_rect(hdc, &box, px(16), theme.raise, theme.line);
        bool reopen = store_supports("reopen") && str_eq(session_status(ss), "closed");
        int bw = reopen ? px(10) * 2 + text_width(hdc, "\xE2\x9F\xB3 Reopen", FONT_FOOTNOTE) + 2 : 0;
        RECT t = { box.left + px(12), box.top, box.right - bw - px(24), box.bottom };
        draw_text(hdc, store_can_manage() ? "This session is closed." : "Read-only access", &t, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (reopen) {
            RECT b = { box.right - px(12) - bw, (box.top + box.bottom) / 2 - px(15), box.right - px(12), (box.top + box.bottom) / 2 + px(15) };
            s->send_rc = b;
            fill_round_rect(hdc, &b, px(7), theme.raise, can ? theme.line : theme.line);
            draw_text(hdc, "\xE2\x9F\xB3 Reopen", &b, FONT_FOOTNOTE, can ? theme.ink : theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        return;
    }
    int ch = composer_height(s, hdc);
    RECT box = { col.left, top, col.right, top + px(10) + ch + px(6) + px(30) + px(8) + 2 };
    fill_round_rect(hdc, &box, px(16), theme.raise, GetFocus() == s->composer ? theme.line_strong : theme.line);
    int row_y = box.top + px(10) + 1 + ch + px(6), bh = px(30), bw = px(32);
    bool trimmed_empty = composer_empty(s);
    bool active = session_is_active(ss);
    VoiceState vs = s->voice ? voice_state(s->voice) : VOICE_IDLE;
    // `#btn-send`: 32×30, the accent when there is something to send; a stop square while the agent works.
    RECT send = { box.right - px(12) - bw, row_y, box.right - px(12), row_y + bh };
    s->send_rc = send;
    if (active && store_supports("cancel") && trimmed_empty) {
        fill_round_rect(hdc, &send, px(8), theme.raise, theme.line);
        draw_text(hdc, "\xE2\x96\xA0", &send, FONT_CAPTION, theme.ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        bool enabled = can && !trimmed_empty;
        fill_round_rect(hdc, &send, px(8), enabled || s->busy ? theme.accent : theme.field, enabled || s->busy ? theme.accent : theme.field);
        draw_text(hdc, "\xE2\x86\xB5", &send, FONT_BODY, enabled || s->busy ? theme.on_accent : theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    int x = box.left + px(12);
    if (store_can_transcribe()) {
        RECT mic = { x, row_y, x + bw, row_y + bh };
        s->mic_rc = mic;
        bool rec = vs == VOICE_RECORDING;
        fill_round_rect(hdc, &mic, px(8), rec ? theme.danger : theme.raise, rec ? theme.danger : theme.line);
        draw_text(hdc, rec ? "\xE2\x96\xA0" : (vs == VOICE_TRANSCRIBING || vs == VOICE_STARTING) ? "\xE2\x80\xA6" : "\xF0\x9F\x8E\xA4", &mic, FONT_EMOJI_LARGE, rec ? theme.white : theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        x += bw + px(6);
        if (rec || vs == VOICE_TRANSCRIBING) {
            RECT discard = { x, row_y, x + bw, row_y + bh };
            s->discard_rc = discard;
            fill_round_rect(hdc, &discard, px(8), theme.raise, theme.line);
            draw_text(hdc, "\xE2\x9C\x95", &discard, FONT_CAPTION, theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            x += bw + px(6);
            if (rec) { char *clock = format_clock(voice_elapsed(s->voice)); RECT cr = { x, row_y, x + px(60), row_y + bh }; draw_text(hdc, clock, &cr, FONT_CAPTION, theme.danger, DT_LEFT | DT_VCENTER | DT_SINGLELINE); free(clock); }
        }
    }
    // `#composer-note`: what a message sent now does.
    RECT note = { col.left, box.bottom + px(6), col.right, box.bottom + px(6) + px(16) };
    const char *text = active ? (session_live_input(ss) ? "Sent into the running turn" : "Queued for the next turn") : "";
    draw_text(hdc, text, &note, FONT_FOOTNOTE, theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static bool in_rect(const RECT *r, POINT pt) { return pt.x >= r->left && pt.x < r->right && pt.y >= r->top && pt.y < r->bottom; }
static void conversation_footer_click(Screen *base, POINT pt) {
    ConversationScreen *s = (ConversationScreen *)base;
    if (!IsRectEmpty(&g_chip_rc[CHIP_LOOP]) && in_rect(&g_chip_rc[CHIP_LOOP], pt)) { menu_choice(s, session_review_loop_on(session(s)) ? MENU_LOOP_OFF : MENU_LOOP_ON); return; }
    if (!can_message(s)) { if (in_rect(&s->send_rc, pt) && !s->busy && !s->uncertain) confirm_and_mutate(s, "reopen"); return; }
    if (in_rect(&s->send_rc, pt)) {
        bool active = session_is_active(session(s));
        if (active && store_supports("cancel") && composer_empty(s)) { if (!s->busy && !s->uncertain) confirm_and_mutate(s, "cancel"); }
        else send_message(s);
        return;
    }
    if (in_rect(&s->mic_rc, pt) && s->voice) {
        VoiceState vs = voice_state(s->voice);
        if (vs == VOICE_RECORDING) voice_stop(s->voice); else if (vs == VOICE_IDLE) voice_record(s->voice);
        pane_footer_changed(base->pane);
        return;
    }
    if (in_rect(&s->discard_rc, pt) && s->voice) { voice_drop(s->voice); pane_footer_changed(base->pane); return; }
    SetFocus(s->composer);
}
static void conversation_command(Screen *base, int id, int code, HWND control) {
    ConversationScreen *s = (ConversationScreen *)base;
    if (id == ID_COMPOSER && code == EN_CHANGE) {
        int lines = (int)SendMessageW(control, EM_GETLINECOUNT, 0, 0);
        if (lines != s->composer_lines) { s->composer_lines = lines; pane_footer_changed(base->pane); }
        else { RECT rc; GetClientRect(pane_hwnd(base->pane), &rc); rc.top = rc.bottom - conversation_footer_height(base, rc.right); InvalidateRect(pane_hwnd(base->pane), &rc, FALSE); }
    }
}

// MARK: - Lifecycle

static void conversation_timer(Screen *base, UINT id) {
    ConversationScreen *s = (ConversationScreen *)base;
    if (id == TIMER_WORKING) {
        s->tick++;
        pane_repaint(base->pane);
        return;
    }
    if (id == TIMER_VOICE) { if (s->voice) voice_tick(s->voice); pane_footer_changed(base->pane); return; }
    if (poller_fired(&s->poller, id)) {
        bool enabled = !s->busy && !s->renaming && !s->dialog_open;
        if (enabled && !s->loading) refresh(s, false); else poller_finished(&s->poller, false, -1);
    }
}
static void show_panel(ConversationScreen *s) {
    int number = session_pull_number(session(s));
    const char *repo = session_repo(session(s));
    if (number && repo && store_supports("pull") && pane_root(s->base.pane) == &s->base) app_set_panel(pull_panel_screen_new(repo, number));
    else app_set_panel(NULL);
}
static void conversation_visible(Screen *base, bool shown) {
    ConversationScreen *s = (ConversationScreen *)base;
    if (shown) {
        show_panel(s);
        pane_stick_to_bottom(base->pane, true); pane_show_bottom_button(base->pane, true);
        poller_start(&s->poller, base->pane, TIMER_POLL, session_is_active(session(s)) ? 2000 : 7000);
        if (session_is_active(session(s))) SetTimer(pane_hwnd(base->pane), TIMER_WORKING, 120, NULL);
        pane_scroll_to_bottom(base->pane);
    } else {
        poller_stop(&s->poller); KillTimer(pane_hwnd(base->pane), TIMER_WORKING); KillTimer(pane_hwnd(base->pane), TIMER_VOICE);
        request_cancel(&s->req_refresh); s->loading = false;
        if (s->voice) voice_drop(s->voice);
        ShowWindow(s->composer, SW_HIDE);
    }
}
static void conversation_refresh(Screen *base) { ConversationScreen *s = (ConversationScreen *)base; refresh(s, true); }
static void conversation_scrolled(Screen *base, bool at_bottom) { ConversationScreen *s = (ConversationScreen *)base; s->at_bottom = at_bottom; pane_stick_to_bottom(base->pane, at_bottom); }
static void conversation_activated(Screen *base, bool active) {
    ConversationScreen *s = (ConversationScreen *)base;
    // Recording cannot go on in the background: what was said until then is transcribed, as if stopped.
    if (!active && s->voice && voice_state(s->voice) == VOICE_RECORDING) voice_stop(s->voice);
    if (active) poller_start(&s->poller, base->pane, TIMER_POLL, session_is_active(session(s)) ? 2000 : 7000);
}
static bool conversation_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    ConversationScreen *s = (ConversationScreen *)base;
    (void)shift;
    if (vk == VK_F5) { refresh(s, true); return true; }
    if (ctrl && vk == 'R') { refresh(s, true); return true; }
    return false;
}
static void conversation_destroy(Screen *base) {
    ConversationScreen *s = (ConversationScreen *)base;
    request_cancel(&s->req_refresh); request_cancel(&s->req_mutate);
    poller_stop(&s->poller);
    if (base->pane) { KillTimer(pane_hwnd(base->pane), TIMER_WORKING); KillTimer(pane_hwnd(base->pane), TIMER_VOICE); }
    if (s->voice) voice_free(s->voice);
    if (s->composer) DestroyWindow(s->composer);
    session_free(&s->initial); if (s->has_snapshot) session_free(&s->snapshot);
    transcript_free(&s->transcript);
    free(s->error); free(s->write_error); free(s->pending_mutation); free(s->expanded); free(s->triage_note);
    str_array_free(s->decisions, s->decision_count);
    screen_release(base);
}

static const ScreenVTable conversation_vt = {
    .destroy = conversation_destroy, .layout = conversation_layout, .header = conversation_header, .action = conversation_action,
    .timer = conversation_timer, .footer_height = conversation_footer_height,
    .footer_layout = conversation_footer_layout, .footer_paint = conversation_footer_paint, .footer_click = conversation_footer_click,
    .visible = conversation_visible, .command = conversation_command, .key = conversation_key, .refresh = conversation_refresh,
    .scrolled = conversation_scrolled, .activated = conversation_activated,
};

Screen *conversation_screen_new(const Session *initial) {
    ConversationScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &conversation_vt; s->base.id = xstrfmt("conversation:%s", session_id(initial));
    session_copy(&s->initial, initial);
    transcript_init(&s->transcript);
    s->composer_lines = 1; s->at_bottom = true;
    HWND parent = pane_hwnd(app_detail_pane());
    s->composer = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)ID_COMPOSER, GetModuleHandleW(NULL), NULL);
    SendMessageW(s->composer, WM_SETFONT, (WPARAM)font(FONT_BODY), TRUE);
    SendMessageW(s->composer, EM_SETCUEBANNER, TRUE, (LPARAM)L"Reply\x2026");
    SetWindowSubclass(s->composer, composer_proc, ID_COMPOSER, (DWORD_PTR)s);
    theme_apply_control(s->composer);
    if (store_can_transcribe()) s->voice = voice_new(voice_changed, s);
    return &s->base;
}
