#include "screen_browser_internal.h"

static void start_feed(BrowserScreen *s);
static void came_up(BrowserScreen *s);
static void state_done(void *owner, Request *req);
static void toggle_done(void *owner, Request *req);
static void switch_browser(BrowserScreen *s, bool on);
static void browser_header(Screen *base, HeaderInfo *info);
static void more_menu(BrowserScreen *s, POINT pt);
static void browser_action(Screen *base, int action, intptr_t arg, POINT pt);
static void browser_context(Screen *base, int action, intptr_t arg, POINT pt);
static void browser_timer(Screen *base, UINT id);
static void browser_visible(Screen *base, bool shown);
static void browser_refresh(Screen *base);
static bool browser_key(Screen *base, WPARAM vk, bool ctrl, bool shift);
static void browser_destroy(Screen *base);

void screen_browser_changed(BrowserScreen *s) { if (s->base.pane) { pane_relayout(s->base.pane); pane_header_changed(s->base.pane); } }

bool screen_browser_can_drive(void) { return store_can_manage() && store_supports("browser_input"); }

void screen_browser_show_frame(BrowserScreen *s, unsigned char *pixels, int pw, int ph, int fw, int fh) {
    free(s->pixels);
    s->pixels = pixels; s->pw = pw; s->ph = ph; s->fw = fw; s->fh = fh;
    if (s->view) InvalidateRect(s->view, NULL, FALSE);
}

void screen_browser_stop_feed(BrowserScreen *s) {
    screen_browser_feed_stop(s->feed); s->feed = NULL;
    if (s->base.pane) KillTimer(pane_hwnd(s->base.pane), TIMER_RECONNECT);
}

static void start_feed(BrowserScreen *s) {
    if (s->feed || !s->shown || !s->state.running || !s->view || !store_supports("browser_stream")) return;
    s->feed = screen_browser_feed_start(s->session_id, s->view);
}

void screen_browser_set_address(BrowserScreen *s) {
    if (!s->address || GetFocus() == s->address) return;
    const BrowserTab *tab = browser_active_tab(&s->state);
    wchar_t *w = utf8_to_wide(tab && !str_eq(tab->url, "about:blank") ? tab->url : "");
    SetWindowTextW(s->address, w);
    free(w);
}

/// The browser went down: the stream stops and the picture goes; the state is polled until it comes back.
void screen_browser_went_down(BrowserScreen *s) {
    s->state.running = false;
    screen_browser_stop_feed(s);
    screen_browser_show_frame(s, NULL, 0, 0, 0, 0);
    browser_inputs_free(&s->inputs);
    if (s->shown && !s->poller.running) poller_start(&s->poller, s->base.pane, TIMER_POLL, 5000);
}

static void came_up(BrowserScreen *s) {
    poller_stop(&s->poller);
    s->failures = 0;
    start_feed(s);
    screen_browser_set_address(s);
}

// MARK: - Reading and switching

static void state_done(void *owner, Request *req) {
    BrowserScreen *s = owner;
    bool failed = !req->ok;
    s->state_read = true;
    if (req->ok) {
        s->closed_session = false;
        set_string(&s->state_error, NULL);
        browser_state_read(&s->state, json_get(req->result, "browser"));
        if (s->state.running) came_up(s); else screen_browser_went_down(s);
    } else {
        if (api_error_unauthorized(&req->error)) store_invalidate_credentials(&req->error);
        request_error_into(&s->state_error, req);
        // A read that failed is tried again, sooner or later as the server says.
        if (s->shown && !s->poller.running) poller_start(&s->poller, s->base.pane, TIMER_POLL, 5000);
    }
    poller_finished(&s->poller, failed, req->error.retry_after);
    screen_browser_changed(s);
}

void screen_browser_read_state(BrowserScreen *s) {
    if (s->req_state || !store_supports("browser")) return;
    Json *a = json_object(); json_set_str(a, "sessionId", s->session_id);
    store_call("browser", a, 0, s, state_done, 0, &s->req_state);
}

static void toggle_done(void *owner, Request *req) {
    BrowserScreen *s = owner;
    bool on = str_eq(req->operation, "browser_on");
    s->starting = s->stopping = false;
    if (!req->ok) {
        request_error_into(&s->error, req);
        // 409: the conversation is closed, and its browser with it until it is reopened.
        if (on && req->error.status == 409) s->closed_session = true;
    } else if (on) {
        set_string(&s->error, NULL);
        browser_state_read(&s->state, json_get(req->result, "browser"));
        s->state.on = true;
        if (s->state.running) came_up(s); else screen_browser_went_down(s);
    } else {
        set_string(&s->error, NULL);
        browser_state_free(&s->state);
        s->state.on = false;
        screen_browser_went_down(s);
    }
    screen_browser_changed(s);
}

static void switch_browser(BrowserScreen *s, bool on) {
    if (s->req_toggle || !store_supports(on ? "browser_on" : "browser_off")) return;
    if (!on && !app_confirm("Switch the browser off?",
                            "The agent loses its browser tools until it is switched on again. Its tabs close; cookies and logins are kept until the conversation is deleted.",
                            "Switch off", false)) return;
    if (on) s->starting = true; else s->stopping = true;
    set_string(&s->error, NULL);
    Json *a = json_object(); json_set_str(a, "sessionId", s->session_id);
    store_call(on ? "browser_on" : "browser_off", a, 0, s, toggle_done, 0, &s->req_toggle);
    screen_browser_changed(s);
}

/// No pane header: the top bar is the screen's own, as compact as a browser's.
static void browser_header(Screen *base, HeaderInfo *info) { (void)base; (void)info; }

/// ⋯: the tabs the strip has no room for, the tab in view's reload and close, switching the browser on or off, and where
/// it is shown.
static void more_menu(BrowserScreen *s, POINT pt) {
    HMENU menu = CreatePopupMenu();
    bool drive = screen_browser_can_drive() && s->state.running;
    const BrowserTab *tab = browser_active_tab(&s->state);
    if (s->state.running && s->state.count > 1) {
        for (size_t i = 0; i < s->state.count && i < 50; i++) {
            wchar_t *w = utf8_to_wide(screen_browser_tab_label(&s->state.tabs[i]));
            AppendMenuW(menu, MF_STRING | (str_eq(s->state.tabs[i].id, s->state.active) ? MF_CHECKED : 0) | (drive ? 0 : MF_GRAYED), MORE_TAB + i, w);
            free(w);
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(menu, MF_STRING | (drive ? 0 : MF_GRAYED), MORE_RELOAD, L"Reload\tF5");
    AppendMenuW(menu, MF_STRING | (drive && tab ? 0 : MF_GRAYED), MORE_CLOSE_TAB, L"Close this tab");
    if (store_can_manage() && s->state.on && store_supports("browser_off")) {
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING | (s->stopping || s->starting ? MF_GRAYED : 0), MORE_STOP, L"Switch the browser off\x2026");
    } else if (store_can_manage() && store_supports("browser_on")) {
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING | (s->stopping || s->starting ? MF_GRAYED : 0), MORE_START, L"Start the browser");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    if (s->detached) AppendMenuW(menu, MF_STRING | (screen_browser_can_dock(s) ? 0 : MF_GRAYED), MORE_DOCK, L"Dock beside the conversation");
    else AppendMenuW(menu, MF_STRING, MORE_DETACH, L"Open in a separate window");
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN, pt.x, pt.y, 0, pane_hwnd(s->base.pane), NULL);
    DestroyMenu(menu);
    if (chosen >= MORE_TAB && (size_t)(chosen - MORE_TAB) < s->state.count) {
        Json *j = screen_browser_simple_input("tab"); json_set_str(j, "tab", s->state.tabs[chosen - MORE_TAB].id);
        screen_browser_send_input(s, j);
        return;
    }
    switch (chosen) {
    case MORE_RELOAD: screen_browser_send_input(s, screen_browser_simple_input("reload")); break;
    case MORE_CLOSE_TAB: if ((tab = browser_active_tab(&s->state))) { Json *j = screen_browser_simple_input("closeTab"); json_set_str(j, "tab", tab->id); screen_browser_send_input(s, j); } break;
    case MORE_START: switch_browser(s, true); break;
    case MORE_STOP: switch_browser(s, false); break;
    case MORE_DETACH: screen_browser_post_move(s, ACT_DETACH); break;
    case MORE_DOCK: screen_browser_post_move(s, ACT_DOCK); break;
    }
}

static void browser_action(Screen *base, int action, intptr_t arg, POINT pt) {
    BrowserScreen *s = (BrowserScreen *)base;
    switch (action) {
    case ACT_START: switch_browser(s, true); break;
    case ACT_STOP: switch_browser(s, false); break;
    case ACT_BACK: screen_browser_send_input(s, screen_browser_simple_input("back")); break;
    case ACT_FORWARD: screen_browser_send_input(s, screen_browser_simple_input("forward")); break;
    case ACT_RELOAD: screen_browser_send_input(s, screen_browser_simple_input("reload")); break;
    case ACT_NEW_TAB: screen_browser_send_input(s, screen_browser_simple_input("newTab")); SetFocus(s->address); break;
    case ACT_CLOSE_TAB: {
        const BrowserTab *tab = browser_active_tab(&s->state);
        if (!tab) break;
        Json *j = screen_browser_simple_input("closeTab"); json_set_str(j, "tab", tab->id);
        screen_browser_send_input(s, j);
        break;
    }
    case ACT_TAB:
        if (arg >= 0 && (size_t)arg < s->state.count && !str_eq(s->state.tabs[arg].id, s->state.active)) {
            Json *j = screen_browser_simple_input("tab"); json_set_str(j, "tab", s->state.tabs[arg].id);
            screen_browser_send_input(s, j);
        }
        break;
    case ACT_MORE: more_menu(s, pt); break;
    case ACT_EXPAND: app_set_browser_expanded(!app_browser_expanded()); break;
    case ACT_DETACH: case ACT_DOCK: case ACT_CLOSE_PANE: screen_browser_post_move(s, action); break;
    }
}

/// A right click on a tab shows or closes it, as a browser's tab menu would.
static void browser_context(Screen *base, int action, intptr_t arg, POINT pt) {
    BrowserScreen *s = (BrowserScreen *)base;
    if (action != ACT_TAB || arg < 0 || (size_t)arg >= s->state.count || !screen_browser_can_drive()) return;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Show this tab");
    AppendMenuW(menu, MF_STRING, 2, L"Close this tab");
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD, pt.x, pt.y, 0, pane_hwnd(base->pane), NULL);
    DestroyMenu(menu);
    if ((size_t)arg >= s->state.count || chosen < 1) return;
    Json *j = screen_browser_simple_input(chosen == 1 ? "tab" : "closeTab"); json_set_str(j, "tab", s->state.tabs[arg].id);
    screen_browser_send_input(s, j);
}

static void browser_timer(Screen *base, UINT id) {
    BrowserScreen *s = (BrowserScreen *)base;
    if (id == TIMER_RECONNECT) { KillTimer(pane_hwnd(base->pane), TIMER_RECONNECT); start_feed(s); return; }
    if (poller_fired(&s->poller, id)) { if (!s->req_state) screen_browser_read_state(s); else poller_finished(&s->poller, false, -1); }
}

static void browser_visible(Screen *base, bool shown) {
    BrowserScreen *s = (BrowserScreen *)base;
    s->shown = shown;
    if (shown) {
        screen_browser_ensure_controls(s);
        screen_browser_read_state(s);
        start_feed(s);
    } else {
        // Frames are only sent while somebody watches: hidden, the stream stops.
        screen_browser_stop_feed(s);
        poller_stop(&s->poller);
        request_cancel(&s->req_state);
        if (s->view) { ShowWindow(s->view, SW_HIDE); ShowWindow(s->address, SW_HIDE); }
    }
}

static void browser_refresh(Screen *base) {
    BrowserScreen *s = (BrowserScreen *)base;
    if (s->state.running) screen_browser_send_input(s, screen_browser_simple_input("reload"));
    screen_browser_read_state(s);
}

static bool browser_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    BrowserScreen *s = (BrowserScreen *)base;
    (void)shift;
    if (ctrl && vk == 'L' && s->address && IsWindowVisible(s->address)) { SetFocus(s->address); SendMessageW(s->address, EM_SETSEL, 0, -1); return true; }
    return false;
}

static void browser_destroy(Screen *base) {
    BrowserScreen *s = (BrowserScreen *)base;
    screen_browser_stop_feed(s);
    poller_stop(&s->poller);
    request_cancel(&s->req_state); request_cancel(&s->req_toggle); request_cancel(&s->req_input);
    if (s->view) { SetWindowLongPtrW(s->view, GWLP_USERDATA, 0); DestroyWindow(s->view); }
    if (s->address) DestroyWindow(s->address);
    browser_state_free(&s->state);
    browser_inputs_free(&s->inputs);
    free(s->pixels); free(s->session_id); free(s->title); free(s->error); free(s->state_error); free(s->window_title);
    screen_release(base);
}

static const ScreenVTable browser_vt = {
    .destroy = browser_destroy, .layout = screen_browser_browser_layout, .header = browser_header, .action = browser_action,
    .context = browser_context, .timer = browser_timer, .place = screen_browser_browser_place, .visible = browser_visible,
    .refresh = browser_refresh, .key = browser_key,
};

bool browser_offered(void) { return store_supports("browser"); }

Screen *screen_browser_browser_new(const char *session_id_, const char *title, bool on, bool running, bool detached) {
    BrowserScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &browser_vt;
    s->base.id = xstrfmt("browser:%s", session_id_);
    s->session_id = xstrdup(session_id_);
    s->title = xstrdup(title);
    s->state.on = on; s->state.running = running;
    s->detached = detached;
    return &s->base;
}

Screen *browser_screen_new(const Session *session) {
    // The session record says whether the browser is on before the first read does.
    bool running = false, on = browser_session_on(session->raw, &running);
    return screen_browser_browser_new(session_id(session), session_display_title(session), on, running, false);
}
