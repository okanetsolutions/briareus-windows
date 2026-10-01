// WhatsApp Web in the detail pane, from the sidebar strip's WhatsApp button. One browser serves every visit: leaving the
// screen hides it instead of closing it, so the chats stay loaded and coming back is instant. The WebView2 profile is
// kept on disk, so the QR code is scanned once.
#include "screens.h"
#include "str.h"
#include "webview.h"
#include <stdio.h>
#include <stdlib.h>

#define WHATSAPP_URL "https://web.whatsapp.com/"
enum { ACT_RELOAD = 1000, ACT_BROWSER };

typedef struct { Screen base; bool shown; } WhatsAppScreen;

static WebView *g_web;               // lives as long as the detail pane, its parent
static WhatsAppScreen *g_current;    // the screen showing it, which hears its changes

static void web_changed(void *ctx) {
    (void)ctx;
    if (!g_current || !g_current->base.pane) return;
    pane_relayout(g_current->base.pane);
    pane_header_changed(g_current->base.pane);
}

static void whatsapp_destroy(Screen *base) {
    WhatsAppScreen *s = (WhatsAppScreen *)base;
    if (g_current == s) { g_current = NULL; if (g_web) webview_show(g_web, false); }
    screen_release(base);
}

static void whatsapp_layout(Screen *base, Doc *doc) {
    (void)base;
    if (g_web && webview_ready(g_web)) return;
    int w = doc->width;
    const char *error = g_web ? webview_error(g_web) : NULL;
    doc_space(doc, px(18));
    doc_text(doc, px(4), w - px(8), error ? error : "Opening WhatsApp\xE2\x80\xA6", FONT_BODY, error ? theme.danger : theme.muted, DT_WORDBREAK);
    if (error) {
        doc_space(doc, px(8));
        int i = doc_text(doc, px(4), w - px(8), "Open in your browser instead \xE2\x86\x97", FONT_BODY, theme.accent, DT_SINGLELINE);
        doc_item(doc, i)->action = ACT_BROWSER; doc_item(doc, i)->hand = true;
    }
}

static void whatsapp_header(Screen *base, HeaderInfo *info) {
    (void)base;
    snprintf(info->title, sizeof info->title, "WhatsApp");
    snprintf(info->subtitle, sizeof info->subtitle, "web.whatsapp.com");
    HeaderButton *r = &info->buttons[info->button_count++];
    r->glyph = 0xE72C; r->action = ACT_RELOAD; r->enabled = g_web && webview_ready(g_web); r->tip = "Reload WhatsApp";
    HeaderButton *o = &info->buttons[info->button_count++];
    o->glyph = 0xE8A7; o->action = ACT_BROWSER; o->enabled = true; o->tip = "Open in your browser";
}

static void whatsapp_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)base; (void)arg; (void)pt;
    if (action == ACT_RELOAD && g_web) webview_reload(g_web);
    else if (action == ACT_BROWSER) open_web_url(WHATSAPP_URL);
}

/// The browser fills the pane under its header, edge to edge.
static void whatsapp_place(Screen *base, const RECT *content, int scroll_y) {
    WhatsAppScreen *s = (WhatsAppScreen *)base;
    (void)scroll_y;
    if (!s->shown || g_current != s) return;
    if (!g_web) g_web = webview_new(pane_hwnd(base->pane), WHATSAPP_URL, NULL, web_changed, NULL);
    bool on = webview_ready(g_web) && content->bottom > content->top;
    if (on) webview_set_bounds(g_web, content);
    webview_show(g_web, on);
}

static void whatsapp_visible(Screen *base, bool shown) {
    WhatsAppScreen *s = (WhatsAppScreen *)base;
    s->shown = shown;
    if (shown) {
        // A browser that failed to start is tried again on the next visit.
        if (g_web && webview_error(g_web)) { webview_free(g_web); g_web = NULL; }
        g_current = s; pane_relayout(base->pane);
    }
    else if (g_web && g_current == s) webview_show(g_web, false);
}

static void whatsapp_refresh(Screen *base) { (void)base; if (g_web) webview_reload(g_web); }

static const ScreenVTable whatsapp_vt = {
    .destroy = whatsapp_destroy, .layout = whatsapp_layout, .header = whatsapp_header, .action = whatsapp_action,
    .place = whatsapp_place, .visible = whatsapp_visible, .refresh = whatsapp_refresh,
};
Screen *whatsapp_screen_new(void) {
    WhatsAppScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &whatsapp_vt; s->base.id = xstrdup("whatsapp");
    return &s->base;
}
