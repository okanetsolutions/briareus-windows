// What ▶ Run serves, inside the app: the served address in an embedded browser that fills the pane under its header.
#include "screens.h"
#include "str.h"
#include "theme.h"
#include "webview.h"
#include <stdio.h>
#include <stdlib.h>

enum { ACT_RELOAD = 1, ACT_OPEN_BROWSER };

typedef struct { Screen base; char *url, *title; WebView *wv; } PreviewScreen;

static void preview_changed(void *ctx) {
    PreviewScreen *s = ctx;
    if (!s->base.pane) return;
    pane_header_changed(s->base.pane); pane_relayout(s->base.pane);
}
/// The address shown now: where the page went, else where it was opened.
static const char *current_url(PreviewScreen *s) { const char *u = s->wv ? webview_url(s->wv) : NULL; return u ? u : s->url; }

static void preview_layout(Screen *base, Doc *doc) {
    PreviewScreen *s = (PreviewScreen *)base;
    if (s->wv && webview_ready(s->wv)) return;
    const char *error = s->wv ? webview_error(s->wv) : NULL;
    doc_space(doc, px(16));
    doc_text(doc, 0, doc->width, error ? error : "Starting the browser\xE2\x80\xA6", FONT_BODY, error ? theme.danger : theme.muted, DT_WORDBREAK);
    if (!error) return;
    doc_space(doc, px(8));
    int i = doc_text(doc, 0, doc->width, "Open in your browser instead \xE2\x86\x97", FONT_BODY, theme.accent, DT_SINGLELINE);
    doc_item(doc, i)->action = ACT_OPEN_BROWSER; doc_item(doc, i)->hand = true;
}
static void preview_header(Screen *base, HeaderInfo *info) {
    PreviewScreen *s = (PreviewScreen *)base;
    const char *title = s->wv && !str_empty(webview_title(s->wv)) ? webview_title(s->wv) : s->title;
    snprintf(info->title, sizeof info->title, "%s", title ? title : "Run");
    snprintf(info->subtitle, sizeof info->subtitle, "%s", current_url(s));
    HeaderButton *r = &info->buttons[info->button_count++];
    r->glyph = 0xE72C; r->action = ACT_RELOAD; r->enabled = s->wv && webview_ready(s->wv); r->tip = "Reload the page";
    HeaderButton *o = &info->buttons[info->button_count++];
    o->glyph = 0xE8A7; o->action = ACT_OPEN_BROWSER; o->enabled = true; o->tip = "Open in your browser";
}
static void preview_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)arg; (void)pt;
    PreviewScreen *s = (PreviewScreen *)base;
    if (action == ACT_RELOAD && s->wv) webview_reload(s->wv);
    else if (action == ACT_OPEN_BROWSER) open_web_url(current_url(s));
}
static void preview_refresh(Screen *base) { PreviewScreen *s = (PreviewScreen *)base; if (s->wv) webview_reload(s->wv); }
static void preview_place(Screen *base, const RECT *content, int scroll_y) {
    (void)scroll_y;
    PreviewScreen *s = (PreviewScreen *)base;
    if (s->wv) webview_set_bounds(s->wv, content);
}
static void preview_visible(Screen *base, bool shown) {
    PreviewScreen *s = (PreviewScreen *)base;
    // The browser is a child of the pane, so it starts once the screen is in one.
    if (shown && !s->wv) s->wv = webview_new(pane_hwnd(base->pane), s->url, preview_changed, s);
    if (s->wv) webview_show(s->wv, shown);
    if (shown) pane_relayout(base->pane);
}
static void preview_destroy(Screen *base) {
    PreviewScreen *s = (PreviewScreen *)base;
    webview_free(s->wv);
    free(s->url); free(s->title);
    screen_release(base);
}
static const ScreenVTable preview_vt = {
    .destroy = preview_destroy, .layout = preview_layout, .header = preview_header, .action = preview_action,
    .place = preview_place, .visible = preview_visible, .refresh = preview_refresh,
};
Screen *preview_screen_new(const char *url, const char *title) {
    PreviewScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &preview_vt; s->base.id = xstrfmt("run:%s", url);
    s->url = xstrdup(url); s->title = title ? xstrdup(title) : NULL;
    return &s->base;
}
bool preview_open_served(Screen *from, const Json *result, const char *title) {
    const char *url = json_str(json_get(result, "url"));
    // Only while the screen that ran it is still in front: a Run that took minutes must not pull the user away.
    if (!safe_web_url(url) || !from->pane || pane_top(from->pane) != from) return false;
    app_push_detail(preview_screen_new(url, title));
    return true;
}
