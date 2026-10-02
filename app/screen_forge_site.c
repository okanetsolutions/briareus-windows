// One Forge site, opened from a project's Forge tab: its Overview (everything Forge says about it, read again), its
// Deploy script and its Environment (.env), each editable and saved back to Forge through the server's proxy
// (/forge/accounts/{account}/servers/{server}/sites/{site}[/deployment-script|/env]). The .env is a production secret: it
// is read only when asked for, and replacing it is confirmed first.
#include "screens.h"
#include "str.h"
#include <commctrl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TAB_OVERVIEW, TAB_SCRIPT, TAB_ENV };
enum { ED_SCRIPT, ED_ENV, ED_COUNT };
enum { ACT_TAB = 1400, ACT_SAVE, ACT_REVEAL, ACT_AUTO_SOURCE, ACT_OPEN_SITE, ACT_RELOAD, ACT_FOCUS };
enum { ID_EDITOR = 2400 };

typedef struct {
    Screen base;
    char *repo;
    double account, server_id, site_id;
    Json *server;           // the ForgeServer the site was opened from
    Json *site;             // the ForgeSite, as listed and then as read again
    int tab;
    bool shown;
    char *site_error;
    Request *req_site;
    // The deploy script and the .env: what was read, whether it was read, the editors over them.
    char *saved[ED_COUNT];
    bool loaded[ED_COUNT];
    bool auto_source, saved_auto_source;
    char *error[ED_COUNT];
    char *notice[ED_COUNT];   // a save's word that it went through
    Request *req_load[ED_COUNT], *req_save[ED_COUNT];
    bool env_wanted;          // the .env was asked for
    HWND edits[ED_COUNT];
    RECT rects[ED_COUNT];
    bool laid[ED_COUNT], clipped[ED_COUNT];
    bool filling;
    bool changed[ED_COUNT];
} SiteScreen;

static const char *site_name(const SiteScreen *s) {
    const char *name = json_str_nonempty(json_get(s->site, "name"));
    return name ? name : "Forge site";
}
static Json *site_args(const SiteScreen *s) {
    Json *a = json_object();
    json_set_num(a, "account", s->account);
    json_set_num(a, "server", s->server_id);
    json_set_num(a, "site", s->site_id);
    return a;
}
static int margin_of(Pane *pane) { RECT rc; GetClientRect(pane_hwnd(pane), &rc); return (rc.right - rc.left - pane_content_width(pane)) / 2; }
static void refresh_view(SiteScreen *s) {
    if (!s->base.pane) return;
    pane_relayout(s->base.pane);
    pane_header_changed(s->base.pane);
}

// MARK: - The editors' text

/// Forge's text with "\n" line ends, as the edit control's with "\r\n", and back.
static wchar_t *to_edit_text(const char *text) {
    Str s; str_init(&s);
    for (const char *p = text ? text : ""; *p; p++) {
        if (*p == '\r') continue;
        if (*p == '\n') str_appendc(&s, '\r');
        str_appendc(&s, *p);
    }
    wchar_t *w = utf8_to_wide(s.data ? s.data : "");
    str_free(&s);
    return w;
}
static char *editor_text(HWND e) {
    int n = GetWindowTextLengthW(e);
    wchar_t *w = xcalloc((size_t)n + 1, sizeof *w);
    GetWindowTextW(e, w, n + 1);
    char *u = wide_to_utf8(w);
    free(w);
    char *out = u, *q = u;
    for (const char *p = u; *p; p++) if (*p != '\r') *q++ = *p;
    *q = 0;
    return out;
}
static void fill_editor(SiteScreen *s, int ed) {
    if (!s->edits[ed]) return;
    s->filling = true;
    wchar_t *w = to_edit_text(s->saved[ed]);
    SetWindowTextW(s->edits[ed], w);
    free(w);
    s->filling = false;
    s->changed[ed] = false;
}
static bool editor_changed(SiteScreen *s, int ed) {
    if (!s->loaded[ed] || !s->edits[ed]) return false;
    char *now = editor_text(s->edits[ed]);
    bool differs = !str_eq(now, s->saved[ed] ? s->saved[ed] : "");
    free(now);
    return differs || (ed == ED_SCRIPT && s->auto_source != s->saved_auto_source);
}
static void note_change(SiteScreen *s, int ed) {
    if (s->filling) return;
    bool changed = editor_changed(s, ed);
    if (changed != s->changed[ed]) { s->changed[ed] = changed; refresh_view(s); }
    if (s->notice[ed]) { set_string(&s->notice[ed], NULL); refresh_view(s); }
}

// MARK: - Loading

static void site_done(void *owner, Request *req) {
    SiteScreen *s = owner;
    const Json *site = req->ok ? json_get(req->result, "site") : NULL;
    if (!json_is_object(site)) request_error_into(&s->site_error, req);
    else { set_string(&s->site_error, NULL); json_free(s->site); s->site = json_clone(site); }
    refresh_view(s);
}
static void site_load(SiteScreen *s) {
    if (s->req_site || !store_supports("forge_site")) return;
    store_call("forge_site", site_args(s), 0, s, site_done, 0, &s->req_site);
}
static void text_done(void *owner, Request *req) {
    SiteScreen *s = owner;
    int ed = req->tag;
    if (!req->ok) { request_error_into(&s->error[ed], req); refresh_view(s); return; }
    const char *content = json_str(json_get(req->result, "content"));
    set_string(&s->error[ed], NULL);
    set_string(&s->saved[ed], content ? content : "");
    if (ed == ED_SCRIPT) s->auto_source = s->saved_auto_source = json_bool_is(json_get(req->result, "autoSource"), true);
    s->loaded[ed] = true;
    fill_editor(s, ed);
    refresh_view(s);
}
static void text_load(SiteScreen *s, int ed) {
    const char *op = ed == ED_SCRIPT ? "forge_deploy_script" : "forge_env";
    if (s->req_load[ed] || !store_supports(op)) return;
    set_string(&s->error[ed], NULL);
    store_call(op, site_args(s), 0, s, text_done, ed, &s->req_load[ed]);
    refresh_view(s);
}
/// What the open tab needs, read once: the script on its first showing, the .env only once asked for.
static void tab_load(SiteScreen *s) {
    if (s->tab == TAB_SCRIPT && !s->loaded[ED_SCRIPT]) text_load(s, ED_SCRIPT);
    if (s->tab == TAB_ENV && s->env_wanted && !s->loaded[ED_ENV]) text_load(s, ED_ENV);
}

// MARK: - Saving

static void save_done(void *owner, Request *req) {
    SiteScreen *s = owner;
    int ed = req->tag;
    if (!req->ok) { request_error_into(&s->error[ed], req); refresh_view(s); return; }
    set_string(&s->error[ed], NULL);
    // What was sent is what Forge now holds; the script's answer is Forge's own word on it.
    const char *content = ed == ED_SCRIPT ? json_str(json_get(req->result, "content")) : NULL;
    char *sent = json_str(json_get(req->args, "content")) ? xstrdup(json_str(json_get(req->args, "content"))) : xstrdup("");
    set_string(&s->saved[ed], content ? content : sent);
    free(sent);
    if (ed == ED_SCRIPT) s->auto_source = s->saved_auto_source = json_bool_is(json_get(req->result, "autoSource"), s->auto_source);
    fill_editor(s, ed);
    set_string(&s->notice[ed], ed == ED_SCRIPT ? "Deploy script saved to Forge."
                                               : "Forge accepted the .env and writes it to the server shortly. Clear the config cache and restart the queue workers for running code to pick it up.");
    refresh_view(s);
}
static void save(SiteScreen *s) {
    int ed = s->tab == TAB_SCRIPT ? ED_SCRIPT : s->tab == TAB_ENV ? ED_ENV : -1;
    if (ed < 0 || !s->loaded[ed] || s->req_save[ed] || !editor_changed(s, ed)) return;
    const char *op = ed == ED_SCRIPT ? "set_forge_deploy_script" : "set_forge_env";
    if (!store_supports(op)) return;
    if (ed == ED_ENV) {
        char *title = xstrfmt("Replace the .env of %s?", site_name(s));
        bool ok = app_confirm(title, "Forge writes this file to the server in place of the current one. Running code keeps the old values until the config cache is cleared and the queue workers restart.", "Replace .env", true);
        free(title);
        if (!ok) return;
    }
    Json *args = site_args(s);
    char *text = editor_text(s->edits[ed]);
    json_set_str(args, "content", text);
    free(text);
    if (ed == ED_SCRIPT) json_set_bool(args, "autoSource", s->auto_source);
    store_call(op, args, 0, s, save_done, ed, &s->req_save[ed]);
    refresh_view(s);
}

// MARK: - Layout

static void paint_editor_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    SiteScreen *s = it->data;
    HWND e = s->edits[it->arg];
    fill_round_rect(cv, rc, px(6), theme.field, e && GetFocus() == e ? theme.accent_dim : theme.line);
}
/// The editor `ed` in a box from `doc->y` to the bottom of the pane.
static void layout_editor(SiteScreen *s, Doc *doc, int x, int w, int ed) {
    RECT view = pane_content_rect(s->base.pane);
    int h = (view.bottom - view.top) - doc->y - px(40);
    if (h < px(260)) h = px(260);
    RECT box = { x, doc->y, x + w, doc->y + h };
    Item *it = doc_item(doc, doc_add(doc, &box, paint_editor_box));
    it->data = s; it->arg = ed; it->action = ACT_FOCUS;
    s->rects[ed] = (RECT){ box.left + px(8), box.top + px(6), box.right - px(4), box.bottom - px(4) };
    s->laid[ed] = true;
    doc->y = box.bottom;
}
typedef struct { char *text; bool on; } CheckData;
static void check_free(void *p) { CheckData *d = p; free(d->text); free(d); }
static void paint_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    CheckData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    int size = px(15), top = rc->top + (rc->bottom - rc->top - size) / 2;
    RECT b = { rc->left, top, rc->left + size, top + size };
    fill_round_rect(cv, &b, px(3), d->on ? theme.accent : theme.field, d->on ? theme.accent : hovered ? theme.accent_dim : theme.line_strong);
    if (d->on) draw_glyph(cv, 0xE73E, &b, FONT_ICON_SMALL, theme.on_accent);
    RECT t = { b.right + px(8), rc->top, rc->right, rc->bottom };
    draw_text(cv, d->text, &t, FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void layout_status(SiteScreen *s, Doc *doc, int w, int ed) {
    if (s->error[ed]) { doc_notice_box(doc, 0, w, s->error[ed]); doc_space(doc, px(10)); }
    if (s->notice[ed]) { doc_text(doc, 0, w, s->notice[ed], FONT_FOOTNOTE, theme.ok, DT_WORDBREAK); doc_space(doc, px(10)); }
}

/// A field read as text whatever its type.
static char *field_text(const Json *v) {
    const char *t = json_str_nonempty(v);
    if (t) return xstrdup(t);
    double n;
    if (json_num(v, &n) && isfinite(n)) return n == floor(n) ? xstrfmt("%.0f", n) : xstrfmt("%g", n);
    int b = json_bool_tristate(v);
    if (b >= 0) return xstrdup(b ? "Yes" : "No");
    if (json_is_array(v) && json_count(v)) {
        Str s; str_init(&s);
        for (size_t i = 0; i < json_count(v); i++) {
            char *one = field_text(json_at(v, i));
            if (one) { if (s.len) str_appendz(&s, ", "); str_appendz(&s, one); }
            free(one);
        }
        char *out = s.len ? xstrdup(s.data) : NULL;
        str_free(&s);
        return out;
    }
    return NULL;
}
/// Forge's snake_case name as words: "deployment_status" → "Deployment status", "php_version" → "PHP version".
static char *field_label(const char *key) {
    static const char *const upper[] = { "url", "php", "ip", "ssl", "id", "ssh", "dns", "http", "https", "tls" };
    Str s; str_init(&s);
    for (const char *p = key; *p;) {
        const char *end = strchr(p, '_');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        char *word = xstrndup(p, n);
        bool caps = false;
        for (size_t k = 0; k < sizeof upper / sizeof *upper; k++) caps |= str_ieq(word, upper[k]);
        for (char *c = word; *c; c++) {
            if (caps && *c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
            else if (!caps && !s.len && c == word && *c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
        }
        if (s.len) str_appendc(&s, ' ');
        str_appendz(&s, word);
        free(word);
        p += n;
        if (*p == '_') p++;
    }
    char *out = xstrdup(s.data ? s.data : "");
    str_free(&s);
    return out;
}
static void labeled(Doc *doc, int x, int w, const char *label, const Json *v) {
    char *text = field_text(v);
    if (!text) return;
    doc_space(doc, px(5));
    doc_labeled(doc, x, w, label, text, theme.text);
    free(text);
}
static bool site_url(const SiteScreen *s, char **out) {
    const char *url = json_str(json_get(s->site, "url"));
    *out = NULL;
    if (!url) return false;
    *out = !safe_web_url(url) && !strstr(url, "://") ? xstrfmt("https://%s", url) : xstrdup(url);
    if (safe_web_url(*out)) return true;
    free(*out); *out = NULL;
    return false;
}

static void layout_overview(SiteScreen *s, Doc *doc, int w) {
    // A wide pane would spread each label and its value apart; the column stops at a reading width.
    if (w > px(820)) w = px(820);
    if (s->site_error) { doc_notice_box(doc, 0, w, s->site_error); doc_space(doc, px(10)); }
    int box = doc_box_begin(doc, 0, w, px(12), theme.elevated, theme.line, px(10));
    doc_item(doc, box)->hover_fill = false;
    int ix = px(14), iw = w - px(28);
    doc_text(doc, ix, iw, "Site", FONT_CAPTION_SEMIBOLD, theme.muted, DT_LEFT | DT_SINGLELINE);
    labeled(doc, ix, iw, "Domain", json_get(s->site, "name"));
    labeled(doc, ix, iw, "Status", json_get(s->site, "status"));
    labeled(doc, ix, iw, "URL", json_get(s->site, "url"));
    labeled(doc, ix, iw, "Deployment", json_get(s->site, "deployment_status"));
    labeled(doc, ix, iw, "Quick deploy", json_get(s->site, "quick_deploy"));
    const Json *repo = json_get(s->site, "repository");
    if (json_is_object(repo)) {
        doc_space(doc, px(14));
        doc_text(doc, ix, iw, "Repository", FONT_CAPTION_SEMIBOLD, theme.muted, DT_LEFT | DT_SINGLELINE);
        for (size_t i = 0; i < json_count(repo); i++) {
            char *label = field_label(json_key(repo, i));
            labeled(doc, ix, iw, label, json_get(repo, json_key(repo, i)));
            free(label);
        }
    }
    doc_box_end(doc, box, px(12));
    // Everything else Forge says about the site, as Forge names it.
    static const char *const shown[] = { "name", "status", "url", "deployment_status", "quick_deploy", "repository", "id" };
    doc_space(doc, px(12));
    box = doc_box_begin(doc, 0, w, px(12), theme.elevated, theme.line, px(10));
    doc_item(doc, box)->hover_fill = false;
    doc_text(doc, ix, iw, "Everything Forge reports", FONT_CAPTION_SEMIBOLD, theme.muted, DT_LEFT | DT_SINGLELINE);
    labeled(doc, ix, iw, "Forge id", json_get(s->site, "id"));
    for (size_t i = 0; i < json_count(s->site); i++) {
        const char *key = json_key(s->site, i);
        bool skip = false;
        for (size_t k = 0; k < sizeof shown / sizeof *shown; k++) skip |= str_eq(key, shown[k]);
        if (skip || json_is_object(json_get(s->site, json_key(s->site, i)))) continue;
        char *label = field_label(key);
        labeled(doc, ix, iw, label, json_get(s->site, json_key(s->site, i)));
        free(label);
    }
    doc_box_end(doc, box, px(12));
    char *url;
    if (site_url(s, &url)) {
        doc_space(doc, px(12));
        ButtonSpec b = { 0xE8A7, "Open site", BUTTON_BORDERED, ACT_OPEN_SITE, 0, true };
        doc_button_row(doc, 0, w, &b, 1);
        free(url);
    }
    if (s->req_site && !s->site_error) { doc_space(doc, px(8)); doc_text(doc, 0, w, "Reading the site from Forge\xE2\x80\xA6", FONT_CAPTION, theme.muted, DT_LEFT); }
}
static void layout_script(SiteScreen *s, Doc *doc, int w) {
    if (!store_supports("forge_deploy_script")) { doc_text(doc, 0, w, "This server does not read Forge deploy scripts.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK); return; }
    layout_status(s, doc, w, ED_SCRIPT);
    if (!s->loaded[ED_SCRIPT]) { if (!s->error[ED_SCRIPT]) doc_loading(doc, 0, w, "Reading the deploy script\xE2\x80\xA6"); return; }
    doc_text(doc, 0, w, "The commands Forge runs on each deployment of this site, from the site's directory. $FORGE_ variables such as $FORGE_SITE_BRANCH and $FORGE_PHP are set.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    doc_space(doc, px(8));
    CheckData *d = xcalloc(1, sizeof *d);
    d->text = xstrdup("Run the script with the site's .env loaded");
    d->on = s->auto_source;
    int it = doc_custom(doc, 0, w, px(26), paint_check, d, check_free, ACT_AUTO_SOURCE, 0);
    doc_item(doc, it)->hand = true;
    doc_space(doc, px(8));
    layout_editor(s, doc, 0, w, ED_SCRIPT);
}
static void layout_env(SiteScreen *s, Doc *doc, int w) {
    if (!store_supports("forge_env")) { doc_text(doc, 0, w, "This server does not read Forge .env files.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK); return; }
    layout_status(s, doc, w, ED_ENV);
    if (!s->env_wanted) {
        doc_space(doc, px(24));
        doc_empty_state(doc, 0, w, 0xE72E, "The .env holds production secrets", "It is read from Forge only when you ask for it, and shown here in full.");
        doc_space(doc, px(12));
        ButtonSpec b = { 0xE7B3, "Show .env", BUTTON_PROMINENT, ACT_REVEAL, 0, true };
        doc_button_row(doc, px(8), w - px(8), &b, 1);
        return;
    }
    if (!s->loaded[ED_ENV]) { if (!s->error[ED_ENV]) doc_loading(doc, 0, w, "Reading the .env\xE2\x80\xA6"); return; }
    // Once saved, the save's own note says what happens next.
    if (!s->notice[ED_ENV]) {
        doc_text(doc, 0, w, "Saving replaces the whole file. Forge writes it to the server shortly after; it does not clear the config cache or restart queue workers.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
        doc_space(doc, px(8));
    }
    layout_editor(s, doc, 0, w, ED_ENV);
}

static void site_layout(Screen *base, Doc *doc) {
    SiteScreen *s = (SiteScreen *)base;
    memset(s->laid, 0, sizeof s->laid);
    int w = doc->width;
    doc_space(doc, px(8));
    int h = px(42), x = 0, y = doc->y;
    doc_tab(doc, &x, &y, 0, w, h, 0xE946, "Overview", NULL, s->tab == TAB_OVERVIEW, ACT_TAB, TAB_OVERVIEW);
    if (store_supports("forge_deploy_script")) doc_tab(doc, &x, &y, 0, w, h, 0xE756, s->changed[ED_SCRIPT] ? "Deploy script \xE2\x80\xA2" : "Deploy script", NULL, s->tab == TAB_SCRIPT, ACT_TAB, TAB_SCRIPT);
    if (store_supports("forge_env")) doc_tab(doc, &x, &y, 0, w, h, 0xE72E, s->changed[ED_ENV] ? "Environment \xE2\x80\xA2" : "Environment", NULL, s->tab == TAB_ENV, ACT_TAB, TAB_ENV);
    doc->y = y + h;
    doc_rule(doc, 0, w);
    doc_space(doc, px(16));
    switch (s->tab) {
    case TAB_SCRIPT: layout_script(s, doc, w); break;
    case TAB_ENV: layout_env(s, doc, w); break;
    default: layout_overview(s, doc, w); break;
    }
    doc_space(doc, px(16));
}

static void site_header(Screen *base, HeaderInfo *info) {
    SiteScreen *s = (SiteScreen *)base;
    snprintf(info->title, sizeof info->title, "%s", site_name(s));
    const char *server = json_str_nonempty(json_get(s->server, "name")), *ip = json_str_nonempty(json_get(s->server, "ip_address"));
    const char *state = json_str_nonempty(json_get(s->site, "status"));
    snprintf(info->subtitle, sizeof info->subtitle, "Forge site on %s%s%s%s", server ? server : "a server", ip ? " (" : "", ip ? ip : "", ip ? ")" : "");
    if (state) { size_t n = strlen(info->subtitle); snprintf(info->subtitle + n, sizeof info->subtitle - n, " \xC2\xB7 %s", state); }
    int ed = s->tab == TAB_SCRIPT ? ED_SCRIPT : s->tab == TAB_ENV ? ED_ENV : -1;
    if (ed >= 0 && s->loaded[ed]) {
        HeaderButton *b = &info->buttons[info->button_count++];
        snprintf(b->label, sizeof b->label, "%s", s->req_save[ed] ? "Saving\xE2\x80\xA6" : ed == ED_ENV ? "Save .env" : "Save");
        b->glyph = 0xE74E; b->action = ACT_SAVE; b->prominent = true;
        b->tip = ed == ED_ENV ? "Replace the site's .env on Forge (Ctrl+S)" : "Save the deploy script to Forge (Ctrl+S)";
        b->enabled = !s->req_save[ed] && s->changed[ed] && store_supports(ed == ED_SCRIPT ? "set_forge_deploy_script" : "set_forge_env");
    }
    HeaderButton *r = &info->buttons[info->button_count++];
    r->glyph = 0xE72C; r->action = ACT_RELOAD; r->enabled = true;
    r->tip = ed >= 0 ? "Read it from Forge again, dropping unsaved changes" : "Read the site from Forge again";
}

// MARK: - The editors

static void site_place(Screen *base, const RECT *content, int scroll_y) {
    SiteScreen *s = (SiteScreen *)base;
    int m = margin_of(base->pane);
    for (int ed = 0; ed < ED_COUNT; ed++) {
        HWND e = s->edits[ed];
        if (!e) continue;
        if (!s->shown || !s->laid[ed]) { ShowWindow(e, SW_HIDE); continue; }
        RECT r = { content->left + m + s->rects[ed].left, content->top + s->rects[ed].top - scroll_y, content->left + m + s->rects[ed].right, content->top + s->rects[ed].bottom - scroll_y };
        RECT visible;
        if (!IntersectRect(&visible, &r, content)) { ShowWindow(e, SW_HIDE); continue; }
        MoveWindow(e, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
        bool clipped = !EqualRect(&visible, &r);
        if (clipped) SetWindowRgn(e, CreateRectRgn(visible.left - r.left, visible.top - r.top, visible.right - r.left, visible.bottom - r.top), TRUE);
        else if (s->clipped[ed]) SetWindowRgn(e, NULL, TRUE);
        s->clipped[ed] = clipped;
        ShowWindow(e, SW_SHOWNA);
    }
}
static LRESULT CALLBACK editor_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    SiteScreen *s = (SiteScreen *)ref;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    switch (msg) {
    case WM_KEYDOWN:
        if (ctrl && wp == 'S') { save(s); return 0; }
        if (ctrl && wp == 'A') { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        if (wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); return 0; }
        break;
    case WM_CHAR:
        if (wp == 0x13 || wp == 0x01 || wp == 0x1B) return 0;
        break;
    case WM_NCDESTROY: RemoveWindowSubclass(hwnd, editor_proc, id); break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
static void ensure_editors(SiteScreen *s) {
    if (s->edits[0] || !s->base.pane) return;
    HWND owner = pane_hwnd(s->base.pane);
    for (int ed = 0; ed < ED_COUNT; ed++) {
        DWORD style = WS_CHILD | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN;
        HWND e = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, owner, (HMENU)(INT_PTR)(ID_EDITOR + ed), GetModuleHandleW(NULL), NULL);
        SendMessageW(e, WM_SETFONT, (WPARAM)font(FONT_MONO_SMALL), TRUE);
        SendMessageW(e, EM_SETLIMITTEXT, 1024 * 1024, 0);
        SetWindowSubclass(e, editor_proc, (UINT_PTR)ed, (DWORD_PTR)s);
        theme_apply_control(e);
        s->edits[ed] = e;
        if (s->loaded[ed]) fill_editor(s, ed);
    }
}

// MARK: - The screen

static void reload(SiteScreen *s) {
    int ed = s->tab == TAB_SCRIPT ? ED_SCRIPT : s->tab == TAB_ENV ? ED_ENV : -1;
    if (ed < 0) { request_cancel(&s->req_site); site_load(s); refresh_view(s); return; }
    if (s->changed[ed] && !app_confirm("Discard unsaved changes?", "Reading it from Forge again drops the changes made here.", "Discard", true)) return;
    request_cancel(&s->req_load[ed]);
    s->loaded[ed] = false; s->changed[ed] = false;
    set_string(&s->notice[ed], NULL);
    text_load(s, ed);
}
static void site_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    SiteScreen *s = (SiteScreen *)base;
    switch (action) {
    case ACT_TAB:
        s->tab = (int)arg;
        tab_load(s);
        refresh_view(s);
        break;
    case ACT_SAVE: save(s); break;
    case ACT_RELOAD: reload(s); break;
    case ACT_REVEAL: s->env_wanted = true; tab_load(s); break;
    case ACT_AUTO_SOURCE: s->auto_source = !s->auto_source; s->changed[ED_SCRIPT] = editor_changed(s, ED_SCRIPT); refresh_view(s); break;
    case ACT_FOCUS: if (arg >= 0 && arg < ED_COUNT && s->edits[arg]) SetFocus(s->edits[arg]); break;
    case ACT_OPEN_SITE: { char *url; if (site_url(s, &url)) { open_web_url(url); free(url); } break; }
    }
}
static void site_command(Screen *base, int id, int code, HWND control) {
    (void)control;
    SiteScreen *s = (SiteScreen *)base;
    int ed = id - ID_EDITOR;
    if (ed < 0 || ed >= ED_COUNT) return;
    if (code == EN_CHANGE) note_change(s, ed);
    else if (code == EN_SETFOCUS || code == EN_KILLFOCUS) pane_repaint(base->pane);
}
static bool site_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)shift;
    if (ctrl && vk == 'S') { save((SiteScreen *)base); return true; }
    return false;
}
static void site_visible(Screen *base, bool shown) {
    SiteScreen *s = (SiteScreen *)base;
    s->shown = shown;
    if (shown) { ensure_editors(s); site_load(s); tab_load(s); }
    else for (int ed = 0; ed < ED_COUNT; ed++) if (s->edits[ed]) ShowWindow(s->edits[ed], SW_HIDE);
}
static void site_refresh(Screen *base) { reload((SiteScreen *)base); }
static bool site_can_leave(Screen *base) {
    SiteScreen *s = (SiteScreen *)base;
    if (!s->changed[ED_SCRIPT] && !s->changed[ED_ENV]) return true;
    char *message = xstrfmt("The changes to %s%s%s have not been saved to Forge.", s->changed[ED_SCRIPT] ? "the deploy script" : "",
                            s->changed[ED_SCRIPT] && s->changed[ED_ENV] ? " and " : "", s->changed[ED_ENV] ? "the .env" : "");
    bool leave = app_confirm("Discard unsaved changes?", message, "Discard", true);
    free(message);
    if (leave) s->changed[ED_SCRIPT] = s->changed[ED_ENV] = false;
    return leave;
}
static void site_destroy(Screen *base) {
    SiteScreen *s = (SiteScreen *)base;
    request_cancel(&s->req_site);
    for (int ed = 0; ed < ED_COUNT; ed++) {
        request_cancel(&s->req_load[ed]); request_cancel(&s->req_save[ed]);
        if (s->edits[ed]) DestroyWindow(s->edits[ed]);
        // The .env stays in memory no longer than the screen.
        if (s->saved[ed]) SecureZeroMemory(s->saved[ed], strlen(s->saved[ed]));
        free(s->saved[ed]); free(s->error[ed]); free(s->notice[ed]);
    }
    json_free(s->server); json_free(s->site);
    free(s->repo); free(s->site_error);
    screen_release(base);
}

static const ScreenVTable site_vt = {
    .destroy = site_destroy, .layout = site_layout, .header = site_header, .action = site_action, .place = site_place,
    .visible = site_visible, .command = site_command, .key = site_key, .refresh = site_refresh, .can_leave = site_can_leave,
};
Screen *forge_site_screen_new(const char *repo, double account, const Json *server, const Json *site) {
    SiteScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &site_vt;
    s->repo = xstrdup(repo ? repo : "");
    s->account = account;
    s->server = json_clone(server);
    s->site = json_clone(site);
    json_num(json_get(server, "id"), &s->server_id);
    json_num(json_get(site, "id"), &s->site_id);
    s->base.id = xstrfmt("forge-site:%.0f:%.0f:%.0f", account, s->server_id, s->site_id);
    return &s->base;
}
