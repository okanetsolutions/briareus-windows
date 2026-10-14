#ifndef BRIAREUS_SCREEN_MAIL_INTERNAL_H
#define BRIAREUS_SCREEN_MAIL_INTERNAL_H
#include "screens.h"
#include "dialogs.h"
#include "mail.h"
#include "mail_settings.h"
#include <commctrl.h>
#include <limits.h>
#include <math.h>
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Global mailbox settings. The read-only inbox has its own screen.

enum { ACT_CONNECT = 1800, ACT_REAUTH, ACT_ENABLED, ACT_DAYS, ACT_DELETE, ACT_SYNC, ACT_FINISH, ACT_CANCEL, ACT_BROWSER_DONE, ACT_SAVE, ACT_FOCUS, ACT_MANAGE };

enum { TIMER_MAIL = 1810, ID_MAIL_LABEL = 2500 };

typedef struct {
    Screen base;
    MailAccounts accounts;
    MailSignIn sign_in;
    Request *read, *write;
    char *error, *notice, *starting_provider;
    int starting_id, failures, focus;
    bool shown, loaded, blocked, modal, retired, read_error;
    ULONGLONG next_read, retry_until;
    // The label is a field on the page, as a project's Label is, saved with Save. Not a dialog.
    HWND label_edit;
    RECT label_rect;
    bool label_laid, label_clipped, label_focused, dirty, filling, label_write;
} MailScreen;

bool mail_settings_offered(void);
double screen_mail_now_ms(void);
void screen_mail_set_text(char **slot, const char *text);
bool mail_settings_result_current(bool shown, const Request *r);
int mail_settings_retry_ms(int failures, double retry_after);
void screen_mail_set_error(MailScreen *s, const char *text, bool from_read);
bool screen_mail_current(MailScreen *s, Request *r);
bool screen_mail_can_write(MailScreen *s, const char *op);
void screen_mail_repaint(MailScreen *s);
void screen_mail_release(MailScreen *s);
void screen_mail_arm(MailScreen *s, int delay);
bool screen_mail_syncing(MailScreen *s);
void screen_mail_stop_waiting(MailScreen *s);
void screen_mail_load(MailScreen *s);
void screen_mail_start(MailScreen *s, const char *provider, int id, bool manage);
void screen_mail_write_done(void *owner, Request *r);
void screen_mail_modal_begin(MailScreen *s);
bool screen_mail_modal_end(MailScreen *s);
void screen_mail_finish(MailScreen *s);
void screen_mail_update(MailScreen *s, const MailAccount *a, int screen_mail_action);
void screen_mail_action(Screen *base, int act, intptr_t arg, POINT pt);
void screen_mail_header(Screen *base, HeaderInfo *info);
char *screen_mail_edit_text(HWND edit);
void screen_mail_set_edit_text(HWND edit, const char *text);
const char *screen_mail_saved_label(const MailScreen *s);
void screen_mail_label_fill(MailScreen *s);
void screen_mail_label_ensure(MailScreen *s);
void screen_mail_label_save(MailScreen *s);
void screen_mail_layout(Screen *base, Doc *doc);
void screen_mail_place(Screen *base, const RECT *content, int scroll_y);
void screen_mail_command(Screen *base, int id, int code, HWND control);
bool screen_mail_key(Screen *base, WPARAM vk, bool ctrl, bool shift);
void screen_mail_visible(Screen *base, bool shown);
Screen *mail_settings_screen_new(void);
Screen *mail_settings_screen_open(int account_id);

#endif
