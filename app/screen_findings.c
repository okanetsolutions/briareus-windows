#include "screen_findings_internal.h"

static void findings_layout(Screen *base, Doc *doc);
static void findings_header(Screen *base, HeaderInfo *info);
static void findings_action(Screen *base, int action, intptr_t arg, POINT pt);
static void findings_timer(Screen *base, UINT id);
static void findings_visible(Screen *base, bool shown);
static void findings_refresh(Screen *base);
static void findings_activated(Screen *base, bool active);
static void findings_destroy(Screen *base);

/// Why the screen can show nothing for this device, or NULL when SSH approvals are offered.
static const char *unavailable_reason(void) {
    if (!store_lists("ssh_requests")) return "This server does not offer SSH approvals.";
    if (!ssh_approvals_offered()) return "SSH approvals need a Manage token; this device's token cannot list them.";
    return NULL;
}

static void findings_layout(Screen *base, Doc *doc) {
    FindingsScreen *s = (FindingsScreen *)base;
    doc_space(doc, px(14));
    if (ssh_approvals_loading(s->ssh)) doc_loading(doc, 0, doc->width, "Loading SSH approvals\xE2\x80\xA6");
    else ssh_approvals_layout(s->ssh, doc, 0, doc->width);
    const char *why = unavailable_reason();
    if (why) doc_text(doc, 0, doc->width, why, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
    doc_space(doc, px(12));
}

static void findings_header(Screen *base, HeaderInfo *info) {
    (void)base;
    snprintf(info->title, sizeof info->title, "\xE2\x9A\x91 Findings");
    // Blank until a read lands: before it, or after a failed one, the count is not known.
    size_t n;
    if (!ssh_approvals_counted(&n)) info->subtitle[0] = 0;
    else if (n) snprintf(info->subtitle, sizeof info->subtitle, "%zu SSH approval%s waiting", n, n == 1 ? "" : "s");
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
