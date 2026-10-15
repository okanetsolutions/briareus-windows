#include "screen_mail_inbox_internal.h"

static char *sync_label(const Inbox *s);
static int unread_total(const Inbox *s);
static char *mailbox_caption(const Inbox *s);
static char *folder_caption(const Inbox *s);
static const char *group_caption(const Inbox *s);
static int layout_filters(Inbox *s, Doc *doc, int x, int w, int fy, int fh, int kind_w, Anchors *anchors);

void screen_mail_inbox_initials_of(const char *name, char out[3]) {
    out[0] = out[1] = out[2] = 0;
    int n = 0; bool start = true;
    for (const unsigned char *p = (const unsigned char *)(name ? name : ""); *p && n < 2; p++) {
        if (strchr(" <@.-_", *p)) { start = true; continue; }
        if (start && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) {
            out[n++] = (char)(*p >= 'a' ? *p - 32 : *p);
            start = false;
        }
    }
    if (!n) out[0] = '?';
}

void screen_mail_inbox_sender_parts(const char *sender, char **name, char **email) {
    const char *s = sender ? sender : "";
    const char *lt = strchr(s, '<'), *gt = lt ? strchr(lt, '>') : NULL;
    if (lt && gt && gt > lt + 1) {
        size_t nlen = (size_t)(lt - s);
        while (nlen && (s[nlen - 1] == ' ' || s[nlen - 1] == '\t')) nlen--;
        *email = xstrndup(lt + 1, (size_t)(gt - lt - 1));
        *name = nlen ? xstrndup(s, nlen) : xstrdup(*email);
    } else {
        *name = xstrdup(*s ? s : "Unknown sender");
        *email = xstrdup(s);
    }
    if (str_empty(*name)) { free(*name); *name = xstrdup(str_empty(*email) ? "Unknown sender" : *email); }
}

char *screen_mail_inbox_account_badge(const MailAccount *a) {
    const char *label = a && !str_empty(a->label) ? a->label : NULL;
    if (label && strlen(label) <= 4 && !strchr(label, ' ')) {
        char *u = xstrdup(label);
        for (char *p = u; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
        return u;
    }
    char ini[3]; screen_mail_inbox_initials_of(label ? label : a && a->email ? a->email : "", ini);
    return xstrdup(ini);
}

char *screen_mail_inbox_list_age(double ms) {
    if (ms <= 0) return xstrdup("");
    char *rel = format_relative((time_t)(ms / 1000));
    size_t n = strlen(rel);
    if (n > 4 && strcmp(rel + n - 4, " ago") == 0) rel[n - 4] = 0;
    return rel;
}

const char *screen_mail_inbox_subject_of(const MailMessage *m) { return str_empty(m->subject) ? "(No subject)" : m->subject; }

bool screen_mail_inbox_row_open(const Inbox *s, const MailMessage *m) {
    return s->selected_id && s->selected_account == m->account_id && str_eq(s->selected_id, m->id);
}

static char *sync_label(const Inbox *s) {
    double latest = 0; bool syncing = false, any = false;
    for (size_t i = 0; i < s->accounts.count; i++) {
        const MailAccount *a = &s->accounts.accounts[i];
        if (s->filter.account && a->id != s->filter.account) continue;
        any = true;
        if (a->syncing) syncing = true;
        if (a->last_sync_at > latest) latest = a->last_sync_at;
    }
    if (!any) return xstrdup(s->accounts_loaded ? "Not synced" : "");
    if (syncing && latest <= 0) return xstrdup("Syncing...");
    if (latest <= 0) return xstrdup("Not synced");
    double sec = difftime(time(NULL), (time_t)(latest / 1000));
    if (sec < 45) return xstrdup("Synced just now");
    if (sec < 3600) { int mins = (int)(sec / 60.0); if (mins < 1) mins = 1; return xstrfmt("Synced %d min ago", mins); }
    char *rel = format_relative((time_t)(latest / 1000));
    char *line = xstrfmt("Synced %s", rel); free(rel); return line;
}

static int unread_total(const Inbox *s) {
    int n = 0;
    for (size_t i = 0; i < s->accounts.count; i++) {
        const MailAccount *a = &s->accounts.accounts[i];
        if (s->filter.account && a->id != s->filter.account) continue;
        if (a->unread > 0) n += a->unread;
    }
    return n;
}

static char *mailbox_caption(const Inbox *s) {
    if (!s->filter.account) return xstrdup("All mailboxes");
    const MailAccount *a = mail_account_find(&s->accounts, s->filter.account);
    if (!a) return xstrdup("All mailboxes");
    return xstrdup(!str_empty(a->label) ? a->label : a->email ? a->email : "Mailbox");
}

static char *folder_caption(const Inbox *s) {
    if (!str_empty(s->filter.label)) return xstrfmt("Folder: %s", s->filter.label);
    if (s->filter.inbox == 1) return xstrdup("Folder: Inbox");
    if (s->filter.inbox == 0) return xstrdup("Folder: Outside");
    return xstrdup("Folder: Any");
}

static const char *group_caption(const Inbox *s) {
    if (!str_empty(s->filter.thread)) return "Thread filter";
    return s->group == GROUP_THREADS ? "Group threads and duplicates" : "Separate messages";
}

const char *screen_mail_inbox_older_label(const Inbox *s) {
    if (!s->messages.count || !s->messages.next_cursor) return "Load older messages";
    double oldest = 0;
    for (size_t i = 0; i < s->messages.count; i++) {
        double t = s->messages.messages[i].received_at;
        if (t > 0 && (oldest <= 0 || t < oldest)) oldest = t;
    }
    time_t now = time(NULL);
    struct tm today = *localtime(&now);
    int screen_mail_inbox_key = (today.tm_year + 1900) * 10000 + (today.tm_mon + 1) * 100 + today.tm_mday;
    return screen_mail_inbox_day_key(oldest) == screen_mail_inbox_key ? "Load yesterday" : "Load older messages";
}

// Unread, Starred, Folder, Grouping and Reset. They share the kind-chip line when the run fits, and wrap inside `w` when it does not.
static int layout_filters(Inbox *s, Doc *doc, int x, int w, int fy, int fh, int kind_w, Anchors *anchors) {
    char *folder = folder_caption(s);
    const char *group = group_caption(s);
    struct {
        const char *text, *shown;
        bool on, accent, caret;
        int action;
        intptr_t arg;
        RECT *anchor;
        int max_w;
    } specs[5] = {
        { "Unread only", "Unread only", s->filter.unread == 1, false, false, ACT_UNREAD, 0, NULL, 0 },
        { "Starred", "Starred", s->filter.starred == 1, false, false, ACT_STARRED, 0, NULL, 0 },
        { "Folder", folder, s->menu == MENU_FOLDER || s->filter.inbox >= 0 || !str_empty(s->filter.label), false, true, ACT_MENU, MENU_FOLDER, &anchors->folder, px(220) },
        { "Grouping", group, s->menu == MENU_GROUP, false, true, ACT_MENU, MENU_GROUP, &anchors->group, 0 },
        { "Reset filters", "Reset", false, true, false, ACT_RESET, 0, NULL, 0 },
    };
    enum { N = 5 };
    int gap = px(8), widths[N], filters = 0;
    for (int i = 0; i < N; i++) {
        int span = screen_mail_inbox_chip_span(doc, specs[i].shown, specs[i].accent, specs[i].caret);
        if (specs[i].max_w > 0 && span > specs[i].max_w) span = specs[i].max_w;
        if (span > w) span = w;
        widths[i] = span;
        filters += span;
    }
    filters += gap * (N - 1);
    bool wrap = filters > w;
    int fx = x + w - filters;
    bool same = !wrap && fx >= x + kind_w + px(16);
    int cy = same ? fy : fy + fh + gap;
    int cx = wrap ? x : fx;
    if (cx < x) cx = x;
    for (int i = 0; i < N; i++) {
        if (wrap && cx > x && cx + widths[i] > x + w) { cx = x; cy += fh + gap; }
        int tw = screen_mail_inbox_add_chip(doc, cx, cy, fh, specs[i].text, specs[i].shown, specs[i].on, specs[i].accent, specs[i].caret, specs[i].action, specs[i].arg, widths[i]);
        if (specs[i].anchor) *specs[i].anchor = (RECT){ cx, cy, cx + tw, cy + fh };
        cx += tw + gap;
    }
    free(folder);
    return cy;
}

void screen_mail_inbox_layout_chrome(Inbox *s, Doc *doc, int x, int w, const MailGroup *rows, size_t nrow, bool available, Anchors *anchors) {
    int people = 0, notes = 0;
    for (size_t i = 0; i < nrow; i++) { if (rows[i].notification) notes++; else people++; }
    int h = px(34), y = doc->y, gap = px(8);
    int refresh_w = h, sync_w = px(156), box_w = px(210);
    bool narrow = w < px(620);
    if (narrow) { box_w = w - refresh_w - gap; sync_w = 0; }
    int search_x = x + box_w + gap;
    int search_r = x + w - sync_w - refresh_w - gap * 2;
    if (!narrow && search_r < search_x + px(120)) { sync_w = px(120); search_r = x + w - sync_w - refresh_w - gap * 2; }
    if (!narrow && search_r < search_x + px(40)) search_r = search_x + px(40);
    char *caption = mailbox_caption(s);
    MailBtn *mb = xcalloc(1, sizeof *mb);
    mb->shown = caption; mb->unread = unread_total(s); mb->on = s->menu == MENU_MAILBOX;
    RECT mrc = { x, y, x + box_w, y + h };
    anchors->mailbox = mrc;
    Item *mit = doc_item(doc, doc_add(doc, &mrc, screen_mail_inbox_paint_mailbtn));
    mit->data = mb; mit->free_data = screen_mail_inbox_mailbtn_free; mit->text = xstrdup("Mailbox");
    mit->action = ACT_MENU; mit->arg = MENU_MAILBOX; mit->hand = true;
    const char *placeholder = s->filter.account ? "Search this mailbox" : "Search sender, subject or snippet";
    SearchData *sd = xcalloc(1, sizeof *sd);
    sd->shown = xstrdup(s->filter.q ? s->filter.q : placeholder); sd->placeholder = !s->filter.q;
    RECT src = narrow ? (RECT){ x, y + h + gap, x + w, y + h * 2 + gap }
        : (RECT){ search_x, y, search_r, y + h };
    Item *sit = doc_item(doc, doc_add(doc, &src, screen_mail_inbox_paint_search));
    sit->data = sd; sit->free_data = screen_mail_inbox_search_free; sit->text = xstrdup("Search");
    sit->action = ACT_Q; sit->hand = true; sit->tip = xstrdup("Search sender, subject or snippet");
    char *sync = sync_label(s);
    RECT sy = { search_r + gap, y, x + w - refresh_w - gap, y + h };
    if (!narrow) doc_text_at(doc, &sy, sync, FONT_CAPTION, theme.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    free(sync);
    RECT rr = { x + w - refresh_w, y, x + w, y + h };
    screen_mail_inbox_add_icon(doc, &rr, 0xE72C, false, !available, "Refresh", available ? ACT_SYNC : 0, "Refresh mail");
    char *pc = xstrfmt("People %d", people), *nc = xstrfmt("Notifications %d", notes), *ac = xstrfmt("All %d", people + notes);
    int fh = px(30), fy = y + h + px(10) + (narrow ? h + gap : 0);
    int widths[3];
    const char *caps[3] = { pc, nc, ac };
    const char *names[3] = { "People", "Notifications", "All" };
    int total = 0;
    for (int i = 0; i < 3; i++) { widths[i] = screen_mail_inbox_chip_span(doc, caps[i], false, false); total += widths[i]; }
    int sx = x;
    for (int i = 0; i < 3; i++) {
        RECT rc = { sx, fy, sx + widths[i], fy + fh };
        ChipData *d = xcalloc(1, sizeof *d);
        d->shown = xstrdup(caps[i]); d->on = s->kind == i;
        Item *it = doc_item(doc, doc_add(doc, &rc, screen_mail_inbox_paint_chip));
        it->data = d; it->free_data = screen_mail_inbox_chip_free; it->text = xstrdup(names[i]);
        it->action = ACT_KIND; it->arg = i; it->hand = true;
        sx += widths[i] + px(4);
    }
    free(pc); free(nc); free(ac);
    int filter_y = layout_filters(s, doc, x, w, fy, fh, total + px(8), anchors);
    doc->y = filter_y + fh + px(12);
}

int screen_mail_inbox_layout_menu(Inbox *s, Doc *doc, const Anchors *anchors) {
    if (!s->menu) return s->chrome_bottom;
    const RECT *anchor = s->menu == MENU_MAILBOX ? &anchors->mailbox : s->menu == MENU_FOLDER ? &anchors->folder : &anchors->group;
    int mw = px(280), x = anchor->left;
    if (x + mw > doc->width - px(8)) x = doc->width - px(8) - mw;
    if (x < px(8)) x = px(8);
    // Menu rows are scrolling items, after the pin. Their document y has to include the scroll offset or they
    // paint and hit above the pinned chrome. The page keeps the unshifted height; a scroll change dismisses the menu.
    int scroll = 0;
    if (s->base.pane) { scroll = pane_scroll_y(s->base.pane); if (scroll < 0) scroll = 0; }
    int top = s->chrome_bottom + scroll, y = top + px(6);
    doc->y = top;
    int box = doc_box_begin(doc, x, mw, 0, theme.raise, theme.line, px(8));
    if (s->menu == MENU_MAILBOX) {
        y += screen_mail_inbox_menu_row(doc, x + px(4), y, mw - px(8), "All mailboxes", s->filter.account == 0, ACT_ACCOUNT, 0);
        for (size_t i = 0; i < s->accounts.count; i++) {
            const MailAccount *a = &s->accounts.accounts[i];
            bool readable = mail_account_readable(&s->accounts, a->id);
            const char *name = !str_empty(a->label) ? a->label : a->email ? a->email : "Mailbox";
            char *shown = readable ? xstrdup(a->email ? a->email : name) : xstrfmt("%s · sign in", a->email ? a->email : name);
            y += screen_mail_inbox_menu_row(doc, x + px(4), y, mw - px(8), shown, a->id == s->filter.account, readable ? ACT_ACCOUNT : 0, a->id);
            free(shown);
        }
    } else if (s->menu == MENU_FOLDER) {
        y += screen_mail_inbox_menu_row(doc, x + px(4), y, mw - px(8), "Any", s->filter.inbox < 0 && str_empty(s->filter.label), ACT_INBOX, -1);
        y += screen_mail_inbox_menu_row(doc, x + px(4), y, mw - px(8), "Inbox", s->filter.inbox == 1 && str_empty(s->filter.label), ACT_INBOX, 1);
        y += screen_mail_inbox_menu_row(doc, x + px(4), y, mw - px(8), "Outside inbox", s->filter.inbox == 0 && str_empty(s->filter.label), ACT_INBOX, 0);
        y += screen_mail_inbox_menu_row(doc, x + px(4), y, mw - px(8), "Exact label...", !str_empty(s->filter.label), ACT_LABEL, 0);
    } else {
        // The thread id is a server filter and does not change group. Check the group the rows actually use.
        y += screen_mail_inbox_menu_row(doc, x + px(4), y, mw - px(8), "Group threads and duplicates", s->group == GROUP_THREADS, ACT_GROUP, GROUP_THREADS);
        y += screen_mail_inbox_menu_row(doc, x + px(4), y, mw - px(8), "Separate messages", s->group == GROUP_EACH, ACT_GROUP, GROUP_EACH);
        y += screen_mail_inbox_menu_row(doc, x + px(4), y, mw - px(8), "Exact thread...", !str_empty(s->filter.thread), ACT_THREAD, 0);
    }
    y += px(6);
    doc->y = y;
    doc_box_end(doc, box, 0);
    doc->y = y - scroll;
    return doc->y;
}
