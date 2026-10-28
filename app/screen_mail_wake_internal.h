#ifndef BRIAREUS_SCREEN_MAIL_WAKE_INTERNAL_H
#define BRIAREUS_SCREEN_MAIL_WAKE_INTERNAL_H
#include "screens.h"
#include "mail_wake.h"
#include "str.h"
#include <commctrl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MW_READ, MW_ACCOUNTS, MW_PROJECTS, MW_WRITE, MW_ROUTES };
enum { MW_SAVE = 2700, MW_RETRY, MW_ADD, MW_REMOVE, MW_TOGGLE, MW_PICK, MW_SELECT, MW_FOCUS, MW_DISCARD, MW_RECONCILE };
enum { MW_FIELDS = MAIL_WAKE_CAPS + MAIL_WAKE_LIMIT * MAIL_WAKE_FILTERS, MW_FIELD_ID = 2800 };
typedef struct {
    Screen base;
    Session session;
    ApiClient *client;
    Json *device, *record;
    MailWake draft;
    MailAccounts accounts;
    Request *requests[MW_ROUTES];
    char *keys[MW_ROUTES], *errors[MW_ROUTES], *notice;
    bool fresh[MW_ROUTES], shown, modal, retired, filling;
    int generation, tools, picker;
    double read_epoch, draft_epoch;
    HWND edits[MW_FIELDS]; RECT rects[MW_FIELDS]; bool laid[MW_FIELDS], clipped[MW_FIELDS];
} MailWakeScreen;
extern const char *const mail_wake_operations[MW_ROUTES];
bool mail_wake_current(MailWakeScreen *s);
bool mail_wake_editable(MailWakeScreen *s);
bool mail_wake_savable(MailWakeScreen *s);
bool mail_wake_read_current(MailWakeScreen *s);
const Json *mail_wake_entry(MailWakeScreen *s, int route);
double mail_wake_deadline(MailWakeScreen *s, int route);
void mail_wake_repaint(MailWakeScreen *s);
void mail_wake_load(MailWakeScreen *s);
void mail_wake_save(MailWakeScreen *s);
void mail_wake_cancel(MailWakeScreen *s);
void mail_wake_release(MailWakeScreen *s);
bool mail_wake_confirm(MailWakeScreen *s);
void mail_wake_fill_controls(MailWakeScreen *s);
void mail_wake_ensure_controls(MailWakeScreen *s);
void mail_wake_capture(MailWakeScreen *s, int field);
void mail_wake_layout(Screen *base, Doc *doc);
void mail_wake_header(Screen *base, HeaderInfo *info);
void mail_wake_place(Screen *base, const RECT *content, int scroll);
#endif
