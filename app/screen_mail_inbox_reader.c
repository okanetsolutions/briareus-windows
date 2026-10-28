#include "screen_mail_inbox_internal.h"

typedef struct { char *name, *meta; } AttachRow;

static void attach_free(void *p);
static void paint_attach(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void layout_attachment(Doc *doc, int x, int w, const MailAttachment *a);
static void layout_block(Inbox *s, Doc *doc, int x, int w, int index);

static void attach_free(void *p) { AttachRow *d = p; free(d->name); free(d->meta); free(d); }

static void paint_attach(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc; AttachRow *d = it->data;
    RECT chip = *rc; chip.right = chip.left + px(36); chip.bottom = chip.top + px(36);
    if (chip.bottom > rc->bottom) chip.bottom = rc->bottom;
    fill_round_rect(cv, &chip, px(6), theme.field, theme.line);
    draw_glyph(cv, 0xE8A5, &chip, FONT_ICON_SMALL, theme.muted);
    int tx = chip.right + px(10);
    RECT n = { tx, rc->top, rc->right, rc->top + px(18) };
    draw_text(cv, d->name, &n, FONT_CAPTION_SEMIBOLD, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT m = { tx, n.bottom, rc->right, rc->bottom };
    draw_text(cv, d->meta, &m, FONT_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void layout_attachment(Doc *doc, int x, int w, const MailAttachment *a) {
    const char *name = a->name ? a->name : "Unnamed", *type = a->mime_type ? a->mime_type : "Unknown type";
    AttachRow *d = xcalloc(1, sizeof *d);
    d->name = xstrdup(name); d->meta = xstrfmt("%s  ·  %.0f bytes", type, a->size);
    Item *it = doc_item(doc, doc_custom(doc, x, w, px(36), paint_attach, d, attach_free, 0, 0));
    it->text = xstrfmt("%s | %s | %.0f bytes", name, type, a->size);
}

static void layout_block(Inbox *s, Doc *doc, int x, int w, int index) {
    const MailMessage *m = &s->messages.messages[index];
    bool selected = screen_mail_inbox_row_open(s, m);
    if (selected) { s->reader_block_y = doc->y; s->reader_block_first = s->reader_blocks == 0; }
    s->reader_blocks++;
    const MailMessage *full = selected && s->body.id ? &s->body : m;
    char *name, *email; screen_mail_inbox_sender_parts(full->sender && *full->sender ? full->sender : m->sender, &name, &email);
    const MailAccount *account = mail_account_find(&s->accounts, m->account_id);
    bool mine = account && account->email && !str_empty(email) && str_ieq(email, account->email);
    int y = doc->y, av = px(32);
    Avatar *ad = xcalloc(1, sizeof *ad);
    screen_mail_inbox_initials_of(name, ad->initials); ad->mine = mine;
    RECT ar = { x, y, x + av, y + av };
    Item *ai = doc_item(doc, doc_add(doc, &ar, screen_mail_inbox_paint_avatar));
    ai->data = ad; ai->free_data = free; ai->text = xstrdup(ad->initials);
    if (!selected && store_supports("mail_message")) { ai->action = ACT_SELECT; ai->arg = index; ai->hand = true; }
    char *age = (full->received_at ? full->received_at : m->received_at) ? format_relative((time_t)((full->received_at ? full->received_at : m->received_at) / 1000)) : xstrdup("");
    int age_w = age && *age ? text_width(doc->cv, age, FONT_CAPTION) : 0;
    char *who = str_empty(email) || str_eq(email, name) ? xstrdup(name) : xstrfmt("%s   %s", name, email);
    RECT nr = { x + av + px(10), y, x + w - (age_w ? age_w + px(8) : 0), y + px(18) };
    int ni = doc_text_at(doc, &nr, who, FONT_BODY_SEMIBOLD, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (!selected && store_supports("mail_message")) { doc->items[ni].action = ACT_SELECT; doc->items[ni].arg = index; doc->items[ni].hand = true; }
    if (age_w) {
        RECT tr = { x + w - age_w, y, x + w, y + px(18) };
        doc_text_at(doc, &tr, age, FONT_CAPTION, theme.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    const char *to = !str_empty(full->to) ? full->to : NULL;
    if (to) {
        char *line = xstrfmt("to %s", to);
        RECT lr = { nr.left, y + px(18), x + w, y + px(36) };
        doc_text_at(doc, &lr, line, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        free(line);
    }
    free(who); free(name); free(email); free(age);
    doc->y = y + (to ? px(44) : px(36));
    if (selected && s->body.id) {
        s->reader_body_y = doc->y;
        doc_text(doc, x, w, str_empty(s->body.text) ? "No plain-text body is available in the synced copy." : s->body.text, FONT_BODY, theme.ink, DT_WORDBREAK);
        s->reader_body_bottom = doc->y;
        if (!str_empty(s->body.cc)) { doc_space(doc, px(6)); char *cc = xstrfmt("Cc  %s", s->body.cc); doc_text(doc, x, w, cc, FONT_CAPTION, theme.muted, DT_WORDBREAK); free(cc); }
        if (!str_empty(s->body.reply_to)) { char *rt = xstrfmt("Reply-To  %s", s->body.reply_to); doc_text(doc, x, w, rt, FONT_CAPTION, theme.muted, DT_WORDBREAK); free(rt); }
        if (s->body.truncated) { doc_space(doc, px(8)); doc_notice(doc, x, w, "The server truncated this body. Open at the provider to read the complete message."); }
        if (s->body.attachment_count) { doc_space(doc, px(12)); doc_section(doc, x, w, "Attachments"); doc_space(doc, px(6)); }
        for (size_t i = 0; i < s->body.attachment_count; i++) { layout_attachment(doc, x, w, &s->body.attachments[i]); doc_space(doc, px(6)); }
    } else {
        const char *preview = !str_empty(m->snippet) ? m->snippet : "Open this message";
        int i = doc_text(doc, x, w, preview, FONT_BODY, theme.muted, DT_WORDBREAK);
        if (store_supports("mail_message")) { doc->items[i].action = ACT_SELECT; doc->items[i].arg = index; doc->items[i].hand = true; }
    }
}

void screen_mail_inbox_layout_reader(Inbox *s, Doc *doc, const MailGroup *rows, size_t n, int x, int w, bool available) {
    int pad = px(20);
    int box = doc_box_begin(doc, x, w, pad, theme.raise, theme.line, px(12));
    int ix = x + pad, iw = w - pad * 2;
    if (!s->selected_id) {
        doc_text(doc, ix, iw, "Select a conversation", FONT_BODY_SEMIBOLD, theme.ink, DT_LEFT | DT_SINGLELINE);
        doc_space(doc, px(4));
        doc_text(doc, ix, iw, "Opening a message here leaves it unread at the provider.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
        doc_box_end(doc, box, pad);
        doc_space(doc, px(12));
        return;
    }
    int bar = doc->y, bh = px(32), right = ix + iw;
    RECT close_rc = { ix, bar, ix + bh, bar + bh };
    screen_mail_inbox_add_icon(doc, &close_rc, 0xE711, false, false, "Close message", ACT_CLOSE, "Close message");
    bool url = s->body.id && mail_message_web_url_safe(s->body.web_url);
    if (url) {
        right -= bh;
        RECT rc = { right, bar, right + bh, bar + bh };
        screen_mail_inbox_add_icon(doc, &rc, 0xE8A7, false, false, "Open at provider", ACT_PROVIDER, "Open at provider");
    }
    bool native_delete = s->body.id && store_supports("delete_mail_message");
    if (url || native_delete) {
        const char *label = native_delete ? "Delete message" : "Delete at provider";
        int bw = text_width(doc->cv, label, FONT_CAPTION) + px(36);
        // Put the command on its own row in a stacked/narrow reader.
        int command_y = bar;
        if (bw + bh * 2 + px(16) > iw) { command_y += bh + px(8); right = ix + iw; }
        RECT rc = { right - px(8) - bw, command_y, right - px(8), command_y + bh };
        screen_mail_inbox_add_command(doc, &rc, 0xE74D, label, native_delete ? (available ? ACT_DELETE : 0) : ACT_DELETE_PROVIDER, native_delete ? "Move only the selected message to trash" : "Open this message in your mailbox to delete it; after the next mail sync, click Refresh to update this list.");
        bar = command_y;
    }
    doc->y = bar + bh + px(12);
    if (s->deletion) { doc_loading(doc, ix, iw, "Moving message to trash..."); doc_space(doc, px(8)); }
    if (s->body_error) doc_notice(doc, ix, iw, s->body_error);
    if (s->body_read) doc_loading(doc, ix, iw, "Loading message body...");
    else if (!s->body.id) doc_button(doc, ix, 0, "Retry message", BUTTON_BORDERED, ACT_RETRY_BODY, 0, available && store_supports("mail_message"));
    if (s->body.id) {
        const MailGroup *c = screen_mail_inbox_convo_for_selection(s, rows, n);
        const MailMessage *head = c ? &s->messages.messages[c->primary] : &s->body;
        doc_text(doc, ix, iw, screen_mail_inbox_subject_of(head), FONT_TITLE, theme.ink, DT_WORDBREAK);
        doc_space(doc, px(6));
        const MailAccount *a = mail_account_find(&s->accounts, s->body.account_id);
        char *badge = screen_mail_inbox_account_badge(a);
        int count = c ? (int)c->count : 1;
        char *meta = xstrfmt("%d message%s", count, count == 1 ? "" : "s");
        int y = doc->y, mh = font_height(doc->cv, FONT_CAPTION);
        int bw = text_width(doc->cv, badge, FONT_TINY_SEMIBOLD) + px(12); if (bw < px(24)) bw = px(24);
        RECT br = { ix, y, ix + bw, y + mh };
        Item *bt = doc_item(doc, doc_add(doc, &br, screen_mail_inbox_paint_tag)); bt->text = badge;
        RECT mr = { ix + bw + px(8), y, ix + iw, y + mh };
        doc_text_at(doc, &mr, meta, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        free(meta);
        doc->y = y + mh + px(10);
        if (!s->body.is_read) {
            doc_text(doc, ix, iw, "Unread at provider · Reading here does not mark it read.", FONT_CAPTION, theme.muted, DT_WORDBREAK);
            doc_space(doc, px(10));
        }
        doc_rule(doc, ix, iw);
        doc_space(doc, px(12));
        if (c) {
            for (size_t i = 0; i < c->count; i++) {
                layout_block(s, doc, ix, iw, c->members[i]);
                if (i + 1 < c->count) { doc_space(doc, px(10)); doc_rule(doc, ix, iw); doc_space(doc, px(10)); }
            }
        } else {
            int index = -1;
            for (size_t i = 0; i < s->messages.count; i++) if (screen_mail_inbox_row_open(s, &s->messages.messages[i])) { index = (int)i; break; }
            if (index >= 0) layout_block(s, doc, ix, iw, index);
            else {
                doc_text(doc, ix, iw, str_empty(s->body.text) ? "No plain-text body is available in the synced copy." : s->body.text, FONT_BODY, theme.ink, DT_WORDBREAK);
                if (s->body.truncated) { doc_space(doc, px(8)); doc_notice(doc, ix, iw, "The server truncated this body. Open at the provider to read the complete message."); }
                for (size_t i = 0; i < s->body.attachment_count; i++) { layout_attachment(doc, ix, iw, &s->body.attachments[i]); doc_space(doc, px(6)); }
            }
        }
        doc_space(doc, px(20));
        doc_rule(doc, ix, iw);
        doc_space(doc, px(12));
        screen_mail_reply_layout(s, doc, ix, iw);
        doc_text(doc, ix, iw, "Forward, archive and star messages in your mailbox.", FONT_CAPTION, theme.muted, DT_WORDBREAK);
        doc_space(doc, px(8));
        doc_button(doc, ix, 0, "Reply at provider", BUTTON_BORDERED, ACT_PROVIDER, 0, url);
    }
    if (s->limit_note) { doc_space(doc, px(8)); doc_text(doc, ix, iw, s->limit_note, FONT_FOOTNOTE, theme.muted, DT_WORDBREAK); }
    doc_box_end(doc, box, pad);
    doc_space(doc, px(12));
}
