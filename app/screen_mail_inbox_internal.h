#ifndef BRIAREUS_SCREEN_MAIL_INBOX_INTERNAL_H
#define BRIAREUS_SCREEN_MAIL_INBOX_INTERNAL_H
#include "screens.h"
#include "dialogs.h"
#include "mail.h"
#include "mail_group.h"
#include "mail_inbox.h"
#include "mail_settings.h"
#include "str.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Global synced inbox; private content never enters DiskCache.

enum { ACT_ACCOUNT = 1900, ACT_Q, ACT_LABEL, ACT_THREAD, ACT_UNREAD, ACT_INBOX, ACT_STARRED,
       ACT_RESET, ACT_MORE, ACT_SELECT, ACT_CLOSE, ACT_PROVIDER, ACT_RETRY_LIST, ACT_RETRY_BODY, ACT_RETRY_ACCOUNTS,
       ACT_MENU, ACT_KIND, ACT_GROUP, ACT_SYNC, ACT_DELETE_PROVIDER, ACT_DELETE, ACT_REPLY, ACT_REPLY_SEND, ACT_REPLY_CANCEL, ACT_REPLY_RECONCILE };

enum { TIMER_INBOX = 1910 };

enum { MENU_NONE, MENU_MAILBOX, MENU_FOLDER, MENU_GROUP };

enum { KIND_PEOPLE, KIND_NOTES, KIND_ALL };

enum { GROUP_THREADS, GROUP_EACH };

typedef struct {
    Screen base;
    ApiClient *mail_client; // retained identity of the loaded mailbox data
    MailAccounts accounts;
    MailMessages messages;
    MailMessage body;
    MailFilter filter;
    Request *account_read, *list_read, *body_read, *deletion, *reply;
    char *selected_id, *account_error, *list_error, *body_error, *notice, *limit_note;
    int selected_account, generation, kind, group, menu, focus_message, focus_y, open_y, chrome_bottom, scroll_seen, reader_y, reader_block_y, reader_blocks;
    // The opened block's last layout, so a later page can add only the height inserted above it. page_follow is 1 to
    // scroll to the block after a wide/stacked swap, and 2 to add page_delta, consumed by the next scrolled().
    int block_offset, block_page, page_follow, page_delta;
    int reader_body_y, reader_body_bottom;
    bool shown, accounts_loaded, loaded, modal, retired, reveal_focus, reveal_open, reveal_block, reader_block_first, in_scroll, stacked;
    bool block_held, block_stacked;
} Inbox;

typedef struct { RECT mailbox, folder, group; } Anchors;

typedef struct { char *shown; bool placeholder; } SearchData;

typedef struct { char *shown; int unread; bool on; } MailBtn;

typedef struct { char *shown; bool on, accent, caret; } ChipData;

typedef struct { char initials[3]; bool mine; } Avatar;

MailReply *screen_mail_reply_draft(Inbox *s, bool create);
bool screen_mail_reply_allowed(Inbox *s);
void screen_mail_reply_action(Inbox *s, int act);
void screen_mail_reply_layout(Inbox *s, Doc *doc, int x, int w);
void screen_mail_reply_cancel(Inbox *s);
bool mail_inbox_offered(void);
bool mail_inbox_result_current(bool shown, int generation, const Request *r);
void screen_mail_inbox_text_set(char **slot, const char *text);
HWND screen_mail_inbox_window(Inbox *s);
void screen_mail_inbox_repaint(Inbox *s);
void screen_mail_inbox_cancel(Inbox *s);
void screen_mail_inbox_clear_selection(Inbox *s);
void screen_mail_inbox_reset_list(Inbox *s);
void screen_mail_inbox_clear_private(Inbox *s);
bool screen_mail_inbox_current(Inbox *s, Request *r);
bool screen_mail_inbox_ready(Inbox *s);
void screen_mail_inbox_arm(Inbox *s);
void screen_mail_inbox_failure(Inbox *s, Request *r, char **error, bool detail);
void screen_mail_inbox_load_body(Inbox *s);
void screen_mail_inbox_load_list(Inbox *s);
void screen_mail_inbox_load_accounts(Inbox *s);
void screen_mail_inbox_reload(Inbox *s);
void screen_mail_inbox_release(Inbox *s);
void mail_inbox_set_thread_filter(Screen *base, const char *thread);
void screen_mail_inbox_edit_filter(Inbox *s, int act);
int screen_mail_inbox_day_key(double ms);
bool screen_mail_inbox_convo_shown(const Inbox *s, const MailGroup *c);
int screen_mail_inbox_convo_primary_for(const Inbox *s, int message);
const MailGroup *screen_mail_inbox_convo_for_selection(const Inbox *s, const MailGroup *rows, size_t n);
void screen_mail_inbox_initials_of(const char *name, char out[3]);
void screen_mail_inbox_sender_parts(const char *sender, char **name, char **email);
char *screen_mail_inbox_account_badge(const MailAccount *a);
char *screen_mail_inbox_list_age(double ms);
const char *screen_mail_inbox_subject_of(const MailMessage *m);
bool screen_mail_inbox_row_open(const Inbox *s, const MailMessage *m);
const char *screen_mail_inbox_older_label(const Inbox *s);
void screen_mail_inbox_open_provider(Inbox *s);
void screen_mail_inbox_cycle_account(Inbox *s);
void screen_mail_inbox_move_focus(Inbox *s, int delta);
void screen_mail_inbox_action(Screen *base, int act, intptr_t arg, POINT pt);
void screen_mail_inbox_header(Screen *base, HeaderInfo *info);
void screen_mail_inbox_search_free(void *p);
void screen_mail_inbox_mailbtn_free(void *p);
void screen_mail_inbox_chip_free(void *p);
void screen_mail_inbox_paint_search(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
void screen_mail_inbox_paint_mailbtn(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
void screen_mail_inbox_paint_chip(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
void screen_mail_inbox_paint_dot(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
void screen_mail_inbox_paint_count(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
void screen_mail_inbox_paint_tag(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
void screen_mail_inbox_paint_avatar(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
void screen_mail_inbox_add_icon(Doc *doc, const RECT *rc, wchar_t glyph, bool on, bool muted, const char *text, int screen_mail_inbox_action, const char *tip);
void screen_mail_inbox_add_command(Doc *doc, const RECT *rc, wchar_t glyph, const char *text, int screen_mail_inbox_action, const char *tip);
int screen_mail_inbox_chip_span(Doc *doc, const char *shown, bool accent, bool caret);
int screen_mail_inbox_add_chip(Doc *doc, int x, int y, int h, const char *text, const char *shown, bool on, bool accent, bool caret, int screen_mail_inbox_action, intptr_t arg, int max_w);
int screen_mail_inbox_menu_row(Doc *doc, int x, int y, int w, const char *text, bool on, int screen_mail_inbox_action, intptr_t arg);
void screen_mail_inbox_layout_chrome(Inbox *s, Doc *doc, int x, int w, const MailGroup *rows, size_t nrow, bool available, Anchors *anchors);
int screen_mail_inbox_layout_menu(Inbox *s, Doc *doc, const Anchors *anchors);
void screen_mail_inbox_layout_list(Inbox *s, Doc *doc, const MailGroup *rows, size_t n, int x, int w, bool available);
void screen_mail_inbox_layout_reader(Inbox *s, Doc *doc, const MailGroup *rows, size_t n, int x, int w, bool available);
void screen_mail_inbox_layout(Screen *base, Doc *doc);
bool screen_mail_inbox_key(Screen *base, WPARAM vk, bool ctrl, bool shift);
void screen_mail_inbox_scrolled(Screen *base, bool at_bottom);
Screen *mail_screen_new(void);

#endif
