// WhatsApp Web and Slack in the detail pane, from the sidebar strip's buttons. One browser per app serves every visit:
// leaving the screen hides it instead of closing it, so the chats stay loaded and coming back is instant. The WebView2
// profile is kept on disk, so the QR code is scanned, or the workspace signed in to, once. Either moves into a window of
// its own, and its browser goes with it.
#include "screens.h"
#include "str.h"
#include "webview.h"
#include <stdio.h>
#include <stdlib.h>

enum { ACT_RELOAD = 1000, ACT_BROWSER };

typedef struct { const char *id, *name, *host, *url, *reload_tip; } WebAppInfo;
static const WebAppInfo APPS[WEB_APP_COUNT] = {
    [WEB_APP_WHATSAPP] = { "whatsapp", "WhatsApp", "web.whatsapp.com", "https://web.whatsapp.com/", "Reload WhatsApp" },
    [WEB_APP_SLACK] = { "slack", "Slack", "app.slack.com", "https://app.slack.com/client", "Reload Slack" },
};

typedef struct { Screen base; WebApp app; bool shown; } WebAppScreen;

static WebView *g_web[WEB_APP_COUNT];            // live as long as the detail pane, their parent while no window holds them
static WebAppScreen *g_current[WEB_APP_COUNT];   // the screen showing each, which hears its changes

static void web_changed(void *ctx) {
    WebAppScreen *s = g_current[(WebApp)(intptr_t)ctx];
    if (!s || !s->base.pane) return;
    pane_relayout(s->base.pane);
    pane_header_changed(s->base.pane);
}

static void web_app_destroy(Screen *base) {
    WebAppScreen *s = (WebAppScreen *)base;
    if (g_current[s->app] == s) {
        g_current[s->app] = NULL;
        // A window of its own closing hands the browser back to the detail pane, which outlives it.
        if (g_web[s->app]) { webview_show(g_web[s->app], false); if (app_detail_pane()) webview_set_parent(g_web[s->app], pane_hwnd(app_detail_pane())); }
    }
    screen_release(base);
}

static void web_app_layout(Screen *base, Doc *doc) {
    WebAppScreen *s = (WebAppScreen *)base;
    WebView *web = g_web[s->app];
    if (web && webview_ready(web)) return;
    int w = doc->width;
    const char *error = web ? webview_error(web) : NULL;
    char opening[64]; snprintf(opening, sizeof opening, "Opening %s\xE2\x80\xA6", APPS[s->app].name);
    doc_space(doc, px(18));
    doc_text(doc, px(4), w - px(8), error ? error : opening, FONT_BODY, error ? theme.danger : theme.muted, DT_WORDBREAK);
    if (error) {
        doc_space(doc, px(8));
        int i = doc_text(doc, px(4), w - px(8), "Open in your browser instead \xE2\x86\x97", FONT_BODY, theme.accent, DT_SINGLELINE);
        doc_item(doc, i)->action = ACT_BROWSER; doc_item(doc, i)->hand = true;
    }
}

static void web_app_header(Screen *base, HeaderInfo *info) {
    WebAppScreen *s = (WebAppScreen *)base;
    const WebAppInfo *a = &APPS[s->app];
    snprintf(info->title, sizeof info->title, "%s", a->name);
    snprintf(info->subtitle, sizeof info->subtitle, "%s", a->host);
    HeaderButton *r = &info->buttons[info->button_count++];
    r->glyph = 0xE72C; r->action = ACT_RELOAD; r->enabled = g_web[s->app] && webview_ready(g_web[s->app]);
    r->tip = a->reload_tip;
    HeaderButton *o = &info->buttons[info->button_count++];
    o->glyph = 0xE774; o->action = ACT_BROWSER; o->enabled = true; o->tip = "Open in your browser";
}

static void web_app_action(Screen *base, int action, intptr_t arg, POINT pt) {
    WebAppScreen *s = (WebAppScreen *)base;
    (void)arg; (void)pt;
    if (action == ACT_RELOAD && g_web[s->app]) webview_reload(g_web[s->app]);
    else if (action == ACT_BROWSER) open_web_url(APPS[s->app].url);
}

/// The browser fills the pane under its header, edge to edge.
static void web_app_place(Screen *base, const RECT *content, int scroll_y) {
    WebAppScreen *s = (WebAppScreen *)base;
    (void)scroll_y;
    if (!s->shown || g_current[s->app] != s) return;
    WebView **web = &g_web[s->app];
    if (!*web) *web = webview_new(pane_hwnd(base->pane), APPS[s->app].url, NULL, web_changed, (void *)(intptr_t)s->app);
    else webview_set_parent(*web, pane_hwnd(base->pane));
    bool on = webview_ready(*web) && content->bottom > content->top;
    if (on) webview_set_bounds(*web, content);
    webview_show(*web, on);
}

static void web_app_visible(Screen *base, bool shown) {
    WebAppScreen *s = (WebAppScreen *)base;
    WebView **web = &g_web[s->app];
    s->shown = shown;
    if (shown) {
        // A browser that failed to start is tried again on the next visit.
        if (*web && webview_error(*web)) { webview_free(*web); *web = NULL; }
        // Moved to its new pane now, not at the next paint: a window docking back is destroyed before that paint.
        if (*web) webview_set_parent(*web, pane_hwnd(base->pane));
        g_current[s->app] = s; pane_relayout(base->pane);
    }
    else if (*web && g_current[s->app] == s) webview_show(*web, false);
}

static void web_app_refresh(Screen *base) {
    WebAppScreen *s = (WebAppScreen *)base;
    if (g_web[s->app]) webview_reload(g_web[s->app]);
}

static const ScreenVTable web_app_vt = {
    .destroy = web_app_destroy, .layout = web_app_layout, .header = web_app_header, .action = web_app_action,
    .place = web_app_place, .visible = web_app_visible, .refresh = web_app_refresh, .detachable = true,
};
Screen *web_app_screen_new(WebApp app) {
    WebAppScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &web_app_vt; s->base.id = xstrdup(APPS[app].id); s->app = app;
    return &s->base;
}
