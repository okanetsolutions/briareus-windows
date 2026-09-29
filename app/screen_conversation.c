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
    ACT_REFRESH_OUTCOME, ACT_REOPEN, ACT_MESSAGE_TEXT, ACT_FINDING_LINK,
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
        char *key = cache_key(s); cache_remove(g_store.cache, key); free(key);
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

typedef struct { char *text; COLORREF color; bool failed; wchar_t glyph; } FooterData;
static void footer_free(void *p) { FooterData *d = p; free(d->text); free(d); }
static void paint_turn_footer(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    FooterData *d = it->data;
    RECT g = { rc->left, rc->top, rc->left + px(16), rc->bottom };
    draw_glyph(hdc, d->glyph, &g, FONT_ICON_SMALL, d->color);
    RECT t = { rc->left + px(20), rc->top, rc->right, rc->bottom };
    draw_text(hdc, d->text, &t, FONT_CAPTION, theme.secondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

typedef struct { wchar_t glyph; bool error; char *title, *summary; bool expandable, expanded; } ToolData;
static void tool_free(void *p) { ToolData *d = p; free(d->title); free(d->summary); free(d); }
static void paint_tool(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    ToolData *d = it->data;
    bool hovered = d->expandable && doc->hover >= 0 && doc_item(doc, doc->hover) == it;
    if (hovered) fill_round_rect(hdc, rc, px(6), blend(theme.text, theme.background, 0.04), blend(theme.text, theme.background, 0.04));
    COLORREF c = d->error ? theme.danger : theme.secondary;
    RECT g = { rc->left, rc->top + px(3), rc->left + px(22), rc->top + px(25) };
    fill_round_rect(hdc, &g, px(6), blend(c, theme.background, 0.12), blend(c, theme.background, 0.12));
    draw_glyph(hdc, d->glyph, &g, FONT_ICON_SMALL, c);
    int x = rc->left + px(30);
    int tw = text_width(hdc, d->title, FONT_SUBHEADLINE_SEMIBOLD);
    RECT t = { x, rc->top, x + tw, rc->bottom };
    draw_text(hdc, d->title, &t, FONT_SUBHEADLINE_SEMIBOLD, d->error ? theme.danger : theme.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    x += tw + px(8);
    int right = rc->right - (d->expandable ? px(20) : 0);
    if (x < right) { RECT sr = { x, rc->top, right, rc->bottom }; draw_text(hdc, d->summary, &sr, FONT_MONO_SMALL, theme.secondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS); }
    if (d->expandable) { RECT cr = { rc->right - px(18), rc->top, rc->right, rc->bottom }; draw_glyph(hdc, d->expanded ? 0xE70D : 0xE76C, &cr, FONT_ICON_SMALL, theme.tertiary); }
}
static const char *tool_title(const Event *e) {
    if (!str_empty(e->name)) return e->name;
    if (str_eq(e->kind, "cmd")) return "Command";
    if (str_eq(e->kind, "git")) return "Git";
    if (str_eq(e->kind, "tool_error")) return "Tool error";
    return "Tool";
}
static void layout_tool(ConversationScreen *s, Doc *doc, int x, int w, const Event *e) {
    const char *detail = event_detail(e);
    char *trimmed = str_trim(detail);
    bool has_details = *trimmed != 0;
    ToolData *d = xcalloc(1, sizeof *d);
    d->error = str_eq(e->kind, "tool_error");
    d->glyph = tool_glyph(e->kind, e->name, d->error);
    d->title = xstrdup(tool_title(e));
    const char *nl = strchr(trimmed, '\n');
    char *first = nl ? xstrndup(trimmed, (size_t)(nl - trimmed)) : xstrdup(trimmed);
    d->summary = str_trim(first); free(first);
    d->expandable = has_details; d->expanded = has_details && is_expanded(s, e->seq);
    doc_custom(doc, x, w, px(30), paint_tool, d, tool_free, has_details ? ACT_TOOL_TOGGLE : 0, e->seq);
    if (d->expanded) {
        // Long tool output is cut to what fits a screen, like the dashboard's scrolling box.
        size_t lines = 0; const char *cut = NULL;
        for (const char *p = trimmed; *p; p++) if (*p == '\n' && ++lines >= 60) { cut = p; break; }
        char *shown = cut ? xstrfmt("%.*s\n\xE2\x80\xA6", (int)(cut - trimmed), trimmed) : xstrdup(trimmed);
        int box = doc_box_begin(doc, x + px(30), w - px(30), px(10), theme.code, theme.border, px(10));
        doc_item(doc, box)->hover_fill = false;
        int ti = doc_text(doc, x + px(40), w - px(50), shown, FONT_MONO_SMALL, theme.text, DT_WORDBREAK | DT_EXPANDTABS);
        doc_item(doc, ti)->action = ACT_MESSAGE_TEXT;
        doc_box_end(doc, box, px(10));
        free(shown);
        doc_space(doc, px(4));
    }
    free(trimmed);
}

static void add_time(Doc *doc, int x, int w, const Event *e, bool right) {
    time_t when;
    if (!event_time(e, &when)) return;
    char *text = format_event_time(when);
    doc_space(doc, px(3));
    doc_text(doc, x, w, text, FONT_CAPTION2, theme.tertiary, (right ? DT_RIGHT : DT_LEFT) | DT_SINGLELINE);
    free(text);
}

static void paint_queued_bubble(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    HPEN pen = CreatePen(PS_DOT, 1, theme.border);
    HGDIOBJ old_pen = SelectObject(hdc, pen), old_brush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    RoundRect(hdc, rc->left, rc->top, rc->right, rc->bottom, px(20), px(20));
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
    draw_text(hdc, text, &t, FONT_SUBHEADLINE, theme.accent, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    free(text);
}

static void layout_event(ConversationScreen *s, Doc *doc, int x, int w, const Event *e) {
    if (str_eq(e->kind, "user")) {
        int max_w = w * 3 / 4;
        int text_w = measure_text(doc->hdc, e->text, max_w - px(28), FONT_BODY, DT_WORDBREAK) < font_height(doc->hdc, FONT_BODY) * 2 ? text_width(doc->hdc, e->text, FONT_BODY) + px(28) : max_w;
        if (text_w > max_w) text_w = max_w;
        int bx = x + w - text_w;
        int box = doc_box_begin(doc, bx, text_w, px(10), theme.bubble, theme.bubble, px(16));
        doc_item(doc, box)->hover_fill = false;
        int ti = doc_text(doc, bx + px(14), text_w - px(28), e->text, FONT_BODY, theme.text, DT_WORDBREAK | DT_EDITCONTROL);
        doc_item(doc, ti)->action = ACT_MESSAGE_TEXT;
        doc_box_end(doc, box, px(10));
        size_t an = json_count(e->attachments);
        for (size_t i = 0; i < an; i++) {
            const char *name = json_str(json_get(json_at(e->attachments, i), "name"));
            doc_space(doc, px(6));
            int aw = text_width(doc->hdc, name ? name : "Attachment", FONT_CAPTION) + px(36);
            int ab = doc_box_begin(doc, x + w - aw, aw, px(5), theme.surface, theme.surface, px(12));
            doc_item(doc, ab)->hover_fill = false;
            doc_label(doc, x + w - aw + px(10), aw - px(20), 0xE723, name ? name : "Attachment", FONT_CAPTION, theme.secondary);
            doc_box_end(doc, ab, px(5));
        }
        add_time(doc, x, w, e, true);
    } else if (str_eq(e->kind, "text")) {
        int start = (int)doc->count;
        doc_markdown(doc, x, w, e->text ? e->text : "", FONT_BODY);
        for (size_t i = (size_t)start; i < doc->count; i++) if (!doc->items[i].action && doc->items[i].text) doc->items[i].action = ACT_MESSAGE_TEXT;
        add_time(doc, x, w, e, false);
    } else if (str_eq(e->kind, "ask")) {
        int box = doc_box_begin(doc, x, w, px(14), theme.elevated, blend(theme.accent, theme.background, 0.4), px(16));
        doc_item(doc, box)->hover_fill = false;
        int ix = x + px(14), iw = w - px(28);
        doc_label(doc, ix, iw, 0xE897, "Your input is needed", FONT_CAPTION_SEMIBOLD, theme.accent);
        doc_space(doc, px(10));
        doc_markdown(doc, ix, iw, e->question ? e->question : (e->text ? e->text : ""), FONT_BODY);
        if (can_message(s)) {
            size_t on = json_count(e->options);
            for (size_t i = 0; i < on; i++) {
                const char *label = json_str(json_get(json_at(e->options, i), "label"));
                if (!label) continue;
                doc_space(doc, px(8));
                int ob = doc_box_begin(doc, ix, iw, px(10), theme.surface, theme.border, px(12));
                doc_text(doc, ix + px(12), iw - px(44), label, FONT_BODY, theme.text, DT_WORDBREAK);
                doc_box_end(doc, ob, px(10));
                doc_box_action(doc, ob, ACT_ANSWER, (intptr_t)((e->seq << 8) | (int)i));
            }
            doc_space(doc, px(8));
            doc_text(doc, ix, iw, "Choose an answer to put it in the composer, or write your own.", FONT_CAPTION, theme.secondary, DT_WORDBREAK);
        }
        doc_box_end(doc, box, px(14));
    } else if (str_eq(e->kind, "result")) {
        FooterData *d = xcalloc(1, sizeof *d);
        d->failed = e->is_error == 1;
        d->glyph = d->failed ? 0xEA39 : 0xE930; d->color = d->failed ? theme.danger : theme.success;
        Str t; str_init(&t); str_appendz(&t, d->failed ? "Turn failed" : "Turn complete");
        if (e->has_duration) { char *dur = format_duration_ms(e->duration_ms); str_appendf(&t, " \xC2\xB7 %s", dur); free(dur); }
        time_t when; if (event_time(e, &when)) { char *tm = format_event_time(when); str_appendf(&t, " \xC2\xB7 %s", tm); free(tm); }
        d->text = str_detach(&t);
        doc_custom(doc, x, w, px(22), paint_turn_footer, d, footer_free, 0, 0);
    } else if (e->text) {
        if (str_eq(e->kind, "stderr") || str_eq(e->kind, "claude")) {
            int ti = doc_text(doc, x, w, e->text, FONT_MONO_SMALL, str_eq(e->kind, "stderr") ? theme.danger : theme.secondary, DT_WORDBREAK | DT_EDITCONTROL);
            doc_item(doc, ti)->action = ACT_MESSAGE_TEXT;
        } else {
            // Dashboard notices (review loops, interruptions).
            doc_label(doc, x, w, 0xE946, e->text, FONT_FOOTNOTE, theme.secondary);
        }
    }
}

static void layout_triage(ConversationScreen *s, Doc *doc, int x, int w, const Json *triage) {
    bool takes_verdicts = json_bool_tristate(json_get(triage, "mine")) != 0;
    int box = doc_box_begin(doc, x, w, px(14), theme.elevated, blend(theme.warning, theme.background, 0.4), px(16));
    doc_item(doc, box)->hover_fill = false;
    int ix = x + px(14), iw = w - px(28);
    Str title; str_init(&title);
    double round, pr;
    if (json_num(json_get(triage, "round"), &round)) str_appendf(&title, "Round %d findings", (int)round); else str_appendz(&title, "Review findings");
    if (json_num(json_get(triage, "prNumber"), &pr)) str_appendf(&title, " \xC2\xB7 PR #%d", (int)pr);
    doc_label(doc, ix, iw, 0xE7C1, title.data, FONT_SUBHEADLINE_SEMIBOLD, theme.warning);
    str_free(&title);
    const Json *findings = json_get(triage, "findings");
    size_t n = json_count(findings), fixes = 0;
    for (size_t i = 0; i < n; i++) {
        const Json *f = json_at(findings, i);
        doc_space(doc, px(10));
        int fb = doc_box_begin(doc, ix, iw, px(10), theme.surface, theme.surface, px(12));
        doc_item(doc, fb)->hover_fill = false;
        int fx = ix + px(10), fw = iw - px(20);
        const char *ft = json_str(json_get(f, "title"));
        doc_text(doc, fx, fw, ft ? ft : "Finding", FONT_SUBHEADLINE, theme.text, DT_WORDBREAK);
        Str meta; str_init(&meta);
        if (json_str(json_get(f, "severity"))) str_appendz(&meta, json_str(json_get(f, "severity")));
        if (json_str(json_get(f, "file"))) str_appendf(&meta, "%s%s", meta.len ? " \xC2\xB7 " : "", json_str(json_get(f, "file")));
        if (meta.len) { doc_space(doc, px(4)); doc_text(doc, fx, fw, meta.data, FONT_MONO_CAPTION2, theme.secondary, DT_WORDBREAK); }
        str_free(&meta);
        const char *why = json_str(json_get(f, "parkedWhy"));
        if (why) { doc_space(doc, px(4)); doc_text(doc, fx, fw, why, FONT_CAPTION, theme.secondary, DT_WORDBREAK); }
        const char *url = json_str(json_get(f, "url"));
        if (safe_web_url(url)) {
            doc_space(doc, px(4));
            int li = doc_text(doc, fx, fw, "Open on GitHub", FONT_CAPTION, theme.accent, DT_SINGLELINE);
            doc_item(doc, li)->action = ACT_FINDING_LINK; doc_item(doc, li)->arg = (intptr_t)i; doc_item(doc, li)->hand = true;
        }
        if (takes_verdicts && json_str(json_get(f, "key"))) {
            const char *decision = decision_for(s, triage, f);
            int selected = -1;
            for (int k = 0; k < 3; k++) if (str_eq(decision, triage_options[k])) selected = k;
            if (selected == 0) fixes++;
            doc_space(doc, px(8));
            doc_segments(doc, fx, fw, triage_titles, 3, selected, ACT_TRIAGE_DECISION, (intptr_t)(i * 4), !s->busy && !s->uncertain);
        }
        doc_box_end(doc, fb, px(10));
    }
    if (takes_verdicts) {
        doc_space(doc, px(10));
        int nb = doc_box_begin(doc, ix, iw, px(8), theme.surface, theme.border, px(10));
        doc_text(doc, ix + px(10), iw - px(20), str_empty(s->triage_note) ? "Note for the fix session (optional)" : s->triage_note, FONT_CALLOUT, str_empty(s->triage_note) ? theme.secondary : theme.text, DT_WORDBREAK);
        doc_box_end(doc, nb, px(8));
        doc_box_action(doc, nb, ACT_TRIAGE_NOTE, 0);
    }
    doc_space(doc, px(12));
    doc_button(doc, ix, iw, takes_verdicts ? "Complete triage" : "Clear findings", BUTTON_PROMINENT, ACT_TRIAGE_COMPLETE, (intptr_t)fixes, !s->busy && !s->uncertain);
    doc_space(doc, px(8));
    doc_text(doc, ix, iw, takes_verdicts ? "Verdicts are recorded on the pull request. Unmarked findings go as optional."
                                        : "This review is of somebody else\xE2\x80\x99s pull request; its author fixes the findings.", FONT_CAPTION, theme.secondary, DT_WORDBREAK);
    doc_box_end(doc, box, px(14));
}

static void complete_triage(ConversationScreen *s, size_t fixes) {
    const Json *triage = session_held_triage(session(s));
    if (!triage) return;
    bool takes_verdicts = json_bool_tristate(json_get(triage, "mine")) != 0;
    char *title;
    if (!takes_verdicts) title = xstrdup("Clear these findings?");
    else if (!fixes) title = xstrdup("Complete triage without fixes?");
    else title = xstrfmt("Start a paid fix session for %zu finding%s?", fixes, fixes == 1 ? "" : "s");
    s->dialog_open = true;
    bool ok = app_confirm(title, NULL, takes_verdicts ? "Complete" : "Clear", false);
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
    int w = doc->width, x = 0;
    doc_space(doc, px(12));
    if (s->error) { doc_notice_box(doc, x, w, s->error); doc_space(doc, px(12)); }
    if (s->write_error) {
        COLORREF fill = blend(theme.danger, theme.background, 0.08);
        int box = doc_box_begin(doc, x, w, px(12), fill, fill, px(12));
        doc_item(doc, box)->hover_fill = false;
        doc_notice(doc, x + px(12), w - px(24), s->write_error);
        doc_space(doc, px(6));
        doc_text(doc, x + px(12), w - px(24), "The action may have completed. Check the latest conversation before trying again.", FONT_CAPTION, theme.secondary, DT_WORDBREAK);
        doc_space(doc, px(8));
        doc_button(doc, x + px(12), 0, "Refresh and check outcome", BUTTON_BORDERED, ACT_REFRESH_OUTCOME, 0, !s->loading && !s->busy);
        doc_box_end(doc, box, px(12));
        doc_space(doc, px(12));
    }
    size_t visible = 0;
    for (size_t i = 0; i < s->transcript.count; i++) if (event_visible(&s->transcript.events[i])) visible++;
    if (!visible && !s->error) {
        doc_space(doc, px(60));
        doc_empty_state(doc, x, w, 0xE8BD, s->loaded ? "No messages yet" : "Waiting for the conversation\xE2\x80\xA6", NULL);
    }
    bool in_tools = false;
    for (size_t i = 0; i < s->transcript.count; i++) {
        const Event *e = &s->transcript.events[i];
        if (!event_visible(e)) continue;
        bool tool = str_eq(e->kind, "tool") || str_eq(e->kind, "tool_error") || str_eq(e->kind, "cmd") || str_eq(e->kind, "git");
        if (tool) {
            // Consecutive tool activity collapses into one tight cluster, like a terminal log.
            if (!in_tools) { doc_space(doc, px(8)); in_tools = true; }
            layout_tool(s, doc, x, w, e);
            doc_space(doc, px(2));
        } else {
            if (in_tools) { in_tools = false; doc_space(doc, px(8)); }
            doc_space(doc, px(10));
            layout_event(s, doc, x, w, e);
            doc_space(doc, px(8));
        }
    }
    const Json *queued = session_queued(session(s));
    for (size_t i = 0; i < json_count(queued); i++) {
        const char *text = json_str(json_get(json_at(queued, i), "text"));
        if (!text) text = "Message";
        doc_space(doc, px(10));
        int max_w = w * 3 / 4;
        int th = measure_text(doc->hdc, text, max_w - px(28), FONT_BODY, DT_WORDBREAK);
        int tw = th <= font_height(doc->hdc, FONT_BODY) + px(2) ? text_width(doc->hdc, text, FONT_BODY) + px(28) : max_w;
        if (tw > max_w) tw = max_w;
        RECT br = { x + w - tw, doc->y, x + w, doc->y + th + px(20) };
        doc_add(doc, &br, paint_queued_bubble);
        RECT tr = { br.left + px(14), br.top + px(10), br.right - px(14), br.bottom - px(10) };
        doc_text_at(doc, &tr, text, FONT_BODY, theme.secondary, DT_WORDBREAK | DT_EDITCONTROL);
        doc->y = br.bottom + px(4);
        bool removable = store_supports("drop_message") && !s->busy && !s->uncertain;
        const char *remove = "Remove";
        int rw = removable ? text_width(doc->hdc, remove, FONT_CAPTION_SEMIBOLD) + px(8) : 0;
        int qw = text_width(doc->hdc, "Queued", FONT_CAPTION2) + px(18);
        int lh = font_height(doc->hdc, FONT_CAPTION2) + px(4);
        RECT qr = { x + w - rw - qw, doc->y, x + w - rw, doc->y + lh };
        int qi = doc_text_at(doc, &qr, "Queued", FONT_CAPTION2, theme.secondary, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        (void)qi;
        if (removable) {
            RECT rr = { x + w - rw, doc->y, x + w, doc->y + lh };
            int ri = doc_text_at(doc, &rr, remove, FONT_CAPTION_SEMIBOLD, theme.danger, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            doc_item(doc, ri)->action = ACT_DROP_QUEUED; doc_item(doc, ri)->arg = (intptr_t)i; doc_item(doc, ri)->hand = true;
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
    doc_space(doc, px(12));
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
static void conversation_header(Screen *base, HeaderInfo *info) {
    ConversationScreen *s = (ConversationScreen *)base;
    const Session *ss = session(s);
    snprintf(info->title, sizeof info->title, "%s", session_display_title(ss));
    char *status = str_capitalized(session_status(ss));
    Str sub; str_init(&sub); str_appendz(&sub, status);
    if (session_model(ss)) str_appendf(&sub, " \xC2\xB7 %s", session_model(ss));
    if (session_review_loop_on(ss)) str_appendz(&sub, " \xC2\xB7 Review loop");
    snprintf(info->subtitle, sizeof info->subtitle, "%s", sub.data);
    snprintf(info->status, sizeof info->status, "%s", session_status(ss));
    str_free(&sub); free(status);
    // Every action the conversation takes, in the header as the dashboard has them.
    bool can = !s->busy && !s->uncertain;
    bool closed = str_eq(session_status(ss), "closed");
    int number = session_pull_number(ss);
    const char *repo = session_repo(ss);
    if (number && repo) {
        if (store_supports("pull_files")) header_button(info, 0xE8A5, "View changes", ACT_MENU_ITEM + MENU_CHANGES, true, false);
        if (store_supports("pull")) { char l[40]; snprintf(l, sizeof l, "Pull request #%d", number); header_button(info, 0xE8AB, l, ACT_MENU_ITEM + MENU_PULL, true, false); }
    }
    if (store_supports("review_loop") && session_can_review_loop(ss)) {
        bool on = session_review_loop_on(ss);
        header_button(info, 0xE895, on ? "Turn off review loop" : "Turn on review loop", ACT_MENU_ITEM + (on ? MENU_LOOP_OFF : MENU_LOOP_ON), can, false);
    }
    if (store_supports("rename")) header_button(info, 0xE70F, "Rename", ACT_MENU_ITEM + MENU_RENAME, can, false);
    if (store_supports("cancel") && session_is_active(ss)) header_button(info, 0xE71A, "Stop agent", ACT_MENU_ITEM + MENU_STOP, can, false);
    if (store_supports("close") && !closed) header_button(info, 0xE8BB, "Close conversation", ACT_MENU_ITEM + MENU_CLOSE, can, false);
    if (store_supports("reopen") && closed) header_button(info, 0xE7A7, "Reopen", ACT_MENU_ITEM + MENU_REOPEN, can, false);
    if (store_supports("delete")) header_button(info, 0xE74D, "Delete conversation", ACT_MENU_ITEM + MENU_DELETE, can, true);
}

static void menu_choice(ConversationScreen *s, int chosen) {
    const Session *ss = session(s);
    int number = session_pull_number(ss);
    const char *repo = session_repo(ss);
    if ((chosen == MENU_CHANGES || chosen == MENU_PULL) && !(number && repo)) return;
    switch (chosen) {
    case MENU_CHANGES: { Project p = { xstrdup(repo), NULL }; app_push_detail(pull_files_screen_new(&p, number)); project_free(&p); break; }
    case MENU_PULL: { Project p = { xstrdup(repo), NULL }; app_push_detail(pull_detail_screen_new(&p, number, NULL, NULL)); project_free(&p); break; }
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
static void conversation_context(Screen *base, int action, intptr_t arg, POINT pt) {
    ConversationScreen *s = (ConversationScreen *)base;
    (void)arg;
    if (action != ACT_MESSAGE_TEXT) return;
    Pane *pane = base->pane;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, MENU_COPY, L"Copy text");
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(pane), NULL);
    DestroyMenu(menu);
    if (chosen == MENU_COPY) {
        char *text = pane_hovered_text(pane);
        if (text) { copy_to_clipboard(pane_hwnd(pane), text); free(text); }
    }
    (void)s;
}

// MARK: - Footer (composer)

static int composer_height(ConversationScreen *s, HDC hdc) {
    int line_h = font_height(hdc, FONT_BODY);
    int lines = s->composer_lines < 1 ? 1 : s->composer_lines > 8 ? 8 : s->composer_lines;
    return lines * line_h + px(4);
}
static int conversation_footer_height(Screen *base, int width) {
    ConversationScreen *s = (ConversationScreen *)base;
    HDC hdc = GetDC(pane_hwnd(base->pane));
    int h;
    if (can_message(s)) h = px(12) + composer_height(s, hdc) + px(10) + px(32) + px(10) + px(14);
    else h = px(14) + font_height(hdc, FONT_SUBHEADLINE) + px(28) + px(14);
    ReleaseDC(pane_hwnd(base->pane), hdc);
    (void)width;
    return h;
}
static void conversation_footer_layout(Screen *base, const RECT *rc) {
    ConversationScreen *s = (ConversationScreen *)base;
    if (!can_message(s)) { ShowWindow(s->composer, SW_HIDE); return; }
    HDC hdc = GetDC(pane_hwnd(base->pane));
    int ch = composer_height(s, hdc);
    ReleaseDC(pane_hwnd(base->pane), hdc);
    RECT box = { rc->left + px(12), rc->top + px(6), rc->right - px(12), rc->bottom - px(8) };
    RECT er = { box.left + px(14), box.top + px(12), box.right - px(14), box.top + px(12) + ch };
    s->composer_rc = er;
    MoveWindow(s->composer, er.left, er.top, er.right - er.left, er.bottom - er.top, TRUE);
    ShowWindow(s->composer, SW_SHOW);
    if (!s->composer_focused_once) { s->composer_focused_once = true; }
}
static void conversation_footer_paint(Screen *base, HDC hdc, const RECT *rc) {
    ConversationScreen *s = (ConversationScreen *)base;
    fill_rect(hdc, rc, theme.background);
    const Session *ss = session(s);
    RECT box = { rc->left + px(12), rc->top + px(6), rc->right - px(12), rc->bottom - px(8) };
    memset(&s->mic_rc, 0, sizeof s->mic_rc); memset(&s->send_rc, 0, sizeof s->send_rc); memset(&s->discard_rc, 0, sizeof s->discard_rc);
    if (!can_message(s)) {
        fill_round_rect(hdc, &box, px(16), theme.surface, theme.surface);
        RECT g = { box.left + px(14), box.top, box.left + px(34), box.bottom };
        draw_glyph(hdc, store_can_manage() ? 0xE7B8 : 0xE7B3, &g, FONT_ICON, theme.secondary);
        bool reopen = store_supports("reopen") && str_eq(session_status(ss), "closed");
        int bw = reopen ? text_width(hdc, "Reopen", FONT_SUBHEADLINE_SEMIBOLD) + px(28) : 0;
        RECT t = { box.left + px(40), box.top, box.right - bw - px(20), box.bottom };
        draw_text(hdc, store_can_manage() ? "This conversation is closed" : "Read-only access", &t, FONT_SUBHEADLINE, theme.secondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (reopen) {
            int bh = font_height(hdc, FONT_SUBHEADLINE_SEMIBOLD) + px(12);
            RECT b = { box.right - px(14) - bw, (box.top + box.bottom) / 2 - bh / 2, box.right - px(14), (box.top + box.bottom) / 2 + bh / 2 };
            s->send_rc = b;
            bool enabled = !s->busy && !s->uncertain;
            fill_round_rect(hdc, &b, bh / 2, enabled ? theme.accent : blend(theme.accent, theme.surface, 0.5), enabled ? theme.accent : blend(theme.accent, theme.surface, 0.5));
            draw_text(hdc, "Reopen", &b, FONT_SUBHEADLINE_SEMIBOLD, theme.white, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        return;
    }
    fill_round_rect(hdc, &box, px(18), theme.elevated, theme.border);
    int row_y = s->composer_rc.bottom + px(8);
    int bs = px(32);
    int right = box.right - px(12);
    bool trimmed_empty = composer_empty(s);
    bool active = session_is_active(ss);
    VoiceState vs = s->voice ? voice_state(s->voice) : VOICE_IDLE;
    // Send or stop
    RECT send = { right - bs, row_y, right, row_y + bs };
    s->send_rc = send;
    if (active && store_supports("cancel") && trimmed_empty) {
        fill_round_rect(hdc, &send, bs / 2, theme.bubble, theme.bubble);
        draw_glyph(hdc, 0xE71A, &send, FONT_ICON_SMALL, theme.text);
    } else {
        bool enabled = !s->busy && !s->uncertain && !trimmed_empty;
        COLORREF fill = enabled || s->busy ? theme.accent : blend(theme.secondary, theme.elevated, 0.35);
        fill_round_rect(hdc, &send, bs / 2, fill, fill);
        draw_glyph(hdc, s->busy ? 0xE823 : 0xE74A, &send, FONT_ICON, theme.white);
    }
    right -= bs + px(10);
    if (store_can_transcribe()) {
        RECT mic = { right - bs, row_y, right, row_y + bs };
        s->mic_rc = mic;
        COLORREF fill = vs == VOICE_RECORDING ? theme.danger : theme.bubble;
        fill_round_rect(hdc, &mic, bs / 2, fill, fill);
        draw_glyph(hdc, vs == VOICE_RECORDING ? 0xE71A : (vs == VOICE_TRANSCRIBING || vs == VOICE_STARTING) ? 0xE823 : 0xE720, &mic, FONT_ICON_SMALL, vs == VOICE_RECORDING ? theme.white : theme.text);
        right -= bs + px(8);
        if (vs == VOICE_RECORDING || vs == VOICE_TRANSCRIBING) {
            RECT discard = { right - bs, row_y, right, row_y + bs };
            s->discard_rc = discard;
            draw_glyph(hdc, 0xE711, &discard, FONT_ICON_SMALL, theme.secondary);
            right -= bs + px(4);
            if (vs == VOICE_RECORDING) {
                char *clock = format_clock(voice_elapsed(s->voice));
                int cw = text_width(hdc, clock, FONT_CAPTION) + px(4);
                RECT cr = { right - cw, row_y, right, row_y + bs };
                draw_text(hdc, clock, &cr, FONT_CAPTION, theme.danger, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                free(clock);
                right -= cw + px(4);
            }
        }
    }
    if (active) {
        bool live = session_live_input(ss);
        RECT g = { box.left + px(14), row_y, box.left + px(30), row_y + bs };
        draw_glyph(hdc, live ? 0xE945 : 0xE823, &g, FONT_ICON_SMALL, theme.secondary);
        RECT t = { box.left + px(32), row_y, right - px(8), row_y + bs };
        draw_text(hdc, live ? "Sent into the running turn" : "Queued for the next turn", &t, FONT_CAPTION, theme.secondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}
static bool in_rect(const RECT *r, POINT pt) { return pt.x >= r->left && pt.x < r->right && pt.y >= r->top && pt.y < r->bottom; }
static void conversation_footer_click(Screen *base, POINT pt) {
    ConversationScreen *s = (ConversationScreen *)base;
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
static void conversation_visible(Screen *base, bool shown) {
    ConversationScreen *s = (ConversationScreen *)base;
    if (shown) {
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
    .context = conversation_context, .timer = conversation_timer, .footer_height = conversation_footer_height,
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
    SetWindowSubclass(s->composer, composer_proc, ID_COMPOSER, (DWORD_PTR)s);
    theme_apply_control(s->composer);
    if (store_can_transcribe()) s->voice = voice_new(voice_changed, s);
    return &s->base;
}
