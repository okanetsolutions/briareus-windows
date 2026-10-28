#include "screen_conversation_internal.h"

static char *cache_key(ConversationScreen *s);
static void refresh_done(void *owner, Request *req);
static void mutate_done(void *owner, Request *req);
static void conversation_timer(Screen *base, UINT id);
static void show_panel(ConversationScreen *s);
static void conversation_visible(Screen *base, bool shown);
static void conversation_window_visible(Screen *base, bool shown);
static void conversation_refresh(Screen *base);
static void conversation_scrolled(Screen *base, bool at_bottom);
static void conversation_activated(Screen *base, bool active);
static bool conversation_key(Screen *base, WPARAM vk, bool ctrl, bool shift);
static void conversation_destroy(Screen *base);

const Session *screen_conversation_session(ConversationScreen *s) { return s->has_snapshot ? &s->snapshot : &s->initial; }

static char *cache_key(ConversationScreen *s) { return xstrfmt("transcript:%s", session_id(&s->initial)); }

bool screen_conversation_can_message(ConversationScreen *s) { return store_supports("message") && !str_eq(session_status(screen_conversation_session(s)), "closed"); }

// MARK: - Reading

static void refresh_done(void *owner, Request *req) {
    ConversationScreen *s = owner;
    s->loading = false;
    bool full = req->arg != 0;
    bool read = false;
    if (!req->ok) {
        request_error_into(&s->error, req);
        s->loaded = true;
        poller_finished(&s->poller, true, req->error.retry_after);
    } else {
        const Json *events = json_get(req->result, "events");
        Session next;
        if (session_parse(json_get(req->result, "session"), &next)) {
            if (s->has_snapshot) session_free(&s->snapshot);
            s->snapshot = next; s->has_snapshot = true; read = true;
        }
        if (full) { transcript_free(&s->transcript); transcript_init(&s->transcript); }
        transcript_append(&s->transcript, events);
        set_string(&s->error, NULL); s->loaded = true;
        char *key = cache_key(s);
        if (full || s->unsaved) { Json *all = transcript_json(&s->transcript); s->unsaved = !cache_replace(g_store.cache, all, key); json_free(all); }
        else if (json_count(events)) s->unsaved = !cache_append(g_store.cache, events, key);
        free(key);
        poller_set_base(&s->poller, session_is_active(screen_conversation_session(s)) ? 2000 : 7000);
        poller_finished(&s->poller, false, -1);
        if (pane_top(s->base.pane) == &s->base) show_panel(s);
        if (session_is_active(screen_conversation_session(s))) SetTimer(pane_hwnd(s->base.pane), TIMER_WORKING, 120, NULL); else KillTimer(pane_hwnd(s->base.pane), TIMER_WORKING);
    }
    pane_relayout(s->base.pane);
    pane_header_changed(s->base.pane);
    // A read asked for while this one was out goes now; after a failed one it waits for the poller's retry and its backoff.
    if (req->ok && (s->pending_full || s->pending_refresh)) { bool f = s->pending_full; s->pending_full = s->pending_refresh = false; screen_conversation_refresh(s, f); }
    // The session a mutation changed is current only once a read of it lands.
    else if (read) s->reloading = false;
}

void screen_conversation_refresh(ConversationScreen *s, bool full) {
    // A poll already reading must not swallow a refresh, which waits its turn.
    if (s->loading) { if (full) s->pending_full = true; else s->pending_refresh = true; return; }
    // A read still queued (after a failed one, or one the hidden screen cancelled) is this one, not a second after it.
    full = full || s->pending_full; s->pending_full = s->pending_refresh = false;
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
        request_error_into(&s->write_error, req);
        // A refusal (draining, closed elsewhere) changed nothing, so the screen stays usable; it is read again, since a
        // refusal often means what it shows is out of date.
        if (request_outcome_unknown(req)) s->uncertain = true;
        else { s->reloading = true; screen_conversation_refresh(s, false); }
        pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
        return;
    }
    if (str_eq(name, "message")) {
        char *sent = screen_conversation_composer_text(s);
        if (str_eq(sent, json_str(json_get(req->args, "text")))) { screen_conversation_set_composer_text(s, ""); pane_stick_to_bottom(s->base.pane, true); pane_scroll_to_bottom(s->base.pane); }
        free(sent);
        // The files that went with it; one attached since stays for the next message.
        attach_list_sent(s->files, json_get(req->args, "attachments"));
    }
    if (str_eq(name, "delete")) {
        // The sidebar drops the row now instead of at its next poll; the transcript goes with it.
        sessions_forget(session_repo(&s->initial), session_id(&s->initial));
        // Beside the list there is nothing to go back to: the right-hand side empties instead.
        Pane *pane = s->base.pane;
        if (pane_depth(pane) > 1) pane_pop(pane); else if (detached_pane(pane)) detached_close(pane); else app_clear_detail();
        return;   // this screen is gone
    }
    if (str_eq(name, "complete_findings")) { str_array_free(s->decisions, s->decision_count); s->decisions = NULL; s->decision_count = 0; set_string(&s->triage_note, NULL); }
    pane_header_changed(s->base.pane);
    // Until the session is read again the loop chip would act on the old one, and a second click resend the same `on`.
    s->reloading = true;
    // A Clear or a compaction hides lines already on screen, so the transcript is read again whole.
    screen_conversation_refresh(s, str_eq(name, "clear") || str_eq(name, "compact"));
}

void screen_conversation_mutate(ConversationScreen *s, const char *name, Json *extra) {
    if (s->busy || s->uncertain) { json_free(extra); return; }
    s->busy = true; set_string(&s->write_error, NULL);
    Json *args = json_object();
    json_set_str(args, "sessionId", session_id(&s->initial));
    if (extra) { json_object_merge(args, extra); json_free(extra); }
    store_call(name, args, 0, s, mutate_done, TAG_MUTATE, &s->req_mutate);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}

void screen_conversation_confirm_and_mutate(ConversationScreen *s, const char *action) {
    const char *title;
    if (str_eq(action, "delete")) title = "Permanently delete this conversation and its transcript?";
    else if (str_eq(action, "cancel")) title = "Stop the running agent?";
    else if (str_eq(action, "close")) title = "Close this conversation?";
    else title = "Reopen this conversation?";
    s->dialog_open = true;
    bool ok = app_confirm(title, NULL, "Confirm", str_eq(action, "delete") || str_eq(action, "cancel"));
    s->dialog_open = false;
    if (!ok) return;
    screen_conversation_mutate(s, action, NULL);
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
        if (enabled && !s->loading) screen_conversation_refresh(s, false); else poller_finished(&s->poller, false, -1);
    }
}

/// The column at the right: the session's pull request and context usage, while this conversation is the main window's
/// page itself (not one pushed over a pull request, nor one in a window of its own) and the session has any of them. An
/// open panel takes the latest record.
static void show_panel(ConversationScreen *s) {
    if (s->base.pane != app_detail_pane()) return;
    if (pane_root(s->base.pane) == &s->base && session_panel_wanted(screen_conversation_session(s))) {
        app_set_panel(session_panel_screen_new(screen_conversation_session(s)));
        session_panel_update(screen_conversation_session(s));
    } else app_set_panel(NULL);
}

void conversation_session_op(const char *session_id_, const char *operation, Json *extra) {
    Screen *top = pane_top(app_detail_pane());
    char *id = xstrfmt("conversation:%s", session_id_);
    bool ours = top && top->id && str_eq(top->id, id);
    free(id);
    if (ours) screen_conversation_mutate((ConversationScreen *)top, operation, extra); else json_free(extra);
}

static void create_composer(ConversationScreen *s, HWND parent) {
    s->composer = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)ID_COMPOSER, GetModuleHandleW(NULL), NULL);
    SendMessageW(s->composer, WM_SETFONT, (WPARAM)font(FONT_BODY), TRUE);
    SendMessageW(s->composer, EM_SETCUEBANNER, TRUE, (LPARAM)L"Reply\x2026");
    SetWindowSubclass(s->composer, screen_conversation_composer_proc, ID_COMPOSER, (DWORD_PTR)s);
    theme_apply_control(s->composer);
    DragAcceptFiles(s->composer, TRUE);
}

static void conversation_visible(Screen *base, bool shown) {
    ConversationScreen *s = (ConversationScreen *)base;
    if (shown) {
        if (!s->composer) create_composer(s, pane_hwnd(base->pane));
        // The composer goes with the conversation into a window of its own, and back.
        if (GetParent(s->composer) != pane_hwnd(base->pane)) SetParent(s->composer, pane_hwnd(base->pane));
        show_panel(s);
        pane_stick_to_bottom(base->pane, true); pane_show_bottom_button(base->pane, true);
        poller_start(&s->poller, base->pane, TIMER_POLL, session_is_active(screen_conversation_session(s)) ? 2000 : 7000);
        if (session_is_active(screen_conversation_session(s))) SetTimer(pane_hwnd(base->pane), TIMER_WORKING, 120, NULL);
        pane_scroll_to_bottom(base->pane);
    } else {
        poller_stop(&s->poller); KillTimer(pane_hwnd(base->pane), TIMER_WORKING); KillTimer(pane_hwnd(base->pane), TIMER_VOICE);
        request_cancel(&s->req_refresh); s->loading = false;
        if (s->voice) voice_drop(s->voice);
        ShowWindow(s->composer, SW_HIDE);
        // A pull request pushed over the conversation fills the window; the column returns with the conversation.
        if (base->pane == app_detail_pane()) app_set_panel(NULL);
    }
}

static void conversation_window_visible(Screen *base, bool shown) {
    prompt_picker_visible(((ConversationScreen *)base)->prompts, shown);
}

static void conversation_refresh(Screen *base) { ConversationScreen *s = (ConversationScreen *)base; screen_conversation_refresh(s, true); }

static void conversation_scrolled(Screen *base, bool at_bottom) { ConversationScreen *s = (ConversationScreen *)base; s->at_bottom = at_bottom; pane_stick_to_bottom(base->pane, at_bottom); }

static void conversation_activated(Screen *base, bool active) {
    ConversationScreen *s = (ConversationScreen *)base;
    // Recording cannot go on in the background: what was said until then is transcribed, as if stopped.
    if (!active && s->voice && voice_state(s->voice) == VOICE_RECORDING) voice_stop(s->voice);
    if (active) poller_start(&s->poller, base->pane, TIMER_POLL, session_is_active(screen_conversation_session(s)) ? 2000 : 7000);
}

static bool conversation_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    ConversationScreen *s = (ConversationScreen *)base;
    (void)shift;
    if (vk == VK_F5) { screen_conversation_refresh(s, true); return true; }
    if (ctrl && vk == 'R') { screen_conversation_refresh(s, true); return true; }
    return false;
}

static void conversation_destroy(Screen *base) {
    ConversationScreen *s = (ConversationScreen *)base;
    request_cancel(&s->req_refresh); request_cancel(&s->req_mutate);
    poller_stop(&s->poller);
    if (base->pane) { KillTimer(pane_hwnd(base->pane), TIMER_WORKING); KillTimer(pane_hwnd(base->pane), TIMER_VOICE); }
    if (s->voice) voice_free(s->voice);
    prompt_picker_free(s->prompts);
    attach_list_free(s->files);
    if (s->composer) DestroyWindow(s->composer);
    session_free(&s->initial); if (s->has_snapshot) session_free(&s->snapshot);
    transcript_free(&s->transcript);
    free(s->error); free(s->write_error); free(s->pending_mutation); free(s->expanded); free(s->triage_note);
    str_array_free(s->decisions, s->decision_count);
    screen_release(base);
}

static const ScreenVTable conversation_vt = {
    .destroy = conversation_destroy, .layout = screen_conversation_conversation_layout, .header = screen_conversation_conversation_header, .action = screen_conversation_conversation_action,
    .timer = conversation_timer, .footer_height = screen_conversation_conversation_footer_height,
    .footer_layout = screen_conversation_conversation_footer_layout, .footer_paint = screen_conversation_conversation_footer_paint, .footer_click = screen_conversation_conversation_footer_click,
    .visible = conversation_visible, .command = screen_conversation_conversation_command, .key = conversation_key, .refresh = conversation_refresh,
    .scrolled = conversation_scrolled, .activated = conversation_activated, .window_visible = conversation_window_visible, .detachable = true, .searchable = true,
};

Screen *conversation_screen_new(const Session *initial) {
    ConversationScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &conversation_vt; s->base.id = xstrfmt("conversation:%s", session_id(initial));
    session_copy(&s->initial, initial);
    transcript_init(&s->transcript);
    s->composer_lines = 1; s->at_bottom = true;
    s->files = attach_list_new(&s->base, "message");
    screen_conversation_prompts_init(s);
    if (store_can_transcribe()) s->voice = voice_new(screen_conversation_voice_changed, s);
    return &s->base;
}
