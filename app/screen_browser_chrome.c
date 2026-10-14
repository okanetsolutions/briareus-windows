#include "screen_browser_internal.h"

/// A toolbar glyph, as Claude's browser pane has them: no frame, tinted under the mouse, greyed while it cannot act.
typedef struct { wchar_t glyph; bool enabled; } IconData;

/// A tab in the strip: its title alone, the one in view in the ink and the others muted until hovered.
typedef struct { char *label; bool active; } TabData;

static void input_done(void *owner, Request *req);
static void pump(BrowserScreen *s);
static void navigate(BrowserScreen *s);
static void paint_field(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void paint_lock(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void paint_icon(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void icon(Doc *doc, int x, int y, int size, wchar_t glyph, const char *tip, int action, bool enabled);
static void tab_free(void *p);
static void paint_tab(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void title_window(BrowserScreen *s);

static void input_done(void *owner, Request *req) {
    BrowserScreen *s = owner;
    if (!req->ok) {
        if (api_error_unauthorized(&req->error)) store_invalidate_credentials(&req->error);
        // 409: it stopped under us; what was waiting is for a page that is gone.
        if (req->error.status == 409) { browser_inputs_free(&s->inputs); screen_browser_went_down(s); screen_browser_read_state(s); }
        request_error_into(&s->error, req);
        screen_browser_changed(s);
    } else if (s->error) { set_string(&s->error, NULL); screen_browser_changed(s); }
    pump(s);
}

static void pump(BrowserScreen *s) {
    if (s->req_input || !s->state.running) return;
    Json *input = browser_inputs_pop(&s->inputs);
    if (!input) return;
    json_set_str(input, "sessionId", s->session_id);
    store_call("browser_input", input, 0, s, input_done, 0, &s->req_input);
}

void screen_browser_send_input(BrowserScreen *s, Json *input) {
    if (!screen_browser_can_drive() || !s->state.running) { json_free(input); return; }
    browser_inputs_push(&s->inputs, input);
    pump(s);
}

Json *screen_browser_simple_input(const char *type) { Json *j = json_object(); json_set_str(j, "type", type); return j; }

void screen_browser_add_modifiers(Json *input, bool with_shift) {
    Json *mods = json_array();
    if (GetKeyState(VK_MENU) < 0) json_array_push(mods, json_string("alt"));
    if (GetKeyState(VK_CONTROL) < 0) json_array_push(mods, json_string("ctrl"));
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) json_array_push(mods, json_string("meta"));
    if (with_shift && GetKeyState(VK_SHIFT) < 0) json_array_push(mods, json_string("shift"));
    if (json_count(mods)) json_object_set(input, "modifiers", mods); else json_free(mods);
}

/// A pointer input at a view point, or NULL off the picture.
Json *screen_browser_pointer_input(BrowserScreen *s, const char *type, int x, int y) {
    double page_x, page_y;
    if (!browser_page_point(&s->drawn, s->fw, s->fh, x, y, &page_x, &page_y)) return NULL;
    Json *j = screen_browser_simple_input(type);
    json_set_num(j, "x", floor(page_x)); json_set_num(j, "y", floor(page_y));
    screen_browser_add_modifiers(j, true);
    return j;
}

static void navigate(BrowserScreen *s) {
    int n = GetWindowTextLengthW(s->address);
    wchar_t *w = xmalloc(((size_t)n + 1) * sizeof *w);
    GetWindowTextW(s->address, w, n + 1);
    char *typed = wide_to_utf8(w); free(w);
    char *url = browser_address(typed);
    free(typed);
    if (!url) { set_string(&s->error, "Enter a web address: http, https or about:blank."); screen_browser_changed(s); return; }
    Json *j = screen_browser_simple_input("navigate"); json_set_str(j, "url", url);
    free(url);
    screen_browser_send_input(s, j);
    SetFocus(s->view);
}

void screen_browser_paste(BrowserScreen *s) {
    if (!OpenClipboard(s->view)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *w = h ? GlobalLock(h) : NULL;
    char *text = w ? wide_to_utf8(w) : NULL;
    if (w) GlobalUnlock(h);
    CloseClipboard();
    if (text && *text) {
        char *lf = str_replace(text, "\r\n", "\n");
        Json *j = screen_browser_simple_input("type"); json_set_str(j, "text", lf);
        screen_browser_send_input(s, j);
        free(lf);
    }
    free(text);
}

LRESULT CALLBACK screen_browser_address_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    BrowserScreen *s = (BrowserScreen *)ref;
    if (msg == WM_KEYDOWN && wp == VK_RETURN) { navigate(s); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { SetFocus(s->view); screen_browser_set_address(s); return 0; }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;
    // A click into the field takes the whole address, as a browser's does, so typing replaces it.
    if (msg == WM_LBUTTONDOWN && GetFocus() != hwnd) { SetFocus(hwnd); SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
    if (msg == WM_KILLFOCUS) { LRESULT r = DefSubclassProc(hwnd, msg, wp, lp); screen_browser_set_address(s); return r; }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, screen_browser_address_proc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// MARK: - The chrome

static void paint_field(Doc *doc, Item *it, Canvas *cv, const RECT *rc) { (void)doc; (void)it; fill_round_rect(cv, rc, px(8), theme.field, theme.line); }

static void paint_lock(Doc *doc, Item *it, Canvas *cv, const RECT *rc) { (void)doc; draw_glyph(cv, (wchar_t)it->arg, rc, FONT_ICON_SMALL, theme.muted); }

static void paint_icon(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    IconData *d = it->data;
    bool hovered = d->enabled && doc_item_hovered(doc, it);
    if (hovered) fill_round_rect(cv, rc, px(6), theme.raise, theme.raise);
    draw_glyph(cv, d->glyph, rc, FONT_ICON_SMALL, !d->enabled ? theme.line_strong : hovered ? theme.ink : theme.muted);
}

static void icon(Doc *doc, int x, int y, int size, wchar_t glyph, const char *tip, int action, bool enabled) {
    IconData *d = xmalloc(sizeof *d); d->glyph = glyph; d->enabled = enabled;
    int keep = doc->y;
    doc->y = y;
    Item *it = doc_item(doc, doc_custom(doc, x, size, size, paint_icon, d, free, enabled ? action : 0, 0));
    it->hover_fill = false;
    it->tip = xstrdup(tip);
    doc->y = keep;
}

static void tab_free(void *p) { TabData *d = p; free(d->label); free(d); }

static void paint_tab(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    TabData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    if (d->active || hovered) fill_round_rect(cv, rc, px(6), d->active ? theme.raise : blend(theme.raise, theme.sidebar, 0.6), theme.raise);
    RECT t = { rc->left + px(10), rc->top, rc->right - px(10), rc->bottom };
    draw_text(cv, d->label, &t, d->active ? FONT_FOOTNOTE_SEMIBOLD : FONT_FOOTNOTE, d->active || hovered ? theme.ink : theme.muted,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

/// A tab's title for its strip: the page title, else its address.
const char *screen_browser_tab_label(const BrowserTab *tab) {
    return !str_empty(tab->title) ? tab->title : !str_empty(tab->url) && !str_eq(tab->url, "about:blank") ? tab->url : "New tab";
}

/// Whether the session's conversation is the page in the detail, which the browser can dock beside.
bool screen_browser_conversation_shown(const char *session_id) {
    Screen *root = app_detail_pane() ? pane_root(app_detail_pane()) : NULL;
    return root && root->id && str_has_prefix(root->id, "conversation:") && str_eq(root->id + 13, session_id);
}

bool screen_browser_can_dock(BrowserScreen *s) { return app_browser_dockable() && screen_browser_conversation_shown(s->session_id); }

/// A window of its own is titled after the page in view, as a browser's is.
static void title_window(BrowserScreen *s) {
    if (!s->detached || !s->base.pane) return;
    const BrowserTab *tab = s->state.running ? browser_active_tab(&s->state) : NULL;
    char *title = xstrfmt("%s \xC2\xB7 %s", tab ? screen_browser_tab_label(tab) : "Browser", s->title ? s->title : "Conversation");
    if (!str_eq(title, s->window_title)) {
        wchar_t *w = utf8_to_wide(title);
        SetWindowTextW(GetAncestor(pane_hwnd(s->base.pane), GA_ROOT), w);
        free(w);
        free(s->window_title); s->window_title = title;
    } else free(title);
}

void screen_browser_browser_layout(Screen *base, Doc *doc) {
    BrowserScreen *s = (BrowserScreen *)base;
    int w = doc->width;
    SetRectEmpty(&s->view_rc); SetRectEmpty(&s->address_rc);
    title_window(s);
    // The top bar: the tabs and ＋ on the left; pop out (or dock), ⋯, expand and close on the right.
    int bar = px(36), ib = px(28), top = doc->y + px(4), iy = top + (bar - ib) / 2, gap = px(2);
    int x = w - ib;
    icon(doc, x, iy, ib, 0xE711, s->detached ? "Close the window" : "Close the browser", ACT_CLOSE_PANE, true);
    if (!s->detached) {
        bool expanded = app_browser_expanded();
        x -= ib + gap;
        icon(doc, x, iy, ib, expanded ? 0xE73F : 0xE740, expanded ? "Show the conversation again" : "Expand over the conversation", ACT_EXPAND, true);
    }
    x -= ib + gap;
    icon(doc, x, iy, ib, 0xE712, "More", ACT_MORE, true);
    x -= ib + gap;
    if (s->detached) icon(doc, x, iy, ib, 0xE90D, screen_browser_can_dock(s) ? "Dock beside the conversation" : "Dock beside the conversation (open the conversation first)", ACT_DOCK, screen_browser_can_dock(s));
    else icon(doc, x, iy, ib, 0xE8A7, "Open in a separate window", ACT_DETACH, true);
    int right = x - px(8);
    if (s->state.running && s->state.count) {
        int plus = screen_browser_can_drive() ? ib + px(4) : 0, avail = right - plus;
        int share = (avail - (int)s->state.count * gap) / (int)s->state.count;
        if (share > px(200)) share = px(200);
        if (share < px(72)) share = px(72);
        int tx = 0;
        for (size_t i = 0; i < s->state.count; i++) {
            const char *label = screen_browser_tab_label(&s->state.tabs[i]);
            bool active = str_eq(s->state.tabs[i].id, s->state.active);
            int tw = text_width(doc->cv, label, active ? FONT_FOOTNOTE_SEMIBOLD : FONT_FOOTNOTE) + px(22);
            if (tw > share) tw = share;
            // Tabs past the room left stay reachable from ⋯.
            if (tx + tw > avail) break;
            TabData *d = xmalloc(sizeof *d); d->label = xstrdup(label); d->active = active;
            doc->y = top + px(4);
            Item *it = doc_item(doc, doc_custom(doc, tx, tw, bar - px(8), paint_tab, d, tab_free, ACT_TAB, (intptr_t)i));
            it->hover_fill = false;
            if (strlen(label) > 24) it->tip = xstrdup(label);
            tx += tw + gap;
        }
        if (screen_browser_can_drive()) icon(doc, tx + px(2), iy, ib, 0xE710, "New tab", ACT_NEW_TAB, true);
    } else {
        RECT t = { 0, top, right, top + bar };
        doc_text_at(doc, &t, "Browser", FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    doc->y = top + bar + px(4);
    const char *notice = s->error ? s->error : s->state_error;
    if (!s->state_read || !s->state.running) {
        doc_space(doc, px(8));
        if (notice) { doc_notice(doc, 0, w, notice); doc_space(doc, px(10)); }
        if (!s->state_read) { doc_loading(doc, 0, w, "Reading the browser\xE2\x80\xA6"); return; }
        const char *title = s->starting ? "Starting the browser\xE2\x80\xA6" : s->state.on ? "The browser is switched on but not running" : "The shared browser is off";
        const char *detail = s->closed_session ? "This conversation is closed. Reopen it, then start the browser."
            : s->state.on ? "It starts with the agent\xE2\x80\x99s next turn, or start it now. Its cookies and logins are still there."
            : "Switch on a Chromium of this session\xE2\x80\x99s own: the agent drives it with its browser tools, and you watch and use it here at the same time, on the same tabs. You can log in for the agent or take over a step and hand back with a message.";
        doc_empty_state(doc, 0, w, 0xE774, title, detail);
        if (store_can_manage() && store_supports("browser_on")) {
            doc_space(doc, px(8));
            doc_button(doc, 0, 0, s->starting ? "Starting\xE2\x80\xA6" : "\xE2\x96\xB6 Start the browser", BUTTON_PROMINENT, ACT_START, 0, !s->starting && !s->stopping);
        }
        return;
    }
    // The address bar: back, forward and reload, then the tab in view's address, and where to go on Enter.
    bool drive = screen_browser_can_drive();
    int nh = px(32), ny = doc->y, by = ny + (nh - ib) / 2;
    icon(doc, 0, by, ib, 0xE72B, "Back", ACT_BACK, drive);
    icon(doc, ib + gap, by, ib, 0xE72A, "Forward", ACT_FORWARD, drive);
    icon(doc, 2 * (ib + gap), by, ib, 0xE72C, "Reload", ACT_RELOAD, drive);
    int fx = 3 * (ib + gap) + px(6);
    RECT field = { fx, ny, w, ny + nh };
    doc_add(doc, &field, paint_field);
    RECT lock = { fx + px(10), ny, fx + px(26), ny + nh };
    const BrowserTab *tab = browser_active_tab(&s->state);
    doc_item(doc, doc_add(doc, &lock, paint_lock))->arg = tab && str_has_prefix(tab->url, "https://") ? 0xE72E : 0xE774;
    int eh = edit_line_height(FONT_BODY);
    SetRect(&s->address_rc, fx + px(34), ny + (nh - eh) / 2, w - px(10), ny + (nh - eh) / 2 + eh);
    doc->y = ny + nh + px(8);
    if (notice) { doc_notice(doc, 0, w, notice); doc_space(doc, px(6)); }
    if (!drive) { doc_text(doc, 0, w, "Watching only: this token cannot act in the browser.", FONT_CAPTION, theme.muted, DT_SINGLELINE | DT_END_ELLIPSIS); doc_space(doc, px(6)); }
    // The picture fills what is left of the pane.
    RECT view = pane_content_rect(base->pane);
    int area = doc->y, vh = (view.bottom - view.top) - area - px(10);
    if (vh < px(200)) vh = px(200);
    SetRect(&s->view_rc, 0, area, w, area + vh);
    doc->y = area + vh;
}

void screen_browser_browser_place(Screen *base, const RECT *content, int scroll_y) {
    BrowserScreen *s = (BrowserScreen *)base;
    if (!s->view) return;
    bool on = s->shown && s->state.running && content && !IsRectEmpty(&s->view_rc);
    if (on) {
        RECT rc; GetClientRect(pane_hwnd(base->pane), &rc);
        int m = (rc.right - rc.left - pane_content_width(base->pane)) / 2;
        RECT v = { content->left + m + s->view_rc.left, content->top + s->view_rc.top - scroll_y, content->left + m + s->view_rc.right, content->top + s->view_rc.bottom - scroll_y };
        RECT a = { content->left + m + s->address_rc.left, content->top + s->address_rc.top - scroll_y, content->left + m + s->address_rc.right, content->top + s->address_rc.bottom - scroll_y };
        RECT visible;
        if (IntersectRect(&visible, &v, content)) SetWindowPos(s->view, NULL, visible.left, visible.top, visible.right - visible.left, visible.bottom - visible.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        MoveWindow(s->address, a.left, a.top, a.right - a.left, a.bottom - a.top, TRUE);
    }
    ShowWindow(s->view, on ? SW_SHOWNA : SW_HIDE);
    ShowWindow(s->address, on ? SW_SHOWNA : SW_HIDE);
    EnableWindow(s->address, screen_browser_can_drive());
}
