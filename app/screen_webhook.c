// A session's ⚡ Webhook, as the dashboard's dialog: whether an outside system may post to wake the session, the caps
// the turns it starts run under, the second webhook that takes the operator's own instructions, and the URLs and keys a
// sender needs, with Rotate key to end the old ones. An admin token's, read and written through /sessions/{id}/webhook.
#include "screens.h"
#include "str.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { F_PER_HOUR, F_MAX_TURNS, F_COUNT };
typedef struct { const char *label, *hint; int min, max; } FieldDef;
static const FieldDef FIELDS[F_COUNT] = {
    [F_PER_HOUR] = { "Deliveries an hour", "Past it a sender is answered 429 with Retry-After. From 1 to 600.", 1, 600 },
    [F_MAX_TURNS] = { "Turns in a row", "Turns deliveries may start with no word from you; past it the webhook pauses until you say anything here (or save this form). From 1 to 1000.", 1, 1000 },
};
enum { T_ARMED, T_INSTRUCTIONS, T_SSH, T_COUNT };
static const char *const TOGGLES[T_COUNT] = {
    [T_ARMED] = "Take deliveries",
    [T_INSTRUCTIONS] = "Take instructions too: a second URL and key whose messages reach the agent as your word",
    [T_SSH] = "Let an SSH server in allow mode run commands unapproved in a turn a delivery started",
};
static const char *const TOGGLE_KEYS[T_COUNT] = { "armed", "instructions", "sshUnattended" };

enum { ACT_SAVE = 1200, ACT_ROTATE, ACT_TOGGLE, ACT_FOCUS, ACT_COPY, ACT_SHOW, ACT_RETRY };
enum { ID_FIELD = 2400 };
enum { TAG_READ = 1, TAG_WRITE = 2 };
// What ACT_COPY and ACT_SHOW are about.
enum { V_URL, V_KEY, V_INSTRUCTIONS_URL, V_INSTRUCTIONS_KEY };

typedef struct {
    Screen base;
    Session session;
    Json *hook;             // the server's last answer, a Webhook
    bool toggles[T_COUNT];
    bool shown_keys[4];
    bool loading, busy, dirty, filling, shown;
    char *error;            // the read failed
    char *message; bool message_ok;
    Request *req_read, *req_write;
    HWND edits[F_COUNT];
    RECT rects[F_COUNT];
    bool laid[F_COUNT], clipped[F_COUNT];
    int focused;
} WebhookForm;

static const char *hook_str(WebhookForm *s, const char *key) { return s->hook ? json_str_nonempty(json_get(s->hook, key)) : NULL; }
static bool hook_armed(WebhookForm *s) { return s->hook && json_bool_is(json_get(s->hook, "armed"), true); }

static char *edit_text(HWND edit) {
    int n = GetWindowTextLengthW(edit);
    wchar_t *w = xmalloc(((size_t)n + 1) * sizeof *w);
    GetWindowTextW(edit, w, n + 1);
    char *text = wide_to_utf8(w); free(w);
    return text;
}
static void set_edit_text(HWND edit, const char *text) { wchar_t *w = utf8_to_wide(text ? text : ""); SetWindowTextW(edit, w); free(w); }

/// The form shows what the server answered, dropping anything unsaved.
static void form_fill(WebhookForm *s) {
    if (!s->hook) return;
    for (int t = 0; t < T_COUNT; t++) s->toggles[t] = json_bool_is(json_get(s->hook, TOGGLE_KEYS[t]), true);
    if (s->edits[F_PER_HOUR]) {
        s->filling = true;
        char *a = xstrfmt("%d", json_int_or(json_get(s->hook, "perHour"), 30)), *b = xstrfmt("%d", json_int_or(json_get(s->hook, "maxTurns"), 10));
        set_edit_text(s->edits[F_PER_HOUR], a); set_edit_text(s->edits[F_MAX_TURNS], b);
        free(a); free(b);
        s->filling = false;
    }
    s->dirty = false;
}

// MARK: - Reading and writing

static void show_message(WebhookForm *s, const char *text, bool ok) {
    set_string(&s->message, text); s->message_ok = ok;
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}
static void read_done(void *owner, Request *req) {
    WebhookForm *s = owner;
    s->loading = false;
    if (!req->ok) request_error_into(&s->error, req);
    else {
        set_string(&s->error, NULL);
        json_free(s->hook); s->hook = json_clone(req->result);
        if (!s->dirty) form_fill(s);
    }
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}
static void load(WebhookForm *s) {
    if (s->loading) return;
    s->loading = true;
    Json *args = json_object();
    json_set_str(args, "sessionId", session_id(&s->session));
    store_call("session_webhook", args, 0, s, read_done, TAG_READ, &s->req_read);
    pane_relayout(s->base.pane);
}
static void write_done(void *owner, Request *req) {
    WebhookForm *s = owner;
    s->busy = false;
    if (!req->ok) { char *e = request_error_text(req); show_message(s, e, false); free(e); return; }
    json_free(s->hook); s->hook = json_clone(req->result);
    form_fill(s);
    bool rotated = str_eq(req->operation, "rotate_session_webhook");
    memset(s->shown_keys, 0, sizeof s->shown_keys);
    show_message(s, rotated ? "Keys rotated. A sender holding the old key is refused until it is given the new one." : "Saved.", true);
}
static void write(WebhookForm *s, const char *operation, Json *body) {
    s->busy = true; set_string(&s->message, NULL);
    Json *args = body ? body : json_object();
    json_set_str(args, "sessionId", session_id(&s->session));
    store_call(operation, args, 0, s, write_done, TAG_WRITE, &s->req_write);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}

static void form_save(WebhookForm *s) {
    if (s->busy || !s->hook || !s->edits[F_PER_HOUR]) return;
    Json *body = json_object();
    for (int t = 0; t < T_COUNT; t++) json_set_bool(body, TOGGLE_KEYS[t], s->toggles[t]);
    static const char *const keys[F_COUNT] = { "perHour", "maxTurns" };
    for (int f = 0; f < F_COUNT; f++) {
        char *text = edit_text(s->edits[f]), *trimmed = str_trim(text), *end = NULL;
        long n = strtol(trimmed, &end, 10);
        bool ok = *trimmed && end && !*end && n >= FIELDS[f].min && n <= FIELDS[f].max;
        free(text); free(trimmed);
        if (!ok) {
            char *m = xstrfmt("%s must be a whole number from %d to %d.", FIELDS[f].label, FIELDS[f].min, FIELDS[f].max);
            show_message(s, m, false); free(m); json_free(body);
            SetFocus(s->edits[f]);
            return;
        }
        json_set_num(body, keys[f], (double)n);
    }
    write(s, "set_session_webhook", body);
}

// MARK: - Layout

static void paint_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    WebhookForm *s = it->data;
    (void)doc;
    fill_round_rect(cv, rc, px(6), theme.raise, s->focused == (int)it->arg ? theme.accent_dim : theme.line);
}
static void field(WebhookForm *s, Doc *doc, int x, int w, int f) {
    doc_field_label(doc, x, w, FIELDS[f].label, theme.ink, FIELDS[f].hint);
    doc_space(doc, px(6));
    int fh = edit_line_height(FONT_BODY), h = px(36);
    RECT box = { x, doc->y, x + w, doc->y + h };
    Item *it = doc_item(doc, doc_add(doc, &box, paint_box));
    it->data = s; it->arg = f; it->action = ACT_FOCUS;
    s->rects[f] = (RECT){ x + px(10), box.top + (h - fh) / 2, x + w - px(10), box.top + (h - fh) / 2 + fh };
    s->laid[f] = true;
    doc->y = box.bottom;
    doc_space(doc, px(14));
}
typedef struct { WebhookForm *s; int t; bool enabled; } CheckData;
static void paint_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    CheckData *d = it->data;
    bool on = d->s->toggles[d->t], hovered = d->enabled && doc_item_hovered(doc, it);
    int size = px(15), top = rc->top + (rc->bottom - rc->top - size) / 2;
    RECT b = { rc->left, top, rc->left + size, top + size };
    fill_round_rect(cv, &b, px(3), on ? theme.accent : theme.field, on ? theme.accent : hovered ? theme.accent_dim : theme.line_strong);
    if (on) draw_glyph(cv, 0xE73E, &b, FONT_ICON_SMALL, theme.on_accent);
    RECT t = { b.right + px(8), rc->top, rc->right, rc->bottom };
    draw_text(cv, TOGGLES[d->t], &t, FONT_FOOTNOTE, d->enabled ? theme.ink : theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void check(WebhookForm *s, Doc *doc, int x, int w, int t, bool enabled) {
    CheckData *d = xcalloc(1, sizeof *d);
    d->s = s; d->t = t; d->enabled = enabled;
    Item *it = doc_item(doc, doc_custom(doc, x, w, px(26), paint_check, d, free, enabled ? ACT_TOGGLE : 0, t));
    it->hand = enabled;
    doc_space(doc, px(6));
}
static void note(Doc *doc, int x, int w, const char *text) {
    doc_text(doc, x, w, text, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
    doc_space(doc, px(10));
}
static void heading(Doc *doc, int x, int w, const char *text) {
    doc_space(doc, px(12));
    doc_text(doc, x, w, text, FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_LEFT | DT_SINGLELINE);
    doc_space(doc, px(8));
}
/// A URL or key on one line with Copy after it, and Show for a key, which is dotted until then.
static void value_row(WebhookForm *s, Doc *doc, int x, int w, const char *label, const char *help, const char *value, int which, bool secret) {
    doc_field_label(doc, x, w, label, theme.ink, help);
    doc_space(doc, px(6));
    bool hidden = secret && !s->shown_keys[which];
    const char *shown = hidden ? "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2" : value;
    int buttons = text_width(doc->cv, "Copy", FONT_CAPTION) + px(28) + (secret ? text_width(doc->cv, "Hide", FONT_CAPTION) + px(34) : 0);
    int top = doc->y, cw = text_width(doc->cv, shown, FONT_MONO);
    if (cw > w - buttons - px(14)) cw = w - buttons - px(14);
    if (cw < px(40)) cw = px(40);
    doc_text(doc, x, cw, shown, FONT_MONO, theme.ink, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    int bottom = doc->y, bx = x + cw + px(14);
    if (secret) {
        doc->y = top;
        int bi = doc_button(doc, bx, 0, hidden ? "Show" : "Hide", BUTTON_PLAIN, ACT_SHOW, which, true);
        bx = doc_item(doc, bi)->rc.right + px(6);
        if (doc->y > bottom) bottom = doc->y;
    }
    doc->y = top;
    doc_button(doc, bx, 0, "Copy", BUTTON_PLAIN, ACT_COPY, which, true);
    if (doc->y < bottom) doc->y = bottom;
    doc_space(doc, px(14));
}

static void form_layout(Screen *base, Doc *doc) {
    WebhookForm *s = (WebhookForm *)base;
    memset(s->laid, 0, sizeof s->laid);
    int w = doc->width, x = 0;
    doc_space(doc, px(16));
    if (s->message) {
        if (s->message_ok) doc_label(doc, x, w, 0xE73E, s->message, FONT_FOOTNOTE, theme.ok);
        else doc_notice_box(doc, x, w, s->message);
        doc_space(doc, px(14));
    }
    if (!s->hook) {
        if (s->error) {
            doc_notice_box(doc, x, w, s->error);
            doc_space(doc, px(10));
            doc_button(doc, x, 0, "Try again", BUTTON_BORDERED, ACT_RETRY, 0, !s->loading);
        } else doc_loading(doc, x, w, "Reading the webhook\xE2\x80\xA6");
        return;
    }
    note(doc, x, w, "An outside system (a support platform relaying what a customer wrote, an alert, a CI) posts to this URL to wake "
                    "the session with a message. What arrives is information, never your word: it answers no question, waits for the "
                    "turn under way to end, and everything held goes to the agent as one turn of its own. A closed session is woken for it.");
    const char *unfit = hook_str(s, "unfit"), *paused = hook_str(s, "paused");
    if (unfit && !hook_armed(s)) { doc_notice_box(doc, x, w, unfit); doc_space(doc, px(14)); }
    if (paused) {
        char *m = xstrfmt("Paused. %s Saving this form lifts the pause.", paused);
        doc_notice_box(doc, x, w, m); free(m);
        doc_space(doc, px(14));
    }
    int held = json_int_or(json_get(s->hook, "held"), 0);
    if (held) {
        char *m = xstrfmt(held == 1 ? "1 delivery is waiting for the turn under way, or for your answer." : "%d deliveries are waiting for the turn under way, or for your answer.", held);
        doc_label(doc, x, w, 0xE823, m, FONT_FOOTNOTE, theme.warn); free(m);
        doc_space(doc, px(12));
    }
    bool editable = !s->busy;
    check(s, doc, x, w, T_ARMED, editable && (!unfit || s->toggles[T_ARMED]));
    doc_space(doc, px(8));
    int gap = px(14), half = (w - gap) / 2, top = doc->y;
    field(s, doc, x, half, F_PER_HOUR);
    int bottom = doc->y;
    doc->y = top;
    field(s, doc, x + w - half, half, F_MAX_TURNS);
    if (doc->y < bottom) doc->y = bottom;
    check(s, doc, x, w, T_SSH, editable);
    check(s, doc, x, w, T_INSTRUCTIONS, editable && s->toggles[T_ARMED]);
    note(doc, x, w, "Instructions are for a bridge that sorts messages by who wrote them (a WhatsApp relay sending your own number's "
                    "messages there and everybody else's to the first URL): they answer the question the agent stands on and queue "
                    "behind a turn like a message typed here. Each URL has a key of its own; neither opens the other.");

    heading(doc, x, w, "Deliveries");
    const char *url = hook_str(s, "url"), *key = hook_str(s, "key");
    if (url) value_row(s, doc, x, w, "URL", "Where a sender posts. The /webhooks/ paths must bypass Cloudflare Access.", url, V_URL, false);
    if (key) value_row(s, doc, x, w, "Key", "Send it as Authorization: Bearer, or sign with it (X-Briareus-Signature-256 over the time sent and the body) so it never travels. Rotate key ends it.", key, V_KEY, true);
    else note(doc, x, w, s->toggles[T_ARMED] ? "Save to get the key a sender signs with." : "The key is handed out once the webhook is on.");
    if (hook_armed(s) && json_bool_is(json_get(s->hook, "instructions"), true)) {
        heading(doc, x, w, "Instructions");
        const char *iurl = hook_str(s, "instructionsUrl"), *ikey = hook_str(s, "instructionsKey");
        if (iurl) value_row(s, doc, x, w, "URL", "Where your own instructions are posted.", iurl, V_INSTRUCTIONS_URL, false);
        if (ikey) value_row(s, doc, x, w, "Key", "The instructions webhook's own key; the deliveries key does not open it.", ikey, V_INSTRUCTIONS_KEY, true);
    } else if (s->toggles[T_INSTRUCTIONS]) note(doc, x, w, "Save to get the instructions URL and key.");

    if (key && url) {
        heading(doc, x, w, "Sending");
        note(doc, x, w, "JSON {\"text\", \"source\", \"id\"}, plain text, or any other JSON, up to 20,000 characters. source is a label the "
                        "transcript shows; id names the delivery, so a retry is answered as a duplicate instead of running the agent twice.");
        char *md = xstrfmt("```sh\ncurl -X POST \"%s\" -H \"Authorization: Bearer $KEY\" -H 'Content-Type: application/json' \\\n"
                           "  -d '{\"text\":\"The nightly build failed\",\"source\":\"ci\",\"id\":\"run-4711\"}'\n```", url);
        doc_markdown(doc, x, w, md, FONT_FOOTNOTE);
        free(md);
    }
    doc_space(doc, px(40));
}

static void form_header(Screen *base, HeaderInfo *info) {
    WebhookForm *s = (WebhookForm *)base;
    snprintf(info->title, sizeof info->title, "\xE2\x9A\xA1 Webhook");
    const char *state = !s->hook ? (s->error ? "could not be read" : "reading\xE2\x80\xA6")
        : hook_str(s, "paused") ? "paused" : hook_armed(s) ? "on" : "off";
    snprintf(info->subtitle, sizeof info->subtitle, "%s \xC2\xB7 %s", session_display_title(&s->session), state);
    HeaderButton *b = &info->buttons[info->button_count++];
    snprintf(b->label, sizeof b->label, "Save");
    b->glyph = 0xE74E; b->action = ACT_SAVE; b->prominent = true; b->tip = "Save (Ctrl+S)";
    b->enabled = s->hook && !s->busy && (s->dirty || hook_str(s, "paused"));
    if (!hook_armed(s)) return;
    HeaderButton *r = &info->buttons[info->button_count++];
    snprintf(r->label, sizeof r->label, "Rotate key");
    r->glyph = 0xE72C; r->action = ACT_ROTATE; r->enabled = !s->busy; r->tip = "Replace the keys; the old ones stop working";
}

// MARK: - The edits

static int margin_of(Pane *pane) { RECT rc; GetClientRect(pane_hwnd(pane), &rc); return (rc.right - rc.left - pane_content_width(pane)) / 2; }
static void form_place(Screen *base, const RECT *content, int scroll_y) {
    WebhookForm *s = (WebhookForm *)base;
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
        EnableWindow(e, !s->busy);
        ShowWindow(e, SW_SHOWNA);
    }
}

static LRESULT CALLBACK field_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    WebhookForm *s = (WebhookForm *)ref;
    int f = (int)id;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_TAB) { SetFocus(s->edits[(f + 1) % F_COUNT]); return 0; }
        if (ctrl && wp == 'S') { form_save(s); return 0; }
        if (ctrl && wp == 'A') { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        if (wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); return 0; }
        if (wp == VK_RETURN) { form_save(s); return 0; }
        break;
    case WM_CHAR:
        if (wp == '\t' || wp == 0x13 || wp == 0x01 || wp == 0x1B || wp == '\r') return 0;
        break;
    case WM_MOUSEWHEEL: SendMessageW(GetParent(hwnd), msg, wp, lp); return 0;
    case WM_NCDESTROY: RemoveWindowSubclass(hwnd, field_proc, id); break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
static void form_ensure_controls(WebhookForm *s) {
    if (s->edits[0]) return;
    HWND owner = pane_hwnd(s->base.pane);
    for (int f = 0; f < F_COUNT; f++) {
        HWND e = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER, 0, 0, 10, 10, owner, (HMENU)(INT_PTR)(ID_FIELD + f), GetModuleHandleW(NULL), NULL);
        SendMessageW(e, WM_SETFONT, (WPARAM)font(FONT_BODY), TRUE);
        SetWindowSubclass(e, field_proc, (UINT_PTR)f, (DWORD_PTR)s);
        theme_apply_control(e);
        s->edits[f] = e;
    }
    if (!s->dirty) form_fill(s);
}

// MARK: - The screen

static void changed(WebhookForm *s) { if (s->filling) return; if (!s->dirty) { s->dirty = true; pane_header_changed(s->base.pane); } }

static void form_destroy(Screen *base) {
    WebhookForm *s = (WebhookForm *)base;
    request_cancel(&s->req_read); request_cancel(&s->req_write);
    for (int f = 0; f < F_COUNT; f++) if (s->edits[f]) DestroyWindow(s->edits[f]);
    json_free(s->hook);
    session_free(&s->session);
    free(s->error); free(s->message);
    screen_release(base);
}
static void form_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    WebhookForm *s = (WebhookForm *)base;
    switch (action) {
    case ACT_SAVE: form_save(s); break;
    case ACT_ROTATE:
        if (s->busy || !app_confirm("Rotate the webhook keys?", "New keys replace both of this session's webhook keys. A sender holding an old key is refused until it is given the new one.", "Rotate", true)) break;
        write(s, "rotate_session_webhook", NULL);
        break;
    case ACT_RETRY: set_string(&s->error, NULL); load(s); break;
    case ACT_TOGGLE:
        if (arg < 0 || arg >= T_COUNT || s->busy) break;
        s->toggles[arg] = !s->toggles[arg];
        // Instructions ride on the webhook: turning it off turns them off with it.
        if (arg == T_ARMED && !s->toggles[T_ARMED]) s->toggles[T_INSTRUCTIONS] = false;
        changed(s); pane_relayout(base->pane);
        break;
    case ACT_FOCUS: if (arg >= 0 && arg < F_COUNT && s->edits[arg]) SetFocus(s->edits[arg]); break;
    case ACT_SHOW: if (arg >= 0 && arg < 4) { s->shown_keys[arg] = !s->shown_keys[arg]; pane_relayout(base->pane); } break;
    case ACT_COPY: {
        static const char *const keys[4] = { "url", "key", "instructionsUrl", "instructionsKey" };
        const char *v = arg >= 0 && arg < 4 ? hook_str(s, keys[arg]) : NULL;
        if (v) { copy_to_clipboard(pane_hwnd(base->pane), v); show_message(s, arg == V_KEY || arg == V_INSTRUCTIONS_KEY ? "Key copied." : "URL copied.", true); }
        break;
    }
    }
}
static void form_command(Screen *base, int id, int code, HWND control) {
    (void)control;
    WebhookForm *s = (WebhookForm *)base;
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
    if (ctrl && vk == 'S') { form_save((WebhookForm *)base); return true; }
    return false;
}
static void form_visible(Screen *base, bool shown) {
    WebhookForm *s = (WebhookForm *)base;
    s->shown = shown;
    if (shown) {
        form_ensure_controls(s);
        // A pause or a held delivery may have come meanwhile; what is being typed is kept.
        load(s);
    } else for (int f = 0; f < F_COUNT; f++) if (s->edits[f]) ShowWindow(s->edits[f], SW_HIDE);
}
static void form_refresh(Screen *base) { load((WebhookForm *)base); }
static bool form_can_leave(Screen *base) {
    WebhookForm *s = (WebhookForm *)base;
    if (!s->dirty) return true;
    bool leave = app_confirm("Discard unsaved changes?", "The webhook's settings have not been saved.", "Discard", true);
    if (leave) s->dirty = false;
    return leave;
}

static const ScreenVTable form_vt = {
    .destroy = form_destroy, .layout = form_layout, .header = form_header, .action = form_action, .place = form_place,
    .visible = form_visible, .command = form_command, .key = form_key, .can_leave = form_can_leave, .refresh = form_refresh,
};
Screen *webhook_screen_new(const Session *session) {
    WebhookForm *s = xcalloc(1, sizeof *s);
    s->base.vt = &form_vt;
    s->base.id = xstrfmt("webhook:%s", session_id(session));
    s->focused = -1;
    session_copy(&s->session, session);
    return &s->base;
}
