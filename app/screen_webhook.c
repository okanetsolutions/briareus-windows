#include "screen_webhook_internal.h"

static void show_message(WebhookForm *s, const char *text, bool ok);
static void read_done(void *owner, Request *req);
static void load(WebhookForm *s);
static void write_done(void *owner, Request *req);
static void submit(WebhookForm *s, const char *operation, Json *body);
static void changed(WebhookForm *s);
static void form_destroy(Screen *base);
static void form_action(Screen *base, int action, intptr_t arg, POINT pt);
static void form_command(Screen *base, int id, int code, HWND control);
static bool form_key(Screen *base, WPARAM vk, bool ctrl, bool shift);
static void form_visible(Screen *base, bool shown);
static void form_refresh(Screen *base);
static bool form_can_leave(Screen *base);

const char *const screen_webhook_TOGGLE_KEYS[T_COUNT] = { "armed", "instructions", "sshUnattended" };

const char *screen_webhook_hook_str(WebhookForm *s, const char *key) { return s->hook ? json_str_nonempty(json_get(s->hook, key)) : NULL; }

bool screen_webhook_hook_armed(WebhookForm *s) { return s->hook && json_bool_is(json_get(s->hook, "armed"), true); }

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
        if (!s->dirty) screen_webhook_form_fill(s);
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
    screen_webhook_form_fill(s);
    bool rotated = str_eq(req->operation, "rotate_session_webhook");
    memset(s->shown_keys, 0, sizeof s->shown_keys);
    show_message(s, rotated ? "Keys rotated. A sender holding the old key is refused until it is given the new one." : "Saved.", true);
}

static void submit(WebhookForm *s, const char *operation, Json *body) {
    s->busy = true; set_string(&s->message, NULL);
    Json *args = body ? body : json_object();
    json_set_str(args, "sessionId", session_id(&s->session));
    store_call(operation, args, 0, s, write_done, TAG_WRITE, &s->req_write);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}

void screen_webhook_form_save(WebhookForm *s) {
    if (s->busy || !s->hook || !s->edits[F_PER_HOUR]) return;
    char *texts[F_COUNT];
    const char *caps[F_COUNT];
    for (int f = 0; f < F_COUNT; f++) caps[f] = texts[f] = screen_webhook_edit_text(s->edits[f]);
    WebhookField invalid;
    Json *body = webhook_save(s->toggles[T_ARMED], s->toggles[T_INSTRUCTIONS], s->toggles[T_SSH], caps, &invalid);
    for (int f = 0; f < F_COUNT; f++) free(texts[f]);
    if (!body) {
        const WebhookFieldDef *def = &webhook_fields[invalid];
        char *m = xstrfmt("%s must be a whole number from %d to %d.", def->label, def->min, def->max);
        show_message(s, m, false); free(m);
        SetFocus(s->edits[invalid]);
        return;
    }
    submit(s, "set_session_webhook", body);
}

// MARK: - The screen

static void changed(WebhookForm *s) { if (s->filling) return; if (!s->dirty) { s->dirty = true; pane_header_changed(s->base.pane); } }

static void form_release(WebhookForm *s) {
    for (int f = 0; f < F_COUNT; f++) if (s->edits[f]) DestroyWindow(s->edits[f]);
    json_free(s->hook);
    session_free(&s->session);
    free(s->error); free(s->message);
    screen_release(&s->base);
}

static void form_destroy(Screen *base) {
    WebhookForm *s = (WebhookForm *)base;
    request_cancel(&s->req_read); request_cancel(&s->req_write);
    if (s->modal) { s->retired = true; return; }
    form_release(s);
}

static void form_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    WebhookForm *s = (WebhookForm *)base;
    switch (action) {
    case ACT_SAVE: screen_webhook_form_save(s); break;
    case ACT_ROTATE:
        if (s->busy || !app_confirm("Rotate the webhook keys?", "New keys replace both of this session's webhook keys. A sender holding an old key is refused until it is given the new one.", "Rotate", true)) break;
        submit(s, "rotate_session_webhook", NULL);
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
        const char *v = arg >= 0 && arg < 4 ? screen_webhook_hook_str(s, keys[arg]) : NULL;
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
    if (ctrl && vk == 'S') { screen_webhook_form_save((WebhookForm *)base); return true; }
    return false;
}

static void form_visible(Screen *base, bool shown) {
    WebhookForm *s = (WebhookForm *)base;
    s->shown = shown;
    if (shown) {
        screen_webhook_form_ensure_controls(s);
        // A pause or a held delivery may have come meanwhile; what is being typed is kept.
        load(s);
    } else for (int f = 0; f < F_COUNT; f++) if (s->edits[f]) ShowWindow(s->edits[f], SW_HIDE);
}

static void form_refresh(Screen *base) { load((WebhookForm *)base); }

static bool form_can_leave(Screen *base) {
    WebhookForm *s = (WebhookForm *)base;
    if (!s->dirty) return true;
    s->modal = true;
    bool leave = app_confirm("Discard unsaved changes?", "The webhook's settings have not been saved.", "Discard", true);
    s->modal = false;
    if (s->retired) { form_release(s); return true; }
    if (leave) s->dirty = false;
    return leave;
}

static const ScreenVTable form_vt = {
    .destroy = form_destroy, .layout = screen_webhook_form_layout, .header = screen_webhook_form_header, .action = form_action, .place = screen_webhook_form_place,
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
