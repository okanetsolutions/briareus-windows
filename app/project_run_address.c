#include "project_run_internal.h"

/// A toolbar glyph, as the shared browser's: no frame, tinted under the mouse, greyed while it cannot act.
typedef struct { wchar_t glyph; bool enabled; } IconData;

static void address_sync(RunAddress *a);
static LRESULT CALLBACK run_address_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);
static void paint_field(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void paint_lock(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void paint_icon(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void icon(Doc *doc, int x, int y, int size, wchar_t glyph, const char *tip, int action, bool enabled);
static void access_done(void *owner, Request *req);

// MARK: - The address bar

static void address_sync(RunAddress *a) {
    if (!a->edit || GetFocus() == a->edit) return;
    wchar_t *text = utf8_to_wide(a->url ? a->url : "");
    SetWindowTextW(a->edit, text);
    free(text);
}

static LRESULT CALLBACK run_address_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    RunAddress *a = (RunAddress *)ref;
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        // Read-only until the browser is up: the address can be copied, not opened.
        if (!a->web || !*a->web || !webview_ready(*a->web)) return 0;
        int n = GetWindowTextLengthW(hwnd);
        wchar_t *text = xmalloc(((size_t)n + 1) * sizeof *text);
        GetWindowTextW(hwnd, text, n + 1);
        char *typed = wide_to_utf8(text), *url = browser_address(typed);
        free(typed); free(text);
        if (url && a->web && webview_navigate(*a->web, url)) {
            set_string(&a->url, url);
            SetFocus(GetParent(hwnd));
            address_sync(a);
        } else {
            EDITBALLOONTIP tip = { sizeof tip, L"Cannot open this address", L"Enter a web address: http, https or about:blank.", TTI_ERROR };
            SendMessageW(hwnd, EM_SHOWBALLOONTIP, 0, (LPARAM)&tip);
        }
        free(url);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); address_sync(a); return 0; }
    if (msg == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0 && !(GetKeyState(VK_MENU) & 0x8000) && (wp == 'A' || wp == 'L')) { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE || wp == 1 || wp == 12)) return 0;
    if (msg == WM_LBUTTONDOWN && GetFocus() != hwnd) { SetFocus(hwnd); SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
    if (msg == WM_KILLFOCUS) { LRESULT r = DefSubclassProc(hwnd, msg, wp, lp); address_sync(a); return r; }
    if (msg == WM_NCDESTROY) { RemoveWindowSubclass(hwnd, run_address_proc, id); a->edit = NULL; }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void run_address_place(RunAddress *a, Screen *host, WebView **web, const char *url, const RECT *content, int scroll_y, bool shown) {
    a->web = web;
    bool on = shown && content && !IsRectEmpty(&a->rc);
    if (on && !a->edit) {
        a->edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10,
                                 pane_hwnd(host->pane), NULL, GetModuleHandleW(NULL), NULL);
        SetWindowSubclass(a->edit, run_address_proc, 1, (DWORD_PTR)a);
        SendMessageW(a->edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Enter a web address");
        theme_apply_control(a->edit);
    }
    if (!a->edit) return;
    if (*web && webview_url(*web)) url = webview_url(*web);
    set_string(&a->url, url);
    address_sync(a);  // Page events must not replace an address while it is being edited.
    if (on) {
        RECT rc; GetClientRect(pane_hwnd(host->pane), &rc);
        int m = (rc.right - rc.left - pane_content_width(host->pane)) / 2;
        RECT r = a->rc;
        OffsetRect(&r, content->left + m, content->top - scroll_y);
        // Keep the native edit out of the header when the bar scrolls above the content: clipped, not hidden, so a
        // partly scrolled bar keeps the address being typed.
        RECT visible;
        on = r.right > r.left && IntersectRect(&visible, &r, content);
        if (on) {
            SendMessageW(a->edit, WM_SETFONT, (WPARAM)font(FONT_BODY), FALSE);
            MoveWindow(a->edit, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
            bool clipped = !EqualRect(&visible, &r);
            if (clipped) SetWindowRgn(a->edit, CreateRectRgn(visible.left - r.left, visible.top - r.top, visible.right - r.left, visible.bottom - r.top), TRUE);
            else if (a->clipped) SetWindowRgn(a->edit, NULL, TRUE);
            a->clipped = clipped;
        }
    }
    if (!on && GetFocus() == a->edit) SetFocus(GetParent(a->edit));
    // Read-only rather than disabled while the browser starts or has failed, so the address can still be copied.
    SendMessageW(a->edit, EM_SETREADONLY, !(*web && webview_ready(*web)), 0);
    ShowWindow(a->edit, on ? SW_SHOWNA : SW_HIDE);
}

void run_address_free(RunAddress *a) {
    if (a->edit) DestroyWindow(a->edit);
    free(a->url);
    memset(a, 0, sizeof *a);
}

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

void run_browser_bar(Doc *doc, int w, WebView *web, const char *url, const int actions[4], RunAddress *address) {
    bool ready = web && webview_ready(web);
    if (web && webview_url(web)) url = webview_url(web);
    int nh = px(32), ib = px(28), gap = px(2), ny = doc->y, by = ny + (nh - ib) / 2;
    icon(doc, 0, by, ib, 0xE72B, "Back", actions[0], ready && webview_can_back(web));
    icon(doc, ib + gap, by, ib, 0xE72A, "Forward", actions[1], ready && webview_can_forward(web));
    icon(doc, 2 * (ib + gap), by, ib, 0xE72C, "Reload the page", actions[2], ready);
    int fx = 3 * (ib + gap) + px(6), fr = w - ib - px(6);
    RECT field = { fx, ny, fr, ny + nh };
    doc_add(doc, &field, paint_field);
    RECT lock = { fx + px(10), ny, fx + px(26), ny + nh };
    doc_item(doc, doc_add(doc, &lock, paint_lock))->arg = url && str_has_prefix(url, "https://") ? 0xE72E : 0xE774;
    int eh = edit_line_height(FONT_BODY);
    SetRect(&address->rc, fx + px(34), ny + (nh - eh) / 2, fr - px(10), ny + (nh - eh) / 2 + eh);
    icon(doc, w - ib, by, ib, 0xE8A7, "Open in your browser", actions[3], url != NULL);
    doc->y = ny + nh + px(8);
}

// MARK: - The tab

void project_run_layout(ProjectRun *p, Doc *doc, int w) {
    RECT view = pane_content_rect(p->host->pane);
    if (p->delete_error) { doc_notice(doc, 0, w, p->delete_error); doc_space(doc, px(10)); }
    bool page = p->url && !p->busy && p->web && webview_ready(p->web);
    if (page && p->serve_error) { doc_text(doc, 0, w, p->serve_error, FONT_FOOTNOTE, theme.danger, DT_SINGLELINE | DT_END_ELLIPSIS); doc_space(doc, px(10)); }
    if (p->url && !p->busy) {
        int actions[4] = { p->base + A_BACK, p->base + A_FORWARD, p->base + A_RELOAD, p->base + A_BROWSER };
        run_browser_bar(doc, w, p->web, p->url, actions, &p->address);
    }
    int area = doc->y, h = (view.bottom - view.top) - area - px(12);
    if (h < px(320)) h = px(320);
    SetRect(&p->web_rc, 0, area, w, area + h);
    if (page) { doc->y = area + h; return; }
    const char *error = p->busy ? NULL : !p->url ? p->serve_error : p->web ? webview_error(p->web) : NULL;
    char *text = error ? xstrdup(error)
        : p->busy && p->url ? (p->asked ? xstrfmt("Restarting with profile %s\xE2\x80\xA6", p->asked) : xstrdup("Serving it again\xE2\x80\xA6"))
        : p->url ? xstrdup("Starting the browser\xE2\x80\xA6")
        : p->busy && p->session ? xstrdup("Serving it\xE2\x80\xA6")
        // Once the setup's console has lines it says what is happening; until then, a line saying what is coming.
        : p->log.count ? NULL
        : p->busy || p->pending ? xstrdup("Preparing a workspace on the default branch and serving it with the project\xE2\x80\x99s run commands. This can take a few minutes\xE2\x80\xA6")
        : NULL;
    if (text) doc_text(doc, 0, w, text, FONT_BODY, error ? theme.danger : theme.muted, DT_WORDBREAK);
    free(text);
    if (!p->url && p->serve_error && !p->busy) {
        doc_space(doc, px(10));
        doc_button(doc, 0, 0, "\xE2\x96\xB6 Try again", BUTTON_BORDERED, p->base + A_RETRY, 0, true);
    } else if (error && p->url) {
        doc_space(doc, px(8));
        int i = doc_text(doc, 0, w, "Open in your browser instead \xE2\x86\x97", FONT_BODY, theme.accent, DT_SINGLELINE);
        doc_item(doc, i)->action = p->base + A_BROWSER; doc_item(doc, i)->hand = true;
    }
    // The setup as it happens, its latest lines filling what is left of the area, as a terminal does.
    if (p->log.count && (p->busy || !p->url)) {
        doc_space(doc, px(12));
        int line = px(17), room = area + h - doc->y - px(24);
        size_t fit = room > line ? (size_t)(room / line) : 1, first = p->log.count > fit ? p->log.count - fit : 0;
        int box = doc_box_begin(doc, 0, w, px(10), theme.sunken, theme.line, px(6));
        doc_item(doc, box)->hover_fill = false;
        for (size_t i = first; i < p->log.count; i++)
            doc_text(doc, px(12), w - px(24), p->log.lines[i], FONT_MONO_SMALL, p->log.errors[i] ? theme.danger : theme.secondary, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        doc_box_end(doc, box, px(10));
    }
    if (doc->y < area + h) doc->y = area + h;
}

void project_run_header(ProjectRun *p, HeaderInfo *info) {
    snprintf(info->subtitle, sizeof info->subtitle, "%s \xC2\xB7 %s", p->repo, p->branch ? p->branch : "default branch");
    HeaderButton *b;
    if (p->profile_count) {
        b = &info->buttons[info->button_count++];
        snprintf(b->label, sizeof b->label, "%s \xE2\x96\xBE", project_run_shown_profile(p)); b->action = p->base + A_PROFILE; b->enabled = true; b->tip = "The run profile it is served with";
    }
    if (store_supports("delete")) {
        b = &info->buttons[info->button_count++];
        b->glyph = 0xE74D; b->action = p->base + A_DELETE; b->enabled = project_run_target(p) && !p->deleting; b->destructive = true; b->tip = "Delete this run and its workspace";
    }
}

static void access_done(void *owner, Request *req) {
    ProjectRun *p = owner;
    p->access_read = true;
    if (req->ok) {
        set_string(&p->access_id, json_str(json_get(req->result, "clientId")));
        set_string(&p->access_secret, json_str(json_get(req->result, "clientSecret")));
        set_string(&p->access_suffix, json_str(json_get(req->result, "hostSuffix")));
    }
    project_run_changed(p);
}

void project_run_place(ProjectRun *p, const RECT *content, int scroll_y, bool shown) {
    if (p->shown != shown) {
        p->shown = shown;
        project_run_timer_on(p, shown && p->busy);
    }
    bool on = shown && p->url && !p->busy;
    // The browser is a child of the pane, so it starts once the tab is first shown in one, and once the service token
    // that lets it past Cloudflare Access has been read.
    if (on && !p->web && !p->access_read && store_supports("preview_access")) {
        if (!p->req_access) store_call("preview_access", json_object(), 0, p, access_done, 0, &p->req_access);
        run_address_place(&p->address, p->host, &p->web, p->url, content, scroll_y, on);  // The address shows meanwhile.
        return;
    }
    if (on && !p->web && p->host->pane) {
        WebViewAccess access = { p->access_id, p->access_secret, p->access_suffix };
        p->web = webview_new(pane_hwnd(p->host->pane), p->url, &access, project_run_web_changed, p);
    }
    run_address_place(&p->address, p->host, &p->web, p->url, content, scroll_y, on);
    if (!p->web) return;
    if (on && content) {
        RECT rc; GetClientRect(pane_hwnd(p->host->pane), &rc);
        int m = (rc.right - rc.left - pane_content_width(p->host->pane)) / 2;
        RECT r = { content->left + m + p->web_rc.left, content->top + p->web_rc.top - scroll_y, content->left + m + p->web_rc.right, content->top + p->web_rc.bottom - scroll_y };
        RECT visible;
        if (!IntersectRect(&visible, &r, content)) on = false;
        else webview_set_bounds(p->web, &visible);
    }
    webview_show(p->web, on && content);
}
