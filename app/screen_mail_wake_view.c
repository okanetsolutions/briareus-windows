#include "screen_mail_wake_internal.h"
#include <time.h>

static void paint_field(Doc *doc, Item *item, Canvas *cv, const RECT *rc) {
    (void)doc; (void)item; fill_round_rect(cv, rc, px(5), theme.field, theme.line_strong);
}
static void field(MailWakeScreen *s, Doc *doc, int width, int index, const char *label, const char *hint) {
    doc_field_label(doc, 0, width, label, theme.ink, hint); doc_space(doc, px(6));
    int id = doc_custom(doc, 0, width, px(36), paint_field, NULL, NULL, MW_FOCUS, index);
    RECT r = doc_item(doc, id)->rc; int fh = font_height(doc->cv, FONT_BODY);
    int top = r.top + (r.bottom - r.top - fh) / 2;
    s->rects[index] = (RECT){r.left + px(10), top, r.right - px(10), top + fh}; s->laid[index] = true;
    doc_space(doc, px(14));
}
static int usable_count(MailWakeScreen *s) {
    int n = 0;
    for (size_t i = 0; i < s->accounts.count; i++) if (mail_wake_mailbox_usable(&s->accounts, s->accounts.accounts[i].id)) n++;
    return n;
}
static void rule(MailWakeScreen *s, Doc *doc, int width, size_t row) {
    const Json *r = json_at(s->draft.rules, row); bool on = json_bool_is(json_get(r, "enabled"), true);
    int account = json_int_or(json_get(r, "account"), 0); const MailAccount *a = mail_account_find(&s->accounts, account);
    char *label = xstrfmt("Rule %u · %s", (unsigned)row + 1, on ? "on" : "off");
    doc_text(doc, 0, width, label, FONT_BODY_SEMIBOLD, theme.ink, DT_SINGLELINE); free(label); doc_space(doc, px(8));
    bool edit = mail_wake_editable(s);
    bool can_enable = s->fresh[MW_PROJECTS] && s->fresh[MW_ACCOUNTS] && s->tools == 1 && mail_wake_mailbox_usable(&s->accounts, account);
    doc_button(doc, 0, 0, on ? "Turn off" : "Turn on", BUTTON_BORDERED, MW_TOGGLE, (intptr_t)row, edit && (on || can_enable));
    doc_button(doc, 0, 0, "Remove rule", BUTTON_PLAIN, MW_REMOVE, (intptr_t)row, edit); doc_space(doc, px(8));
    char *mailbox = a ? xstrfmt("Mailbox: %s (%s)", a->label ? a->label : a->email, a->email)
        : account ? xstrfmt("Mailbox %d (unavailable)", account) : xstrdup("Choose a mailbox");
    doc_button(doc, 0, width, mailbox, BUTTON_BORDERED, MW_PICK, (intptr_t)row, edit && s->fresh[MW_ACCOUNTS]); free(mailbox);
    if (s->picker == (int)row) {
        for (size_t i = 0; i < s->accounts.count; i++) {
            const MailAccount *box = &s->accounts.accounts[i];
            if (mail_wake_mailbox_usable(&s->accounts, box->id)) doc_button(doc, px(12), width - px(12), box->email, BUTTON_PLAIN, MW_SELECT, box->id, edit);
        }
    }
    if (account && !mail_wake_mailbox_usable(&s->accounts, account)) doc_notice(doc, 0, width, "This mailbox cannot currently wake the session. Turn this rule off or choose another mailbox; its saved filters are retained.");
    static const char *const hints[] = {"One exact address in To/Cc; empty matches any.", "One exact sender address; empty matches any except this mailbox itself.", "Case-insensitive text in the subject; empty matches any. At most 320 UTF-16 characters."};
    for (int f = 0; f < MAIL_WAKE_FILTERS; f++) field(s, doc, width, MAIL_WAKE_CAPS + (int)row * MAIL_WAKE_FILTERS + f, mail_wake_filter_labels[f], hints[f]);
    doc_space(doc, px(10));
}
void mail_wake_layout(Screen *base, Doc *doc) {
    MailWakeScreen *s = (MailWakeScreen *)base; memset(s->laid, 0, sizeof s->laid);
    int w = doc->width; doc_space(doc, px(16));
    if (!mail_wake_current(s)) { doc_notice(doc, 0, w, "This connection or token changed. Reopen Mail wake from the conversation."); return; }
    static const char *const names[] = {"Wake rules", "Mailboxes", "Project mail tools", "Save"};
    for (int i = 0; i < MW_ROUTES; i++) {
        if (s->errors[i]) {
            char *text = xstrfmt("%s: %s", names[i], s->errors[i]); doc_notice(doc, 0, w, text); free(text);
        }
        if (s->requests[i]) {
            char *text = xstrfmt(i == MW_WRITE ? "Saving mail wake rules..." : "Reading %s...", names[i]); doc_loading(doc, 0, w, text); free(text);
        }
        if (mail_wake_deadline(s, i) > (double)time(NULL)) doc_notice(doc, 0, w, "Retry-After is active. Refresh and Save respect this cooldown.");
    }
    doc_button(doc, 0, 0, "Retry / Refresh", BUTTON_BORDERED, MW_RETRY, 0, !s->requests[MW_WRITE]);
    if (s->notice) doc_notice(doc, 0, w, s->notice);
    const Json *write = mail_wake_entry(s, MW_WRITE);
    if (json_bool_is(json_get(write, "pending"), true)) doc_loading(doc, 0, w, "Waiting for the previous save to finish; no duplicate save will be sent.");
    if (json_bool_is(json_get(write, "uncertain"), true)) {
        doc_notice(doc, 0, w, "Save outcome is uncertain. Your draft is kept. Refresh the saved rules, then explicitly use them before saving again.");
        doc_button(doc, 0, 0, "Discard draft and use refreshed rules", BUTTON_BORDERED, MW_RECONCILE, 0,
            mail_wake_read_current(s) && mail_wake_editable(s));
        if (s->record) { char *record = json_pretty(s->record); doc_text(doc, 0, w, record, FONT_MONO, theme.muted, DT_WORDBREAK); free(record); }
    }
    if (!s->draft.rules) return;
    doc_space(doc, px(12));
    doc_text(doc, 0, w, "New inbox mail matching every filter of an enabled rule may wake this session. Mail waits behind running turns and questions; closed or failed sessions stay closed. Mail sent by the mailbox itself never wakes it.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK); doc_space(doc, px(12));
    if (s->tools == 0) doc_notice(doc, 0, w, "This project's mail tools are off. Disable rules here, or enable tools in project settings.");
    if (s->tools < 0) doc_notice(doc, 0, w, "Project mail tools could not be confirmed from Settings > Projects.");
    if (s->fresh[MW_ACCOUNTS] && !usable_count(s)) doc_notice(doc, 0, w, "No usable mailbox. Connect and enable a mailbox with an available provider in Settings > Mail.");
    field(s, doc, w, 0, mail_wake_cap_labels[0], "From 1 to 600. Mail past the cap waits for the next hour.");
    field(s, doc, w, 1, mail_wake_cap_labels[1], "From 1 to 1000; zero is invalid. Wakes wait after this many automatic turns until an operator speaks.");
    if (!json_count(s->draft.rules)) doc_notice(doc, 0, w, "No rules. Mail does not wake this session.");
    for (size_t i = 0; i < json_count(s->draft.rules); i++) rule(s, doc, w, i);
    doc_button(doc, 0, 0, "Add rule", BUTTON_BORDERED, MW_ADD, 0, mail_wake_editable(s)
        && s->fresh[MW_ACCOUNTS] && s->fresh[MW_PROJECTS] && s->tools == 1 && usable_count(s) && json_count(s->draft.rules) < MAIL_WAKE_LIMIT);
    doc_button(doc, 0, 0, "Discard changes", BUTTON_PLAIN, MW_DISCARD, 0, mail_wake_editable(s) && mail_wake_dirty(&s->draft));
    doc_space(doc, px(40));
}
void mail_wake_header(Screen *base, HeaderInfo *info) {
    MailWakeScreen *s = (MailWakeScreen *)base; snprintf(info->title, sizeof info->title, "✉ Mail wake");
    if (!mail_wake_current(s)) return;
    const char *state = s->requests[MW_WRITE] ? "saving..." : !s->record ? "not loaded" : mail_wake_active(s->record) ? "on" : "off";
    snprintf(info->subtitle, sizeof info->subtitle, "%s · %s%s", session_display_title(&s->session), state, mail_wake_dirty(&s->draft) ? " · unsaved" : "");
    HeaderButton *b = &info->buttons[info->button_count++]; snprintf(b->label, sizeof b->label, "Save");
    b->glyph = 0xE74E; b->action = MW_SAVE; b->prominent = true; b->tip = "Save (Ctrl+S)"; b->enabled = mail_wake_savable(s);
}
