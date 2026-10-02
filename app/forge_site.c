// One Forge site inside a project's Forge tab, in place of the server's list of sites: its Overview (everything Forge
// says about it, read again), its Deploy script and its Environment (.env), each editable and saved back to Forge through
// the server's proxy (/forge/accounts/{account}/servers/{server}/sites/{site}[/deployment-script|/env]). The .env is a
// production secret: it is read only when asked for, and replacing it is confirmed first.
#include "screens.h"
#include "str.h"
#include <commctrl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TAB_OVERVIEW, TAB_SCRIPT, TAB_ENV };
enum { ED_SCRIPT, ED_ENV, ED_COUNT };
// The actions, from the host's `action_base` up.
enum { A_TAB, A_SAVE, A_REVEAL, A_AUTO_SOURCE, A_OPEN_SITE, A_FOCUS };
enum { ID_EDITOR = 2600 };

struct ForgeSite {
    Screen *host;
    int base;
    double account, server_id, site_id;
    Json *server;           // the ForgeServer the site is on
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
};

static const char *site_name(const ForgeSite *v) {
    const char *name = json_str_nonempty(json_get(v->site, "name"));
    return name ? name : "Forge site";
}
static Json *site_args(const ForgeSite *v) {
    Json *a = json_object();
    json_set_num(a, "account", v->account);
    json_set_num(a, "server", v->server_id);
    json_set_num(a, "site", v->site_id);
    return a;
}
static int margin_of(Pane *pane) { RECT rc; GetClientRect(pane_hwnd(pane), &rc); return (rc.right - rc.left - pane_content_width(pane)) / 2; }
static void refresh_view(ForgeSite *v) {
    if (!v->host->pane) return;
    pane_relayout(v->host->pane);
    pane_header_changed(v->host->pane);
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
    SecureZeroMemory(w, (size_t)n * sizeof *w);
    free(w);
    char *q = u;
    for (const char *p = u; *p; p++) if (*p != '\r') *q++ = *p;
    *q = 0;
    return u;
}
static void fill_editor(ForgeSite *v, int ed) {
    if (!v->edits[ed]) return;
    v->filling = true;
    wchar_t *w = to_edit_text(v->saved[ed]);
    SetWindowTextW(v->edits[ed], w);
    free(w);
    v->filling = false;
    v->changed[ed] = false;
}
static bool editor_changed(ForgeSite *v, int ed) {
    if (!v->loaded[ed] || !v->edits[ed]) return false;
    char *now = editor_text(v->edits[ed]);
    bool differs = !str_eq(now, v->saved[ed] ? v->saved[ed] : "");
    free(now);
    return differs || (ed == ED_SCRIPT && v->auto_source != v->saved_auto_source);
}
static void note_change(ForgeSite *v, int ed) {
    if (v->filling) return;
    bool changed = editor_changed(v, ed);
    if (changed != v->changed[ed]) { v->changed[ed] = changed; refresh_view(v); }
    if (v->notice[ed]) { set_string(&v->notice[ed], NULL); refresh_view(v); }
}

// MARK: - Loading

static void site_done(void *owner, Request *req) {
    ForgeSite *v = owner;
    const Json *site = req->ok ? json_get(req->result, "site") : NULL;
    if (!json_is_object(site)) request_error_into(&v->site_error, req);
    else { set_string(&v->site_error, NULL); json_free(v->site); v->site = json_clone(site); }
    refresh_view(v);
}
static void site_load(ForgeSite *v) {
    if (v->req_site || !store_supports("forge_site")) return;
    store_call("forge_site", site_args(v), 0, v, site_done, 0, &v->req_site);
}
static void text_done(void *owner, Request *req) {
    ForgeSite *v = owner;
    int ed = req->tag;
    if (!req->ok) { request_error_into(&v->error[ed], req); refresh_view(v); return; }
    const char *content = json_str(json_get(req->result, "content"));
    set_string(&v->error[ed], NULL);
    set_string(&v->saved[ed], content ? content : "");
    if (ed == ED_SCRIPT) v->auto_source = v->saved_auto_source = json_bool_is(json_get(req->result, "autoSource"), true);
    v->loaded[ed] = true;
    fill_editor(v, ed);
    refresh_view(v);
}
static void text_load(ForgeSite *v, int ed) {
    const char *op = ed == ED_SCRIPT ? "forge_deploy_script" : "forge_env";
    if (v->req_load[ed] || !store_supports(op)) return;
    set_string(&v->error[ed], NULL);
    store_call(op, site_args(v), 0, v, text_done, ed, &v->req_load[ed]);
    refresh_view(v);
}
/// What the open tab needs, read once: the script on its first showing, the .env only once asked for.
static void tab_load(ForgeSite *v) {
    if (v->tab == TAB_SCRIPT && !v->loaded[ED_SCRIPT]) text_load(v, ED_SCRIPT);
    if (v->tab == TAB_ENV && v->env_wanted && !v->loaded[ED_ENV]) text_load(v, ED_ENV);
}

// MARK: - Saving

static void save_done(void *owner, Request *req) {
    ForgeSite *v = owner;
    int ed = req->tag;
    if (!req->ok) { request_error_into(&v->error[ed], req); refresh_view(v); return; }
    set_string(&v->error[ed], NULL);
    // What was sent is what Forge now holds; the script's answer is Forge's own word on it.
    const char *content = ed == ED_SCRIPT ? json_str(json_get(req->result, "content")) : NULL;
    const char *sent = json_str(json_get(req->args, "content"));
    set_string(&v->saved[ed], content ? content : sent ? sent : "");
    if (ed == ED_SCRIPT) v->auto_source = v->saved_auto_source = json_bool_is(json_get(req->result, "autoSource"), v->auto_source);
    fill_editor(v, ed);
    set_string(&v->notice[ed], ed == ED_SCRIPT ? "Deploy script saved to Forge."
                                               : "Forge accepted the .env and writes it to the server shortly. Clear the config cache and restart the queue workers for running code to pick it up.");
    refresh_view(v);
}
static int open_editor(const ForgeSite *v) { return v->tab == TAB_SCRIPT ? ED_SCRIPT : v->tab == TAB_ENV ? ED_ENV : -1; }
static void save(ForgeSite *v) {
    int ed = open_editor(v);
    if (ed < 0 || !v->loaded[ed] || v->req_save[ed] || !editor_changed(v, ed)) return;
    const char *op = ed == ED_SCRIPT ? "set_forge_deploy_script" : "set_forge_env";
    if (!store_supports(op)) return;
    if (ed == ED_ENV) {
        char *title = xstrfmt("Replace the .env of %s?", site_name(v));
        bool ok = app_confirm(title, "Forge writes this file to the server in place of the current one. Running code keeps the old values until the config cache is cleared and the queue workers restart.", "Replace .env", true);
        free(title);
        if (!ok) return;
    }
    Json *args = site_args(v);
    char *text = editor_text(v->edits[ed]);
    json_set_str(args, "content", text);
    SecureZeroMemory(text, strlen(text));
    free(text);
    if (ed == ED_SCRIPT) json_set_bool(args, "autoSource", v->auto_source);
    store_call(op, args, 0, v, save_done, ed, &v->req_save[ed]);
    refresh_view(v);
}

// MARK: - Layout

static void paint_editor_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    ForgeSite *v = it->data;
    HWND e = v->edits[it->arg];
    fill_round_rect(cv, rc, px(6), theme.field, e && GetFocus() == e ? theme.accent_dim : theme.line);
}
/// The editor `ed` in a box from `doc->y` to the bottom of the pane.
static void layout_editor(ForgeSite *v, Doc *doc, int x, int w, int ed) {
    RECT view = pane_content_rect(v->host->pane);
    int h = (view.bottom - view.top) - doc->y - px(40);
    if (h < px(260)) h = px(260);
    RECT box = { x, doc->y, x + w, doc->y + h };
    Item *it = doc_item(doc, doc_add(doc, &box, paint_editor_box));
    it->data = v; it->arg = ed; it->action = v->base + A_FOCUS;
    v->rects[ed] = (RECT){ box.left + px(8), box.top + px(6), box.right - px(4), box.bottom - px(4) };
    v->laid[ed] = true;
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
static void layout_status(ForgeSite *v, Doc *doc, int x, int w, int ed) {
    if (v->error[ed]) { doc_notice_box(doc, x, w, v->error[ed]); doc_space(doc, px(10)); }
    if (v->notice[ed]) { doc_text(doc, x, w, v->notice[ed], FONT_FOOTNOTE, theme.ok, DT_WORDBREAK); doc_space(doc, px(10)); }
}

/// A field read as text whatever its type.
static char *field_text(const Json *j) {
    const char *t = json_str_nonempty(j);
    if (t) return xstrdup(t);
    double n;
    if (json_num(j, &n) && isfinite(n)) return n == floor(n) ? xstrfmt("%.0f", n) : xstrfmt("%g", n);
    int b = json_bool_tristate(j);
    if (b >= 0) return xstrdup(b ? "Yes" : "No");
    if (json_is_array(j) && json_count(j)) {
        Str s; str_init(&s);
        for (size_t i = 0; i < json_count(j); i++) {
            char *one = field_text(json_at(j, i));
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
/// A URL with the value of its `token` parameter hidden: Forge's deployment trigger URL deploys the site to whoever has it.
static char *masked_url(const char *text) {
    const char *t = strstr(text, "token=");
    if (!t) return xstrdup(text);
    const char *end = t + 6;
    while (*end && *end != '&' && *end != '#') end++;
    return xstrfmt("%.*stoken=\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2%s", (int)(t - text), text, end);
}
static void labeled(Doc *doc, int x, int w, const char *label, const Json *j, bool mask) {
    char *text = field_text(j);
    if (!text) return;
    if (mask) { char *m = masked_url(text); free(text); text = m; }
    doc_space(doc, px(5));
    doc_labeled(doc, x, w, label, text, theme.text);
    free(text);
}
static bool site_url(const ForgeSite *v, char **out) {
    const char *url = json_str(json_get(v->site, "url"));
    *out = NULL;
    if (!url) return false;
    *out = !safe_web_url(url) && !strstr(url, "://") ? xstrfmt("https://%s", url) : xstrdup(url);
    if (safe_web_url(*out)) return true;
    free(*out); *out = NULL;
    return false;
}

static void layout_overview(ForgeSite *v, Doc *doc, int x, int w) {
    if (v->site_error) { doc_notice_box(doc, x, w, v->site_error); doc_space(doc, px(10)); }
    int box = doc_box_begin(doc, x, w, px(12), theme.elevated, theme.line, px(10));
    doc_item(doc, box)->hover_fill = false;
    int ix = x + px(14), iw = w - px(28);
    doc_text(doc, ix, iw, "Site", FONT_CAPTION_SEMIBOLD, theme.muted, DT_LEFT | DT_SINGLELINE);
    labeled(doc, ix, iw, "Domain", json_get(v->site, "name"), false);
    labeled(doc, ix, iw, "Status", json_get(v->site, "status"), false);
    labeled(doc, ix, iw, "URL", json_get(v->site, "url"), false);
    labeled(doc, ix, iw, "Deployment", json_get(v->site, "deployment_status"), false);
    labeled(doc, ix, iw, "Quick deploy", json_get(v->site, "quick_deploy"), false);
    const Json *repo = json_get(v->site, "repository");
    if (json_is_object(repo)) {
        doc_space(doc, px(14));
        doc_text(doc, ix, iw, "Repository", FONT_CAPTION_SEMIBOLD, theme.muted, DT_LEFT | DT_SINGLELINE);
        for (size_t i = 0; i < json_count(repo); i++) {
            char *label = field_label(json_key(repo, i));
            labeled(doc, ix, iw, label, json_get(repo, json_key(repo, i)), false);
            free(label);
        }
    }
    doc_box_end(doc, box, px(12));
    // Everything else Forge says about the site, as Forge names it.
    static const char *const shown[] = { "name", "status", "url", "deployment_status", "quick_deploy", "repository", "id" };
    doc_space(doc, px(12));
    box = doc_box_begin(doc, x, w, px(12), theme.elevated, theme.line, px(10));
    doc_item(doc, box)->hover_fill = false;
    doc_text(doc, ix, iw, "Everything Forge reports", FONT_CAPTION_SEMIBOLD, theme.muted, DT_LEFT | DT_SINGLELINE);
    labeled(doc, ix, iw, "Forge id", json_get(v->site, "id"), false);
    for (size_t i = 0; i < json_count(v->site); i++) {
        const char *key = json_key(v->site, i);
        bool skip = false;
        for (size_t k = 0; k < sizeof shown / sizeof *shown; k++) skip |= str_eq(key, shown[k]);
        if (skip || json_is_object(json_get(v->site, key))) continue;
        char *label = field_label(key);
        labeled(doc, ix, iw, label, json_get(v->site, key), str_icontains(key, "url"));
        free(label);
    }
    doc_box_end(doc, box, px(12));
    char *url;
    if (site_url(v, &url)) {
        doc_space(doc, px(12));
        ButtonSpec b = { 0xE8A7, "Open site", BUTTON_BORDERED, v->base + A_OPEN_SITE, 0, true };
        doc_button_row(doc, x, w, &b, 1);
        free(url);
    }
    if (v->req_site && !v->site_error) { doc_space(doc, px(8)); doc_text(doc, x, w, "Reading the site from Forge\xE2\x80\xA6", FONT_CAPTION, theme.muted, DT_LEFT); }
}
static void layout_script(ForgeSite *v, Doc *doc, int x, int w) {
    layout_status(v, doc, x, w, ED_SCRIPT);
    if (!v->loaded[ED_SCRIPT]) { if (!v->error[ED_SCRIPT]) doc_loading(doc, x, w, "Reading the deploy script\xE2\x80\xA6"); return; }
    doc_text(doc, x, w, "The commands Forge runs on each deployment of this site, from the site's directory. $FORGE_ variables such as $FORGE_SITE_BRANCH and $FORGE_PHP are set.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    doc_space(doc, px(8));
    CheckData *d = xcalloc(1, sizeof *d);
    d->text = xstrdup("Run the script with the site's .env loaded");
    d->on = v->auto_source;
    int it = doc_custom(doc, x, w, px(26), paint_check, d, check_free, v->base + A_AUTO_SOURCE, 0);
    doc_item(doc, it)->hand = true;
    doc_space(doc, px(8));
    layout_editor(v, doc, x, w, ED_SCRIPT);
}
static void layout_env(ForgeSite *v, Doc *doc, int x, int w) {
    layout_status(v, doc, x, w, ED_ENV);
    if (!v->env_wanted) {
        doc_space(doc, px(24));
        doc_empty_state(doc, x, w, 0xE72E, "The .env holds production secrets", "It is read from Forge only when you ask for it, and shown here in full.");
        doc_space(doc, px(12));
        ButtonSpec b = { 0xE7B3, "Show .env", BUTTON_PROMINENT, v->base + A_REVEAL, 0, true };
        doc_button_row(doc, x + px(8), w - px(8), &b, 1);
        return;
    }
    if (!v->loaded[ED_ENV]) { if (!v->error[ED_ENV]) doc_loading(doc, x, w, "Reading the .env\xE2\x80\xA6"); return; }
    // Once saved, the save's own note says what happens next.
    if (!v->notice[ED_ENV]) {
        doc_text(doc, x, w, "Saving replaces the whole file. Forge writes it to the server shortly after; it does not clear the config cache or restart queue workers.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
        doc_space(doc, px(8));
    }
    layout_editor(v, doc, x, w, ED_ENV);
}

void forge_site_layout(ForgeSite *v, Doc *doc, int x, int w) {
    memset(v->laid, 0, sizeof v->laid);
    const char *state = json_str_nonempty(json_get(v->site, "status"));
    doc_text(doc, x, w, site_name(v), FONT_TITLE3, theme.ink, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (state) { doc_space(doc, px(2)); doc_text(doc, x, w, state, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_SINGLELINE); }
    doc_space(doc, px(6));
    int h = px(40), tx = x, ty = doc->y;
    doc_tab(doc, &tx, &ty, x, x + w, h, 0xE946, "Overview", NULL, v->tab == TAB_OVERVIEW, v->base + A_TAB, TAB_OVERVIEW);
    if (store_supports("forge_deploy_script"))
        doc_tab(doc, &tx, &ty, x, x + w, h, 0xE756, v->changed[ED_SCRIPT] ? "Deploy script \xE2\x80\xA2" : "Deploy script", NULL, v->tab == TAB_SCRIPT, v->base + A_TAB, TAB_SCRIPT);
    if (store_supports("forge_env"))
        doc_tab(doc, &tx, &ty, x, x + w, h, 0xE72E, v->changed[ED_ENV] ? "Environment \xE2\x80\xA2" : "Environment", NULL, v->tab == TAB_ENV, v->base + A_TAB, TAB_ENV);
    doc->y = ty + h;
    doc_rule(doc, x, w);
    doc_space(doc, px(14));
    switch (v->tab) {
    case TAB_SCRIPT: layout_script(v, doc, x, w); break;
    case TAB_ENV: layout_env(v, doc, x, w); break;
    default: {
        // A wide pane would spread each label and its value apart; the overview stops at a reading width.
        int ow = w > px(820) ? px(820) : w;
        layout_overview(v, doc, x, ow);
        break;
    }
    }
}

void forge_site_header(ForgeSite *v, HeaderInfo *info) {
    int ed = open_editor(v);
    if (ed < 0 || !v->loaded[ed]) return;
    HeaderButton *b = &info->buttons[info->button_count++];
    snprintf(b->label, sizeof b->label, "%s", v->req_save[ed] ? "Saving\xE2\x80\xA6" : ed == ED_ENV ? "Save .env" : "Save");
    b->glyph = 0xE74E; b->action = v->base + A_SAVE; b->prominent = true;
    b->tip = ed == ED_ENV ? "Replace the site's .env on Forge (Ctrl+S)" : "Save the deploy script to Forge (Ctrl+S)";
    b->enabled = !v->req_save[ed] && v->changed[ed] && store_supports(ed == ED_SCRIPT ? "set_forge_deploy_script" : "set_forge_env");
}

// MARK: - The editors

void forge_site_place(ForgeSite *v, const RECT *content, int scroll_y, bool shown) {
    v->shown = shown;
    int m = shown && content ? margin_of(v->host->pane) : 0;
    for (int ed = 0; ed < ED_COUNT; ed++) {
        HWND e = v->edits[ed];
        if (!e) continue;
        if (!shown || !content || !v->laid[ed] || open_editor(v) != ed) { ShowWindow(e, SW_HIDE); continue; }
        RECT r = { content->left + m + v->rects[ed].left, content->top + v->rects[ed].top - scroll_y, content->left + m + v->rects[ed].right, content->top + v->rects[ed].bottom - scroll_y };
        RECT visible;
        if (!IntersectRect(&visible, &r, content)) { ShowWindow(e, SW_HIDE); continue; }
        MoveWindow(e, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
        bool clipped = !EqualRect(&visible, &r);
        if (clipped) SetWindowRgn(e, CreateRectRgn(visible.left - r.left, visible.top - r.top, visible.right - r.left, visible.bottom - r.top), TRUE);
        else if (v->clipped[ed]) SetWindowRgn(e, NULL, TRUE);
        v->clipped[ed] = clipped;
        ShowWindow(e, SW_SHOWNA);
    }
}
static LRESULT CALLBACK editor_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    ForgeSite *v = (ForgeSite *)ref;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    switch (msg) {
    case WM_KEYDOWN:
        if (ctrl && wp == 'S') { save(v); return 0; }
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
static void ensure_editors(ForgeSite *v) {
    if (v->edits[0] || !v->host->pane) return;
    // Created with the pane as parent, so their notifications reach the board (see forge_site_command).
    HWND owner = pane_hwnd(v->host->pane);
    for (int ed = 0; ed < ED_COUNT; ed++) {
        DWORD style = WS_CHILD | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN;
        HWND e = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, owner, (HMENU)(INT_PTR)(ID_EDITOR + ed), GetModuleHandleW(NULL), NULL);
        SendMessageW(e, WM_SETFONT, (WPARAM)font(FONT_MONO_SMALL), TRUE);
        SendMessageW(e, EM_SETLIMITTEXT, 1024 * 1024, 0);
        SetWindowSubclass(e, editor_proc, (UINT_PTR)ed, (DWORD_PTR)v);
        theme_apply_control(e);
        v->edits[ed] = e;
        if (v->loaded[ed]) fill_editor(v, ed);
    }
}

// MARK: - Actions

void forge_site_refresh(ForgeSite *v) {
    int ed = open_editor(v);
    if (ed < 0) { request_cancel(&v->req_site); site_load(v); refresh_view(v); return; }
    if (ed == ED_ENV && !v->env_wanted) return;
    if (v->changed[ed] && !app_confirm("Discard unsaved changes?", "Reading it from Forge again drops the changes made here.", "Discard", true)) return;
    request_cancel(&v->req_load[ed]);
    v->loaded[ed] = false; v->changed[ed] = false;
    set_string(&v->notice[ed], NULL);
    text_load(v, ed);
}
bool forge_site_action(ForgeSite *v, int action, intptr_t arg) {
    if (!v || action < v->base || action >= v->base + FORGE_SITE_ACTIONS) return false;
    switch (action - v->base) {
    case A_TAB:
        v->tab = (int)arg;
        tab_load(v);
        refresh_view(v);
        break;
    case A_SAVE: save(v); break;
    case A_REVEAL: v->env_wanted = true; tab_load(v); break;
    case A_AUTO_SOURCE: v->auto_source = !v->auto_source; v->changed[ED_SCRIPT] = editor_changed(v, ED_SCRIPT); refresh_view(v); break;
    case A_FOCUS: if (arg >= 0 && arg < ED_COUNT && v->edits[arg]) SetFocus(v->edits[arg]); break;
    case A_OPEN_SITE: { char *url; if (site_url(v, &url)) { open_web_url(url); free(url); } break; }
    }
    return true;
}
bool forge_site_command(ForgeSite *v, int id, int code) {
    int ed = id - ID_EDITOR;
    if (!v || ed < 0 || ed >= ED_COUNT) return false;
    if (code == EN_CHANGE) note_change(v, ed);
    else if (code == EN_SETFOCUS || code == EN_KILLFOCUS) pane_repaint(v->host->pane);
    return true;
}
bool forge_site_key(ForgeSite *v, WPARAM vk, bool ctrl) {
    if (!v || !ctrl || vk != 'S' || open_editor(v) < 0) return false;
    save(v);
    return true;
}
bool forge_site_can_leave(ForgeSite *v) {
    if (!v || (!v->changed[ED_SCRIPT] && !v->changed[ED_ENV])) return true;
    char *message = xstrfmt("The changes to %s%s%s of %s have not been saved to Forge.", v->changed[ED_SCRIPT] ? "the deploy script" : "",
                            v->changed[ED_SCRIPT] && v->changed[ED_ENV] ? " and " : "", v->changed[ED_ENV] ? "the .env" : "", site_name(v));
    bool leave = app_confirm("Discard unsaved changes?", message, "Discard", true);
    free(message);
    if (leave) v->changed[ED_SCRIPT] = v->changed[ED_ENV] = false;
    return leave;
}

// MARK: - Lifetime

ForgeSite *forge_site_new(Screen *host, int action_base, double account, const Json *server, const Json *site) {
    ForgeSite *v = xcalloc(1, sizeof *v);
    v->host = host; v->base = action_base;
    v->account = account;
    v->server = json_clone(server);
    v->site = json_clone(site);
    json_num(json_get(server, "id"), &v->server_id);
    json_num(json_get(site, "id"), &v->site_id);
    ensure_editors(v);
    site_load(v);
    return v;
}
void forge_site_free(ForgeSite *v) {
    if (!v) return;
    request_cancel(&v->req_site);
    for (int ed = 0; ed < ED_COUNT; ed++) {
        request_cancel(&v->req_load[ed]); request_cancel(&v->req_save[ed]);
        if (v->edits[ed]) {
            // The .env leaves the edit control's memory with the view.
            SetWindowTextW(v->edits[ed], L"");
            DestroyWindow(v->edits[ed]);
        }
        if (v->saved[ed]) SecureZeroMemory(v->saved[ed], strlen(v->saved[ed]));
        free(v->saved[ed]); free(v->error[ed]); free(v->notice[ed]);
    }
    json_free(v->server); json_free(v->site);
    free(v->site_error);
    free(v);
}
