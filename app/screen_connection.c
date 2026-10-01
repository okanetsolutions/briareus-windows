// The connection: the paired dashboard, the projects the token permits, and how to end it.
#include "resource.h"
#include "screens.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ACT_REVOKE = 1000, ACT_FORGET, ACT_COPY_SERVER };

typedef struct {
    Screen base;
    bool busy;
    char *error;
} ConnectionScreen;

/// The screen a revocation is running for, so its answer is dropped if the screen went away first.
static ConnectionScreen *g_revoking;

static void connection_destroy(Screen *base) {
    ConnectionScreen *s = (ConnectionScreen *)base;
    if (g_revoking == s) g_revoking = NULL;
    free(s->error);
    screen_release(base);
}

typedef struct { char *repo; } RepoData;
static void repo_free(void *p) { RepoData *d = p; free(d->repo); free(d); }
static void paint_repo(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    RepoData *d = it->data;
    int size = px(26);
    draw_monogram(hdc, rc->left, rc->top + (rc->bottom - rc->top - size) / 2, size, d->repo);
    RECT t = { rc->left + size + px(10), rc->top, rc->right, rc->bottom };
    draw_text(hdc, d->repo, &t, FONT_BODY, theme.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void paint_seal(Doc *doc, Item *it, HDC hdc, const RECT *rc) { draw_glyph(hdc, 0xE930, rc, FONT_ICON, theme.success); }

static int card_begin(Doc *doc, int w) { int b = doc_box_begin(doc, 0, w, px(12), theme.elevated, theme.border, px(12)); doc_item(doc, b)->hover_fill = false; return b; }
static void row_gap(Doc *doc, int w) { doc_space(doc, px(7)); doc_rule(doc, px(12), w - px(24)); doc_space(doc, px(7)); }

static void connection_layout(Screen *base, Doc *doc) {
    ConnectionScreen *s = (ConnectionScreen *)base;
    int w = doc->width, ix = px(12), iw = w - px(24);
    doc_section(doc, 0, w, "Connected dashboard");
    int box = card_begin(doc, w);
    {
        int y = doc->y;
        RECT g = { ix, y, ix + px(20), y + px(22) };
        doc_add(doc, &g, paint_seal);
        int ti = doc_text(doc, ix + px(28), iw - px(28), g_store.server ? g_store.server : "", FONT_BODY_MEDIUM, theme.text, DT_WORDBREAK);
        doc_item(doc, ti)->action = ACT_COPY_SERVER;
        if (doc->y < y + px(22)) doc->y = y + px(22);
    }
    if (g_store.has_device) {
        const Device *d = &g_store.device;
        row_gap(doc, w);
        doc_labeled(doc, ix, iw, "Device", d->label, theme.secondary);
        row_gap(doc, w);
        doc_labeled(doc, ix, iw, "Access", str_eq(d->permission, "admin") ? "Admin" : device_can_manage(d) ? "Manage" : "Read only", theme.secondary);
        row_gap(doc, w);
        char *expires = format_date_abbrev(device_expiry(d));
        doc_labeled(doc, ix, iw, "Expires", expires, theme.secondary);
        free(expires);
    }
    doc_box_end(doc, box, px(12));

    doc_section(doc, 0, w, "Permitted projects");
    box = card_begin(doc, w);
    size_t repos = g_store.has_device ? g_store.device.repo_count : 0;
    for (size_t i = 0; i < repos; i++) {
        if (i) row_gap(doc, w);
        RepoData *d = xcalloc(1, sizeof *d); d->repo = xstrdup(g_store.device.repos[i]);
        doc_custom(doc, ix, iw, px(30), paint_repo, d, repo_free, 0, 0);
    }
    if (!repos) doc_text(doc, ix, iw, "No projects are permitted for this token.", FONT_CALLOUT, theme.secondary, DT_WORDBREAK);
    doc_box_end(doc, box, px(12));

    doc_space(doc, px(18));
    box = card_begin(doc, w);
    doc_button(doc, ix, iw, s->busy ? "Revoking\xE2\x80\xA6" : "Revoke token and disconnect", BUTTON_DESTRUCTIVE, ACT_REVOKE, 0, !s->busy);
    doc_space(doc, px(8));
    doc_button(doc, ix, iw, "Forget this connection", BUTTON_DESTRUCTIVE, ACT_FORGET, 0, !s->busy);
    doc_box_end(doc, box, px(12));
    doc_space(doc, px(8));
    doc_text(doc, px(4), w - px(8), "Revoking disables this token on the server. Forgetting removes it and the saved conversations from this computer only; revoke it later in web Settings. Neither action stops running agents.",
             FONT_CAPTION, theme.secondary, DT_WORDBREAK);
    if (s->error) { doc_space(doc, px(12)); doc_notice_box(doc, 0, w, s->error); }
    doc_space(doc, px(20));
    doc_text(doc, px(4), w - px(8), "Briareus for Windows \xC2\xB7 " APP_VERSION_STRING, FONT_FOOTNOTE, theme.tertiary, DT_LEFT | DT_SINGLELINE);
    doc_space(doc, px(16));
}

static void connection_header(Screen *base, HeaderInfo *info) { (void)base; snprintf(info->title, sizeof info->title, "Connection"); }

static void revoke_done(void *ctx, const char *error) {
    ConnectionScreen *s = ctx;
    if (g_revoking != s) return;
    g_revoking = NULL;
    s->busy = false;
    // Without an error the store has forgotten the connection and the pairing screen replaces everything.
    if (error) { set_string(&s->error, error); pane_relayout(s->base.pane); }
}
static void connection_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)arg; (void)pt;
    ConnectionScreen *s = (ConnectionScreen *)base;
    switch (action) {
    case ACT_COPY_SERVER: copy_to_clipboard(pane_hwnd(base->pane), g_store.server); break;
    case ACT_REVOKE:
        // Named both ways: a token meant to be revoked must never be merely forgotten.
        if (s->busy || !app_confirm("Revoke this device token?", "The token stops working on the server for every screen that uses it.", "Revoke", true)) break;
        s->busy = true; set_string(&s->error, NULL); g_revoking = s;
        pane_relayout(base->pane);
        store_revoke(revoke_done, s);
        break;
    case ACT_FORGET:
        if (s->busy || !app_confirm("Forget this connection?", "The token and the saved conversations are removed from this computer only.", "Forget", true)) break;
        store_forget();
        break;
    }
}
static const ScreenVTable connection_vt = { .destroy = connection_destroy, .layout = connection_layout, .header = connection_header, .action = connection_action };
Screen *connection_screen_new(void) {
    ConnectionScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &connection_vt; s->base.id = xstrdup("connection");
    return &s->base;
}
