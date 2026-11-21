#include "screen_findings_internal.h"

static void findings_layout(Screen *base, Doc *doc);
static void findings_header(Screen *base, HeaderInfo *info);
static void findings_action(Screen *base, int action, intptr_t arg, POINT pt);
static void findings_timer(Screen *base, UINT id);
static void findings_visible(Screen *base, bool shown);
static void findings_refresh(Screen *base);
static void findings_activated(Screen *base, bool active);
static void findings_destroy(Screen *base);

static void findings_layout(Screen *base, Doc *doc) {
    FindingsScreen *s = (FindingsScreen *)base;
    doc_space(doc, px(14));
    int top = doc->y;
    ssh_approvals_layout(s->ssh, doc, 0, doc->width);
    if (doc->y == top) doc_text(doc, 0, doc->width, "Nothing is waiting. SSH approvals appear here for a device with a manage token.", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
    doc_space(doc, px(12));
}

static void findings_header(Screen *base, HeaderInfo *info) {
    (void)base;
    snprintf(info->title, sizeof info->title, "\xE2\x9A\x91 Findings");
    size_t n = ssh_approvals_badge();
    if (n) snprintf(info->subtitle, sizeof info->subtitle, "%zu SSH approval%s waiting", n, n == 1 ? "" : "s");
    else snprintf(info->subtitle, sizeof info->subtitle, "nothing is waiting");
}

static void findings_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    FindingsScreen *s = (FindingsScreen *)base;
    if (action >= SSH_APPROVE && action <= SSH_REFRESH) ssh_approvals_action(s->ssh, action, arg);
}

static void findings_timer(Screen *base, UINT id) { ssh_approvals_timer(((FindingsScreen *)base)->ssh, id); }

static void findings_visible(Screen *base, bool shown) { ssh_approvals_visible(((FindingsScreen *)base)->ssh, shown); }

static void findings_refresh(Screen *base) { ssh_approvals_refresh(((FindingsScreen *)base)->ssh); }

static void findings_activated(Screen *base, bool active) { if (active) ssh_approvals_visible(((FindingsScreen *)base)->ssh, true); }

static void findings_destroy(Screen *base) {
    FindingsScreen *s = (FindingsScreen *)base;
    ssh_approvals_free(s->ssh);
    screen_release(base);
}

static const ScreenVTable findings_vt = {
    .destroy = findings_destroy, .layout = findings_layout, .header = findings_header, .action = findings_action,
    .timer = findings_timer, .visible = findings_visible, .refresh = findings_refresh, .activated = findings_activated,
    .detachable = true,
};

Screen *findings_screen_new(void) {
    FindingsScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &findings_vt; s->base.id = xstrdup("findings");
    s->ssh = ssh_approvals_new(&s->base);
    return &s->base;
}
