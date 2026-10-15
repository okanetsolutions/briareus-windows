#include "screen_mail_inbox_internal.h"

static char *day_title(int screen_mail_inbox_key);
static bool convo_open(const Inbox *s, const MailGroup *c);
static void layout_day(Doc *doc, int x, int w, int day, int convos, int messages);
static void layout_row(Inbox *s, Doc *doc, int x, int w, const MailGroup *c);

int screen_mail_inbox_day_key(double ms) {
    if (ms <= 0) return 0;
    time_t when = (time_t)(ms / 1000);
    // A finite receivedAt past year 3000 still parses. localtime returns NULL for it, and copying that used to fault every layout.
    struct tm *t = localtime(&when);
    if (!t) return 0;
    return (t->tm_year + 1900) * 10000 + (t->tm_mon + 1) * 100 + t->tm_mday;
}

static char *day_title(int screen_mail_inbox_key) {
    if (!screen_mail_inbox_key) return xstrdup("UNDATED");
    time_t now = time(NULL);
    struct tm t = *localtime(&now);
    int today = (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
    time_t back = now - 86400;
    struct tm y = *localtime(&back);
    int yesterday = (y.tm_year + 1900) * 10000 + (y.tm_mon + 1) * 100 + y.tm_mday;
    if (screen_mail_inbox_key == today) return xstrdup("TODAY");
    if (screen_mail_inbox_key == yesterday) return xstrdup("YESTERDAY");
    static const char *mon[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    int month = (screen_mail_inbox_key / 100) % 100, day = screen_mail_inbox_key % 100;
    if (month < 1 || month > 12) return xstrdup("UNDATED");
    return xstrfmt("%s %d", mon[month - 1], day);
}

bool screen_mail_inbox_convo_shown(const Inbox *s, const MailGroup *c) {
    if (s->kind == KIND_ALL) return true;
    if (s->kind == KIND_NOTES) return c->notification;
    return !c->notification;
}

static bool convo_open(const Inbox *s, const MailGroup *c) {
    if (!s->selected_id) return false;
    for (size_t i = 0; i < c->count; i++) {
        const MailMessage *m = &s->messages.messages[c->members[i]];
        if (m->account_id == s->selected_account && str_eq(m->id, s->selected_id)) return true;
    }
    return false;
}

int screen_mail_inbox_convo_primary_for(const Inbox *s, int message) {
    MailGroups groups = mail_groups_build(&s->messages, s->group == GROUP_THREADS);
    MailGroup *rows = groups.groups; size_t n = groups.count;
    int primary = message;
    for (size_t i = 0; i < n; i++) for (size_t k = 0; k < rows[i].count; k++)
        if (rows[i].members[k] == message) primary = rows[i].primary;
    mail_groups_free(&groups);
    return primary;
}

const MailGroup *screen_mail_inbox_convo_for_selection(const Inbox *s, const MailGroup *rows, size_t n) {
    for (size_t i = 0; i < n; i++) if (convo_open(s, &rows[i])) return &rows[i];
    return NULL;
}

static void layout_day(Doc *doc, int x, int w, int day, int convos, int messages) {
    char *title = day_title(day);
    char *meta = xstrfmt("%d conversation%s, %d message%s", convos, convos == 1 ? "" : "s", messages, messages == 1 ? "" : "s");
    int y = doc->y, h = font_height(doc->cv, FONT_CAPTION);
    int tw = text_width(doc->cv, title, FONT_CAPTION_SEMIBOLD) + px(10);
    RECT tr = { x + px(8), y, x + px(8) + tw, y + h };
    doc_text_at(doc, &tr, title, FONT_CAPTION_SEMIBOLD, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT mr = { tr.right, y, x + w, y + h };
    doc_text_at(doc, &mr, meta, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    free(title); free(meta);
    doc->y = y + h + px(6);
}

static void layout_row(Inbox *s, Doc *doc, int x, int w, const MailGroup *c) {
    const MailMessage *m = &s->messages.messages[c->primary];
    bool open = convo_open(s, c), focused = s->focus_message == c->primary;
    COLORREF fill = open ? blend(theme.accent, theme.canvas, theme.dark ? 0.20 : 0.10) : theme.canvas;
    COLORREF border = open ? theme.line_strong : focused ? theme.accent_dim : fill;
    int top = doc->y, sender_h = font_height(doc->cv, FONT_BODY), subject_h = sender_h;
    bool has_snip = !str_empty(m->snippet);
    int snip_h = has_snip ? font_height(doc->cv, FONT_FOOTNOTE) : 0;
    int row_h = px(10) + sender_h + px(2) + subject_h + (has_snip ? px(2) + snip_h : 0) + px(10);
    char *chip = c->count > 1 ? xstrfmt("%d in thread", (int)c->count) : NULL;
    int chip_w = chip ? text_width(doc->cv, chip, FONT_CAPTION2) + px(12) : 0;
    int box = doc_box_begin(doc, x, w, 0, fill, border, px(10));
    // The subject item has to follow the box immediately: selecting a row clicks the item before its subject.
    int ix = x + px(22), iw = w - px(34);
    int subject_y = top + px(10) + sender_h + px(2);
    RECT sr = { ix, subject_y, ix + iw - (chip_w ? chip_w + px(6) : 0), subject_y + subject_h };
    doc_text_at(doc, &sr, screen_mail_inbox_subject_of(m), open || c->unread ? FONT_BODY_SEMIBOLD : FONT_BODY, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    char *name, *email; screen_mail_inbox_sender_parts(m->sender, &name, &email); free(email);
    char *age = screen_mail_inbox_list_age(m->received_at);
    int age_w = age && *age ? text_width(doc->cv, age, FONT_CAPTION) : 0;
    RECT nr = { ix, top + px(8), ix + iw - (age_w ? age_w + px(8) : 0), top + px(8) + sender_h };
    doc_text_at(doc, &nr, name, c->unread || open ? FONT_BODY_SEMIBOLD : FONT_BODY, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    free(name);
    if (age_w) {
        RECT ar = { ix + iw - age_w, nr.top, ix + iw, nr.bottom };
        doc_text_at(doc, &ar, age, FONT_CAPTION, theme.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    free(age);
    if (c->unread) { RECT dr = { x + px(6), nr.top, x + px(18), nr.bottom }; doc_add(doc, &dr, screen_mail_inbox_paint_dot); }
    if (chip) {
        RECT cr = { ix + iw - chip_w, subject_y + px(1), ix + iw, subject_y + subject_h - px(1) };
        Item *it = doc_item(doc, doc_add(doc, &cr, screen_mail_inbox_paint_count));
        it->text = chip;
    }
    if (has_snip) {
        const MailAccount *a = mail_account_find(&s->accounts, m->account_id);
        char *badge = screen_mail_inbox_account_badge(a);
        int bw = text_width(doc->cv, badge, FONT_TINY_SEMIBOLD) + px(10);
        if (bw < px(24)) bw = px(24);
        int sy = subject_y + subject_h + px(2);
        RECT br = { ix, sy, ix + bw, sy + snip_h };
        Item *bt = doc_item(doc, doc_add(doc, &br, screen_mail_inbox_paint_tag));
        bt->text = badge;
        RECT sn = { ix + bw + px(6), sy, ix + iw, sy + snip_h };
        doc_text_at(doc, &sn, m->snippet, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (s->focus_message == c->primary) s->focus_y = top;
    if (open) s->open_y = top;
    doc->y = top + row_h;
    doc_box_end(doc, box, 0);
    if (store_supports("mail_message")) doc_box_action(doc, box, ACT_SELECT, c->primary);
    doc->y += px(4);
}

void screen_mail_inbox_layout_list(Inbox *s, Doc *doc, const MailGroup *rows, size_t n, int x, int w, bool available) {
    if (s->list_error) { doc_notice(doc, x, w, s->list_error); doc_button(doc, x, 0, "Retry list", BUTTON_BORDERED, ACT_RETRY_LIST, 0, available && !s->list_read); doc_space(doc, px(8)); }
    if (s->list_read) { doc_loading(doc, x, w, "Loading messages..."); doc_space(doc, px(6)); }
    if (s->loaded && !s->messages.count) {
        doc_text(doc, x, w, s->messages.next_cursor
            ? "No readable messages on the loaded pages. Load older messages to keep looking."
            : "No synced messages match these filters.", FONT_BODY, theme.muted, DT_WORDBREAK);
        doc_space(doc, px(8));
    }
    int last_day = -2;
    for (size_t i = 0; i < n; i++) {
        if (!screen_mail_inbox_convo_shown(s, &rows[i])) continue;
        int day = screen_mail_inbox_day_key(s->messages.messages[rows[i].primary].received_at);
        if (day != last_day) {
            int convos = 0, messages = 0;
            // A hidden conversation between two shown ones on this day is skipped. The tally stops only when the day changes.
            for (size_t k = i; k < n && screen_mail_inbox_day_key(s->messages.messages[rows[k].primary].received_at) == day; k++) {
                if (!screen_mail_inbox_convo_shown(s, &rows[k])) continue;
                convos++;
                messages += (int)rows[k].count;
            }
            layout_day(doc, x, w, day, convos, messages);
            last_day = day;
        }
        layout_row(s, doc, x, w, &rows[i]);
    }
    if (s->messages.next_cursor) {
        const char *label = screen_mail_inbox_older_label(s);
        int bw = text_width(doc->cv, label, FONT_FOOTNOTE) + px(28);
        int bx = x + (w - bw) / 2; if (bx < x) bx = x;
        doc_space(doc, px(8));
        doc_button(doc, bx, 0, label, BUTTON_BORDERED, ACT_MORE, 0, available && !s->list_read);
    }
}
