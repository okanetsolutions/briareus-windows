// Read-only global inbox; private content never enters DiskCache or the preview WebView.
#include "screens.h"
#include "dialogs.h"
#include "mail.h"
#include "mail_inbox.h"
#include "mail_settings.h"
#include "str.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { ACT_ACCOUNT = 1900, ACT_Q, ACT_LABEL, ACT_THREAD, ACT_UNREAD, ACT_INBOX, ACT_STARRED,
       ACT_RESET, ACT_MORE, ACT_SELECT, ACT_CLOSE, ACT_PROVIDER, ACT_RETRY_LIST, ACT_RETRY_BODY, ACT_RETRY_ACCOUNTS,
       ACT_MENU, ACT_KIND, ACT_GROUP, ACT_SYNC, ACT_DELETE_PROVIDER };
enum { TIMER_INBOX = 1910 };
enum { MENU_NONE, MENU_MAILBOX, MENU_FOLDER, MENU_GROUP };
enum { KIND_PEOPLE, KIND_NOTES, KIND_ALL };
enum { GROUP_THREADS, GROUP_EACH };
typedef struct {
    Screen base;
    MailAccounts accounts;
    MailMessages messages;
    MailMessage body;
    MailFilter filter;
    Request *account_read, *list_read, *body_read;
    char *selected_id, *account_error, *list_error, *body_error, *notice, *limit_note;
    int selected_account, generation, kind, group, menu, focus_message, focus_y, open_y, chrome_bottom, scroll_seen, reader_y, reader_block_y, reader_blocks;
    // The opened block's last layout, so a later page can add only the height inserted above it. page_follow is 1 to
    // scroll to the block after a wide/stacked swap, and 2 to add page_delta, consumed by the next scrolled().
    int block_offset, block_page, page_follow, page_delta;
    bool shown, accounts_loaded, loaded, modal, retired, reveal_focus, reveal_open, reveal_block, reader_block_first, in_scroll, stacked;
    bool block_held, block_stacked;
} Inbox;

bool mail_inbox_offered(void) {
    return g_store.has_device && permission_rank(g_store.device.permission) == 2
        && store_supports("settings_mail_accounts") && store_supports("mail_messages");
}
bool mail_inbox_result_current(bool shown, int generation, const Request *r) {
    return shown && !r->cancelled && r->client && r->client == g_store.client
        && r->tag == generation && mail_inbox_offered() && store_supports(r->operation);
}
static void text_set(char **slot, const char *text) { free(*slot); *slot = xstrdup(text); }
static HWND window(Inbox *s) { return s->base.pane ? pane_hwnd(s->base.pane) : NULL; }
static void repaint(Inbox *s) {
    if (s->base.pane) { pane_relayout(s->base.pane); pane_header_changed(s->base.pane); }
}
static void cancel(Inbox *s) {
    request_cancel(&s->account_read); request_cancel(&s->list_read); request_cancel(&s->body_read);
    if (window(s)) KillTimer(window(s), TIMER_INBOX);
}
static void clear_selection(Inbox *s) {
    request_cancel(&s->body_read); mail_message_free(&s->body);
    text_set(&s->selected_id, NULL); text_set(&s->body_error, NULL); s->selected_account = 0;
    s->reveal_open = false; s->reveal_block = false; s->block_held = false; s->page_follow = 0;
}
static void reset_list(Inbox *s) {
    request_cancel(&s->list_read); clear_selection(s); mail_messages_free(&s->messages);
    text_set(&s->list_error, NULL); text_set(&s->notice, NULL); text_set(&s->limit_note, NULL); s->loaded = false;
    // Old account responses must also be discarded across a filter generation.
    request_cancel(&s->account_read);
    s->focus_message = -1; s->menu = MENU_NONE;
    s->generation = s->generation == INT_MAX ? 1 : s->generation + 1;
}
static void clear_private(Inbox *s) {
    cancel(s); reset_list(s); mail_accounts_free(&s->accounts); s->accounts_loaded = false;
    mail_filter_free(&s->filter); text_set(&s->account_error, NULL);
    s->kind = KIND_PEOPLE; s->group = GROUP_THREADS;
}
static bool current(Inbox *s, Request *r) {
    if (mail_inbox_result_current(s->shown, s->generation, r)) return true;
    if (!mail_inbox_offered() || r->client != g_store.client) { clear_private(s); repaint(s); }
    return false;
}
static bool ready(Inbox *s) { return s->shown && !s->modal && g_store.active && g_store.client && mail_inbox_offered() && GetTickCount64() >= g_store.mail_retry_until; }
static void arm(Inbox *s) {
    if (!s->shown || s->modal || !g_store.active || !mail_inbox_offered() || !window(s)) return;
    ULONGLONG tick = GetTickCount64(), delay = tick < g_store.mail_retry_until ? g_store.mail_retry_until - tick : 30000;
    SetTimer(window(s), TIMER_INBOX, (UINT)(delay > INT_MAX ? INT_MAX : delay), NULL);
}
static void load_accounts(Inbox *s);
static void load_list(Inbox *s);
static void load_body(Inbox *s);
static void failure(Inbox *s, Request *r, char **error, bool detail) {
    text_set(error, mail_read_error(r->error.status, detail));
    if (r->error.status == 401 || r->error.status == 403
        || (r->error.status == 404 && str_eq(r->operation, "settings_mail_accounts"))) {
        clear_private(s); text_set(&s->account_error, mail_read_error(r->error.status, false)); return;
    }
    if (r->error.status == 409) {
        reset_list(s); mail_accounts_free(&s->accounts); s->accounts_loaded = false;
        text_set(&s->account_error, mail_read_error(409, false));
    }
    if (r->error.status == 429 || r->error.status == 503 || r->error.status == 0 || r->error.status >= 500) {
        if (g_store.mail_failures < 4) g_store.mail_failures++;
        ULONGLONG until = GetTickCount64() + (ULONGLONG)mail_settings_retry_ms(g_store.mail_failures, r->error.retry_after);
        if (until > g_store.mail_retry_until) g_store.mail_retry_until = until;
    }
    arm(s);
}
static void body_done(void *owner, Request *r) {
    Inbox *s = owner;
    if (!current(s, r) || !mail_account_readable(&s->accounts, s->selected_account)
        || json_int_or(json_get(r->args, "account"), 0) != s->selected_account
        || !str_eq(json_str(json_get(r->args, "id")), s->selected_id)) return;
    if (!r->ok) {
        if (r->error.status == 404) {
            reset_list(s); text_set(&s->notice, mail_read_error(404, true));
            load_accounts(s); // The next account snapshot validates removals before a fresh first page.
        } else failure(s, r, &s->body_error, true);
    } else {
        MailMessage fresh;
        if (!mail_message_parse(json_get(r->result, "message"), true, &fresh)) text_set(&s->body_error, "The server returned an unexpected message.");
        else {
            if (fresh.account_id != s->selected_account || !str_eq(fresh.id, s->selected_id)) {
                text_set(&s->body_error, "The server returned a different message; refresh the list."); mail_message_free(&fresh);
            } else { mail_message_free(&s->body); s->body = fresh; text_set(&s->body_error, NULL); g_store.mail_failures = 0; }
        }
    }
    repaint(s);
}
static void load_body(Inbox *s) {
    if (!ready(s) || s->body_read || !s->selected_id || !store_supports("mail_message") || !mail_account_readable(&s->accounts, s->selected_account)) return;
    Json *args = json_object(); json_set_num(args, "account", s->selected_account); json_set_str(args, "id", s->selected_id);
    store_call("mail_message", args, 0, s, body_done, s->generation, &s->body_read); repaint(s);
}
static void list_done(void *owner, Request *r) {
    Inbox *s = owner; if (!current(s, r)) return;
    if (!r->ok) {
        failure(s, r, &s->list_error, false);
        if (r->error.status == 404) {
            clear_private(s); text_set(&s->account_error, mail_read_error(404, false));
        }
    } else {
        MailMessages page;
        if (!mail_messages_parse(r->result, &page)) text_set(&s->list_error, "The server returned an unexpected message list.");
        else {
            bool valid = true;
            for (size_t i = 0; i < page.count; i++) {
                if (!mail_account_find(&s->accounts, page.messages[i].account_id)
                    || (s->filter.account && page.messages[i].account_id != s->filter.account)) valid = false;
            }
            const char *cursor = json_str(json_get(r->args, "cursor"));
            if (page.next_cursor && str_eq(page.next_cursor, cursor)) valid = false;
            if (!valid) { mail_messages_free(&page); text_set(&s->list_error, "The page has unexpected accounts or a repeated cursor. Refresh account status and the list."); }
            else {
                // Revoked accounts may still occur in the server's synced copy; skip them without losing older pages.
                char *next = page.next_cursor; page.next_cursor = NULL;
                mail_messages_prune(&page, &s->accounts); page.next_cursor = next;
                mail_messages_append(&s->messages, &page); s->loaded = true; g_store.mail_failures = 0;
                text_set(&s->list_error, NULL);
            }
        }
    }
    repaint(s);
}
static void load_list(Inbox *s) {
    if (!ready(s) || !s->accounts_loaded || s->list_read || (s->loaded && !s->messages.next_cursor)) return;
    if (s->filter.account && !mail_account_readable(&s->accounts, s->filter.account)) return;
    store_call("mail_messages", mail_filter_args(&s->filter, s->loaded ? s->messages.next_cursor : NULL), 0, s, list_done, s->generation, &s->list_read);
    repaint(s);
}
static void accounts_done(void *owner, Request *r) {
    Inbox *s = owner; if (!current(s, r)) return;
    if (!r->ok) failure(s, r, &s->account_error, false);
    else {
        MailAccounts fresh;
        if (!mail_accounts_parse(r->result, &fresh)) {
            clear_private(s); text_set(&s->account_error, "The server returned an unexpected account list. Retry account status.");
        } else {
            bool changed = false;
            for (size_t i = 0; i < s->accounts.count; i++) {
                const MailAccount *a = &s->accounts.accounts[i];
                if (mail_account_readable(&s->accounts, a->id) && !mail_account_readable(&fresh, a->id)) changed = true;
            }
            for (size_t i = 0; i < fresh.count; i++) {
                const MailAccount *a = &fresh.accounts[i];
                if (mail_account_readable(&fresh, a->id) && !mail_account_readable(&s->accounts, a->id)) changed = true;
            }
            if (changed) reset_list(s); // Reload skipped rows as well as clearing removed/revoked accounts.
            mail_accounts_free(&s->accounts); s->accounts = fresh; s->accounts_loaded = true;
            // Account polling must not reset backoff while the first message page is still failing.
            if (s->loaded) g_store.mail_failures = 0;
            if (s->filter.account && !mail_account_readable(&fresh, s->filter.account)) { s->filter.account = 0; reset_list(s); }
            text_set(&s->account_error, NULL);
            if (!s->loaded) load_list(s);
        }
    }
    arm(s); repaint(s);
}
static void load_accounts(Inbox *s) {
    if (!ready(s) || s->account_read) { arm(s); return; }
    store_call("settings_mail_accounts", json_object(), 0, s, accounts_done, s->generation, &s->account_read); repaint(s);
}
static void reload(Inbox *s) { reset_list(s); load_accounts(s); repaint(s); }
static void release(Inbox *s) { clear_private(s); text_set(&s->list_error, NULL); text_set(&s->body_error, NULL); screen_release(&s->base); }
void mail_inbox_set_thread_filter(Screen *base, const char *thread) {
    Inbox *s = (Inbox *)base;
    free(s->filter.thread);
    s->filter.thread = xstrdup(thread && *thread ? thread : NULL);
}
static void edit_filter(Inbox *s, int act) {
    if (!window(s)) return;
    char **slot = act == ACT_Q ? &s->filter.q : act == ACT_LABEL ? &s->filter.label : &s->filter.thread;
    const char *label = act == ACT_Q ? "Search sender, subject or snippet (200 characters maximum)" : act == ACT_LABEL ? "Exact label or folder name" : "Exact thread/conversation ID";
    s->modal = true; cancel(s);
    char *value = dialog_text(window(s), "Mail filter", label, "Apply", *slot ? *slot : "");
    s->modal = false;
    if (s->retired) { free(value); release(s); return; }
    if (!s->shown || !mail_inbox_offered()) { free(value); clear_private(s); repaint(s); return; }
    if (value) {
        free(*slot); *slot = value;
        // Any, Inbox, and Outside already drop the label. Applying a label is the other alternative.
        if (act == ACT_LABEL) s->filter.inbox = -1;
        reload(s);
    } else { if (!s->accounts_loaded || !s->loaded) load_accounts(s); if (s->selected_id && !s->body.id) load_body(s); arm(s); }
}

typedef struct {
    int primary, *members, day;
    size_t count;
    bool unread, starred, notification;
} Convo;
typedef struct { RECT mailbox, folder, group; } Anchors;

static int day_key(double ms) {
    if (ms <= 0) return 0;
    time_t when = (time_t)(ms / 1000);
    // A finite receivedAt past year 3000 still parses. localtime returns NULL for it, and copying that used to fault every layout.
    struct tm *t = localtime(&when);
    if (!t) return 0;
    return (t->tm_year + 1900) * 10000 + (t->tm_mon + 1) * 100 + t->tm_mday;
}
static char *day_title(int key) {
    if (!key) return xstrdup("UNDATED");
    time_t now = time(NULL);
    struct tm t = *localtime(&now);
    int today = (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
    time_t back = now - 86400;
    struct tm y = *localtime(&back);
    int yesterday = (y.tm_year + 1900) * 10000 + (y.tm_mon + 1) * 100 + y.tm_mday;
    if (key == today) return xstrdup("TODAY");
    if (key == yesterday) return xstrdup("YESTERDAY");
    static const char *mon[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    int month = (key / 100) % 100, day = key % 100;
    if (month < 1 || month > 12) return xstrdup("UNDATED");
    return xstrfmt("%s %d", mon[month - 1], day);
}
static bool is_notification(const MailMessage *m) {
    static const char *const labels[] = { "CATEGORY_PROMOTIONS", "CATEGORY_UPDATES", "CATEGORY_SOCIAL", "CATEGORY_FORUMS" };
    for (size_t i = 0; i < m->label_count; i++)
        for (size_t k = 0; k < sizeof labels / sizeof *labels; k++)
            if (m->labels[i] && str_eq(m->labels[i], labels[k])) return true;
    const char *s = m->sender ? m->sender : "";
    return str_icontains(s, "noreply") || str_icontains(s, "no-reply") || str_icontains(s, "donotreply")
        || str_icontains(s, "notifications@") || str_icontains(s, "notify@") || str_icontains(s, "mailer-daemon");
}
static bool same_thread(const MailMessage *a, const MailMessage *b) {
    return a->account_id == b->account_id && !str_empty(a->thread_id) && str_eq(a->thread_id, b->thread_id);
}
static void free_convos(Convo *rows, size_t n) {
    if (!rows) return;
    for (size_t i = 0; i < n; i++) free(rows[i].members);
    free(rows);
}
static void build_convos(const Inbox *s, Convo **out, size_t *count) {
    *out = NULL; *count = 0;
    if (!s->messages.count) return;
    bool *used = xcalloc(s->messages.count, sizeof *used);
    Convo *rows = NULL; size_t n = 0, cap = 0;
    for (size_t i = 0; i < s->messages.count; i++) {
        if (used[i]) continue;
        if (n == cap) { cap = cap ? cap * 2 : 8; rows = xrealloc(rows, cap * sizeof *rows); }
        Convo *c = &rows[n++];
        memset(c, 0, sizeof *c);
        c->primary = (int)i;
        c->day = day_key(s->messages.messages[i].received_at);
        size_t mc = 0, mcap = 4;
        int *members = xcalloc(mcap, sizeof *members);
        for (size_t k = i; k < s->messages.count; k++) {
            if (k != i && (s->group != GROUP_THREADS || !same_thread(&s->messages.messages[i], &s->messages.messages[k]))) continue;
            used[k] = true;
            if (mc == mcap) { mcap *= 2; members = xrealloc(members, mcap * sizeof *members); }
            members[mc++] = (int)k;
            if (!s->messages.messages[k].is_read) c->unread = true;
            if (s->messages.messages[k].is_starred) c->starred = true;
        }
        for (size_t a = 0, b = mc; a + 1 < b; a++) { b--; int tmp = members[a]; members[a] = members[b]; members[b] = tmp; }
        c->members = members; c->count = mc;
        c->notification = is_notification(&s->messages.messages[c->primary]);
    }
    free(used);
    *out = rows; *count = n;
}
static bool convo_shown(const Inbox *s, const Convo *c) {
    if (s->kind == KIND_ALL) return true;
    if (s->kind == KIND_NOTES) return c->notification;
    return !c->notification;
}
static bool convo_open(const Inbox *s, const Convo *c) {
    if (!s->selected_id) return false;
    for (size_t i = 0; i < c->count; i++) {
        const MailMessage *m = &s->messages.messages[c->members[i]];
        if (m->account_id == s->selected_account && str_eq(m->id, s->selected_id)) return true;
    }
    return false;
}
static int convo_primary_for(const Inbox *s, int message) {
    Convo *rows; size_t n; build_convos(s, &rows, &n);
    int primary = message;
    for (size_t i = 0; i < n; i++) for (size_t k = 0; k < rows[i].count; k++)
        if (rows[i].members[k] == message) primary = rows[i].primary;
    free_convos(rows, n);
    return primary;
}
static const Convo *convo_for_selection(const Inbox *s, const Convo *rows, size_t n) {
    for (size_t i = 0; i < n; i++) if (convo_open(s, &rows[i])) return &rows[i];
    return NULL;
}
static void initials_of(const char *name, char out[3]) {
    out[0] = out[1] = out[2] = 0;
    int n = 0; bool start = true;
    for (const unsigned char *p = (const unsigned char *)(name ? name : ""); *p && n < 2; p++) {
        if (*p == ' ' || *p == '<' || *p == '@' || *p == '.' || *p == '-' || *p == '_') { start = true; continue; }
        if (start && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) {
            out[n++] = (char)(*p >= 'a' ? *p - 32 : *p);
            start = false;
        }
    }
    if (!n) out[0] = '?';
}
static void sender_parts(const char *sender, char **name, char **email) {
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
static char *account_badge(const MailAccount *a) {
    const char *label = a && !str_empty(a->label) ? a->label : NULL;
    if (label && strlen(label) <= 4 && !strchr(label, ' ')) {
        char *u = xstrdup(label);
        for (char *p = u; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
        return u;
    }
    char ini[3]; initials_of(label ? label : a && a->email ? a->email : "", ini);
    return xstrdup(ini);
}
static char *list_age(double ms) {
    if (ms <= 0) return xstrdup("");
    char *rel = format_relative((time_t)(ms / 1000));
    size_t n = strlen(rel);
    if (n > 4 && strcmp(rel + n - 4, " ago") == 0) rel[n - 4] = 0;
    return rel;
}
static const char *subject_of(const MailMessage *m) { return str_empty(m->subject) ? "(No subject)" : m->subject; }
static bool row_open(const Inbox *s, const MailMessage *m) {
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
static const char *older_label(const Inbox *s) {
    if (!s->messages.count || !s->messages.next_cursor) return "Load older messages";
    double oldest = 0;
    for (size_t i = 0; i < s->messages.count; i++) {
        double t = s->messages.messages[i].received_at;
        if (t > 0 && (oldest <= 0 || t < oldest)) oldest = t;
    }
    time_t now = time(NULL);
    struct tm today = *localtime(&now);
    int key = (today.tm_year + 1900) * 10000 + (today.tm_mon + 1) * 100 + today.tm_mday;
    return day_key(oldest) == key ? "Load yesterday" : "Load older messages";
}
static void open_provider(Inbox *s) {
    if (s->body.id && mail_account_readable(&s->accounts, s->body.account_id) && mail_message_web_url_safe(s->body.web_url))
        open_web_url(s->body.web_url);
}
static void cycle_account(Inbox *s) {
    int cap = (int)s->accounts.count + 1;
    int *ids = xcalloc((size_t)cap, sizeof *ids), n = 0, cur = 0;
    ids[n++] = 0;
    for (size_t i = 0; i < s->accounts.count; i++)
        if (mail_account_readable(&s->accounts, s->accounts.accounts[i].id)) ids[n++] = s->accounts.accounts[i].id;
    for (int i = 0; i < n; i++) if (ids[i] == s->filter.account) cur = i;
    s->filter.account = ids[(cur + 1) % n];
    free(ids);
    reload(s);
}
static void move_focus(Inbox *s, int delta) {
    Convo *rows; size_t n; build_convos(s, &rows, &n);
    int *shown = xcalloc(n ? n : 1, sizeof *shown); int sn = 0;
    for (size_t i = 0; i < n; i++) if (convo_shown(s, &rows[i])) shown[sn++] = rows[i].primary;
    free_convos(rows, n);
    if (!sn) { free(shown); return; }
    int at = -1;
    for (int i = 0; i < sn; i++) if (shown[i] == s->focus_message) at = i;
    if (at < 0) at = delta > 0 ? 0 : sn - 1;
    else { at += delta; if (at < 0) at = 0; if (at >= sn) at = sn - 1; }
    s->focus_message = shown[at];
    s->reveal_focus = true; s->reveal_open = false;
    free(shown);
    repaint(s);
}

static void action(Screen *base, int act, intptr_t arg, POINT pt) {
    (void)pt; Inbox *s = (Inbox *)base;
    if (!s->shown || s->modal || !mail_inbox_offered()) return;
    if (act == ACT_MENU) {
        s->menu = s->menu == (int)arg ? MENU_NONE : (int)arg;
        if (s->base.pane) s->scroll_seen = pane_scroll_y(s->base.pane);
        repaint(s); return;
    }
    s->menu = MENU_NONE;
    if (act == ACT_Q || act == ACT_LABEL || act == ACT_THREAD) { edit_filter(s, act); return; }
    if (act == ACT_RESET) { mail_filter_free(&s->filter); s->group = GROUP_THREADS; reload(s); return; }
    if (act == ACT_ACCOUNT) {
        if (arg && !mail_account_readable(&s->accounts, (int)arg)) return;
        s->filter.account = (int)arg; reload(s); return;
    }
    if (act == ACT_KIND) { if (arg < KIND_PEOPLE || arg > KIND_ALL) return; s->kind = (int)arg; repaint(s); return; }
    if (act == ACT_GROUP) { if (arg != GROUP_THREADS && arg != GROUP_EACH) return; s->group = (int)arg; repaint(s); return; }
    if (act == ACT_UNREAD) { s->filter.unread = s->filter.unread == 1 ? -1 : 1; reload(s); return; }
    if (act == ACT_STARRED) { s->filter.starred = s->filter.starred == 1 ? -1 : 1; reload(s); return; }
    if (act == ACT_INBOX) {
        if (arg < -1 || arg > 1) return;
        s->filter.inbox = (int)arg; free(s->filter.label); s->filter.label = NULL; reload(s); return;
    }
    if (act == ACT_SYNC) { if (ready(s)) reload(s); return; }
    if (act == ACT_CLOSE) { clear_selection(s); repaint(s); return; }
    if (act == ACT_PROVIDER) { open_provider(s); return; }
    if (act == ACT_DELETE_PROVIDER) {
        if (!s->body.id || !mail_account_readable(&s->accounts, s->body.account_id) || !mail_message_web_url_safe(s->body.web_url)) return;
        text_set(&s->limit_note, "Delete this message in the mailbox that opens. It leaves this list after the next mail sync.");
        open_provider(s); repaint(s); return;
    }
    if (act == ACT_MORE || act == ACT_RETRY_LIST) { if (!s->accounts_loaded) load_accounts(s); else load_list(s); return; }
    if (act == ACT_RETRY_ACCOUNTS) { load_accounts(s); return; }
    if (act == ACT_RETRY_BODY) { load_body(s); return; }
    if (act == ACT_SELECT && arg >= 0 && (size_t)arg < s->messages.count && store_supports("mail_message")) {
        const MailMessage *m = &s->messages.messages[arg];
        if (!mail_account_readable(&s->accounts, m->account_id)) return;
        int primary = convo_primary_for(s, (int)arg);
        clear_selection(s); s->selected_id = xstrdup(m->id); s->selected_account = m->account_id;
        s->focus_message = primary; s->reveal_open = true; s->reveal_focus = false; s->reveal_block = true;
        text_set(&s->limit_note, NULL);
        load_body(s); repaint(s);
    }
}
static void header(Screen *base, HeaderInfo *info) {
    (void)base; snprintf(info->title, sizeof info->title, "Mail"); info->subtitle[0] = 0;
}

typedef struct { char *shown; bool placeholder; } SearchData;
typedef struct { char *shown; int unread; bool on; } MailBtn;
typedef struct { char *shown; bool on, accent, caret; } ChipData;
typedef struct { wchar_t glyph; bool on, muted; } IconData;
typedef struct { char initials[3]; bool mine; } Avatar;
static void search_free(void *p) { SearchData *d = p; free(d->shown); free(d); }
static void mailbtn_free(void *p) { MailBtn *d = p; free(d->shown); free(d); }
static void chip_free(void *p) { ChipData *d = p; free(d->shown); free(d); }
static void paint_search(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    SearchData *d = it->data;
    bool hot = doc_item_hovered(doc, it);
    fill_round_rect(cv, rc, px(8), theme.field, hot ? theme.line_strong : theme.line);
    RECT g = { rc->left + px(6), rc->top, rc->left + px(28), rc->bottom };
    draw_glyph(cv, 0xE721, &g, FONT_ICON_SMALL, theme.muted);
    RECT slash = { rc->right - px(28), rc->top + px(6), rc->right - px(8), rc->bottom - px(6) };
    if (slash.left < g.right) slash.left = g.right;
    fill_round_rect(cv, &slash, px(4), theme.raise, theme.line);
    draw_text(cv, "/", &slash, FONT_CAPTION2, theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT t = { g.right + px(2), rc->top, slash.left - px(4), rc->bottom };
    draw_text(cv, d && d->shown ? d->shown : "", &t, FONT_CAPTION, d && d->placeholder ? theme.muted : theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void paint_mailbtn(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    MailBtn *d = it->data;
    bool hot = doc_item_hovered(doc, it);
    fill_round_rect(cv, rc, (rc->bottom - rc->top) / 2, hot || (d && d->on) ? theme.field : theme.canvas, theme.line);
    int left = rc->left + px(12);
    if (d && d->unread > 0) {
        fill_circle(cv, left + px(4), (rc->top + rc->bottom) / 2, px(4), theme.accent);
        left += px(16);
    }
    RECT t = { left, rc->top, rc->right - px(28), rc->bottom };
    if (d && d->unread > 0) {
        char num[16]; snprintf(num, sizeof num, "%d", d->unread > 999 ? 999 : d->unread);
        int nw = text_width(cv, num, FONT_CAPTION2) + px(12);
        RECT b = { rc->right - px(28) - nw, rc->top + px(7), rc->right - px(26), rc->bottom - px(7) };
        if (b.left < left) b.left = left;
        fill_round_rect(cv, &b, (b.bottom - b.top) / 2, theme.raise, theme.line);
        draw_text(cv, num, &b, FONT_CAPTION2, theme.ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        t.right = b.left - px(4);
    }
    draw_text(cv, d && d->shown ? d->shown : "", &t, FONT_CAPTION, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT chev = { rc->right - px(24), rc->top, rc->right - px(4), rc->bottom };
    draw_glyph(cv, 0xE70D, &chev, FONT_ICON_SMALL, theme.muted);
}
static void paint_chip(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    ChipData *d = it->data;
    bool hot = doc_item_hovered(doc, it) && it->action;
    COLORREF ink = d && d->accent ? theme.accent : d && d->on ? theme.ink : theme.muted;
    if (!d || !d->accent) {
        COLORREF fill = d && d->on ? theme.field : theme.canvas;
        COLORREF border = (d && d->on) || hot ? theme.line_strong : theme.line;
        fill_round_rect(cv, rc, (rc->bottom - rc->top) / 2, fill, border);
    }
    int pad = d && d->caret ? px(22) : px(12);
    RECT t = { rc->left + px(12), rc->top, rc->right - pad, rc->bottom };
    draw_text(cv, d && d->shown ? d->shown : "", &t, FONT_CAPTION, ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (d && d->caret) {
        RECT g = { rc->right - px(22), rc->top, rc->right - px(2), rc->bottom };
        draw_glyph(cv, 0xE70D, &g, FONT_ICON_SMALL, theme.muted);
    }
}
static void paint_icon(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    IconData *d = it->data;
    bool hot = doc_item_hovered(doc, it) && it->action;
    if (hot) fill_round_rect(cv, rc, px(8), theme.field, theme.line);
    COLORREF ink = d && d->on ? theme.accent : (!it->action || (d && d->muted)) ? theme.muted : theme.ink;
    draw_glyph(cv, d ? d->glyph : 0, rc, FONT_ICON_SMALL, ink);
}
static void paint_command(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    IconData *d = it->data;
    bool hot = doc_item_hovered(doc, it) && it->action;
    fill_round_rect(cv, rc, px(8), hot ? theme.field : theme.raise, theme.line);
    RECT g = { rc->left + px(6), rc->top, rc->left + px(26), rc->bottom };
    if (d && d->glyph) draw_glyph(cv, d->glyph, &g, FONT_ICON_SMALL, theme.ink);
    RECT t = { rc->left + (d && d->glyph ? px(26) : px(10)), rc->top, rc->right - px(8), rc->bottom };
    draw_text(cv, it->text ? it->text : "", &t, FONT_CAPTION, it->action ? theme.ink : theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void paint_dot(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc; (void)it;
    fill_circle(cv, (rc->left + rc->right) / 2, (rc->top + rc->bottom) / 2, px(3), theme.accent);
}
static void paint_count(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    fill_round_rect(cv, rc, px(8), theme.field, theme.line);
    RECT t = *rc;
    draw_text(cv, it->text ? it->text : "", &t, FONT_CAPTION2, theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
static void paint_tag(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    COLORREF fill = blend(theme.accent, theme.canvas, theme.dark ? 0.28 : 0.16);
    fill_round_rect(cv, rc, px(4), fill, fill);
    RECT t = *rc;
    draw_text(cv, it->text ? it->text : "", &t, FONT_TINY_SEMIBOLD, theme.accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
static void paint_avatar(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc; Avatar *a = it->data;
    int cx = (rc->left + rc->right) / 2, cy = (rc->top + rc->bottom) / 2, rad = (rc->right - rc->left) / 2;
    fill_circle(cv, cx, cy, rad, a && a->mine ? theme.accent : theme.field);
    RECT t = *rc;
    draw_text(cv, a ? a->initials : "?", &t, FONT_CAPTION_SEMIBOLD, a && a->mine ? theme.on_accent : theme.ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
typedef struct { bool on; } MenuRow;
static void paint_menu_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    MenuRow *d = it->data;
    bool hot = doc_item_hovered(doc, it);
    if ((d && d->on) || hot) fill_round_rect(cv, rc, px(6), theme.field, theme.field);
    RECT t = { rc->left + px(10), rc->top, rc->right - px(10), rc->bottom };
    draw_text(cv, it->text ? it->text : "", &t, FONT_CAPTION, it->action ? theme.ink : theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void add_icon(Doc *doc, const RECT *rc, wchar_t glyph, bool on, bool muted, const char *text, int action, const char *tip) {
    IconData *d = xcalloc(1, sizeof *d); d->glyph = glyph; d->on = on; d->muted = muted;
    Item *it = doc_item(doc, doc_add(doc, rc, paint_icon));
    it->data = d; it->free_data = free; it->text = xstrdup(text ? text : "");
    if (action) { it->action = action; it->hand = true; }
    if (tip) it->tip = xstrdup(tip);
}
static void add_command(Doc *doc, const RECT *rc, wchar_t glyph, const char *text, int action, const char *tip) {
    IconData *d = xcalloc(1, sizeof *d); d->glyph = glyph;
    Item *it = doc_item(doc, doc_add(doc, rc, paint_command));
    it->data = d; it->free_data = free; it->text = xstrdup(text ? text : "");
    if (action) { it->action = action; it->hand = true; }
    if (tip) it->tip = xstrdup(tip);
}
static int chip_span(Doc *doc, const char *shown, bool accent, bool caret) {
    int tw = text_width(doc->cv, shown, FONT_CAPTION) + px(24) + (caret ? px(14) : 0);
    if (caret && tw > px(280)) tw = px(280);
    if (!accent && tw < px(36)) tw = px(36);
    return tw;
}
// `max_w` clamps a chip that would otherwise paint past the row; 0 leaves the measured width.
static int add_chip(Doc *doc, int x, int y, int h, const char *text, const char *shown, bool on, bool accent, bool caret, int action, intptr_t arg, int max_w) {
    int tw = chip_span(doc, shown, accent, caret);
    if (max_w > 0 && tw > max_w) tw = max_w;
    RECT rc = { x, y, x + tw, y + h };
    ChipData *d = xcalloc(1, sizeof *d);
    d->shown = xstrdup(shown ? shown : ""); d->on = on; d->accent = accent; d->caret = caret;
    Item *it = doc_item(doc, doc_add(doc, &rc, paint_chip));
    it->data = d; it->free_data = chip_free; it->text = xstrdup(text ? text : "");
    if (action) { it->action = action; it->arg = arg; it->hand = true; }
    return tw;
}
static int menu_row(Doc *doc, int x, int y, int w, const char *text, bool on, int action, intptr_t arg) {
    RECT rc = { x, y, x + w, y + px(28) };
    MenuRow *d = xcalloc(1, sizeof *d); d->on = on;
    Item *it = doc_item(doc, doc_add(doc, &rc, paint_menu_row));
    it->data = d; it->free_data = free; it->text = xstrdup(text ? text : "");
    if (action) { it->action = action; it->arg = arg; it->hand = true; }
    return px(28);
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
        int span = chip_span(doc, specs[i].shown, specs[i].accent, specs[i].caret);
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
        int tw = add_chip(doc, cx, cy, fh, specs[i].text, specs[i].shown, specs[i].on, specs[i].accent, specs[i].caret, specs[i].action, specs[i].arg, widths[i]);
        if (specs[i].anchor) *specs[i].anchor = (RECT){ cx, cy, cx + tw, cy + fh };
        cx += tw + gap;
    }
    free(folder);
    return cy;
}

typedef struct { char *name, *meta; } AttachRow;
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
static void layout_chrome(Inbox *s, Doc *doc, int x, int w, const Convo *rows, size_t nrow, bool available, Anchors *anchors) {
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
    Item *mit = doc_item(doc, doc_add(doc, &mrc, paint_mailbtn));
    mit->data = mb; mit->free_data = mailbtn_free; mit->text = xstrdup("Mailbox");
    mit->action = ACT_MENU; mit->arg = MENU_MAILBOX; mit->hand = true;
    const char *placeholder = s->filter.account ? "Search this mailbox" : "Search sender, subject or snippet";
    SearchData *sd = xcalloc(1, sizeof *sd);
    sd->shown = xstrdup(s->filter.q ? s->filter.q : placeholder); sd->placeholder = !s->filter.q;
    RECT src = narrow ? (RECT){ x, y + h + gap, x + w, y + h * 2 + gap }
        : (RECT){ search_x, y, search_r, y + h };
    Item *sit = doc_item(doc, doc_add(doc, &src, paint_search));
    sit->data = sd; sit->free_data = search_free; sit->text = xstrdup("Search");
    sit->action = ACT_Q; sit->hand = true; sit->tip = xstrdup("Search sender, subject or snippet");
    char *sync = sync_label(s);
    RECT sy = { search_r + gap, y, x + w - refresh_w - gap, y + h };
    if (!narrow) doc_text_at(doc, &sy, sync, FONT_CAPTION, theme.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    free(sync);
    RECT rr = { x + w - refresh_w, y, x + w, y + h };
    add_icon(doc, &rr, 0xE72C, false, !available, "Refresh", available ? ACT_SYNC : 0, "Refresh mail");
    char *pc = xstrfmt("People %d", people), *nc = xstrfmt("Notifications %d", notes), *ac = xstrfmt("All %d", people + notes);
    int fh = px(30), fy = y + h + px(10) + (narrow ? h + gap : 0);
    int widths[3];
    const char *caps[3] = { pc, nc, ac };
    const char *names[3] = { "People", "Notifications", "All" };
    int total = 0;
    for (int i = 0; i < 3; i++) { widths[i] = chip_span(doc, caps[i], false, false); total += widths[i]; }
    int sx = x;
    for (int i = 0; i < 3; i++) {
        RECT rc = { sx, fy, sx + widths[i], fy + fh };
        ChipData *d = xcalloc(1, sizeof *d);
        d->shown = xstrdup(caps[i]); d->on = s->kind == i;
        Item *it = doc_item(doc, doc_add(doc, &rc, paint_chip));
        it->data = d; it->free_data = chip_free; it->text = xstrdup(names[i]);
        it->action = ACT_KIND; it->arg = i; it->hand = true;
        sx += widths[i] + px(4);
    }
    free(pc); free(nc); free(ac);
    int filter_y = layout_filters(s, doc, x, w, fy, fh, total + px(8), anchors);
    doc->y = filter_y + fh + px(12);
}
static int layout_menu(Inbox *s, Doc *doc, const Anchors *anchors) {
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
        y += menu_row(doc, x + px(4), y, mw - px(8), "All mailboxes", s->filter.account == 0, ACT_ACCOUNT, 0);
        for (size_t i = 0; i < s->accounts.count; i++) {
            const MailAccount *a = &s->accounts.accounts[i];
            bool readable = mail_account_readable(&s->accounts, a->id);
            const char *name = !str_empty(a->label) ? a->label : a->email ? a->email : "Mailbox";
            char *shown = readable ? xstrdup(a->email ? a->email : name) : xstrfmt("%s · sign in", a->email ? a->email : name);
            y += menu_row(doc, x + px(4), y, mw - px(8), shown, a->id == s->filter.account, readable ? ACT_ACCOUNT : 0, a->id);
            free(shown);
        }
    } else if (s->menu == MENU_FOLDER) {
        y += menu_row(doc, x + px(4), y, mw - px(8), "Any", s->filter.inbox < 0 && str_empty(s->filter.label), ACT_INBOX, -1);
        y += menu_row(doc, x + px(4), y, mw - px(8), "Inbox", s->filter.inbox == 1 && str_empty(s->filter.label), ACT_INBOX, 1);
        y += menu_row(doc, x + px(4), y, mw - px(8), "Outside inbox", s->filter.inbox == 0 && str_empty(s->filter.label), ACT_INBOX, 0);
        y += menu_row(doc, x + px(4), y, mw - px(8), "Exact label...", !str_empty(s->filter.label), ACT_LABEL, 0);
    } else {
        // The thread id is a server filter and does not change group. Check the group the rows actually use.
        y += menu_row(doc, x + px(4), y, mw - px(8), "Group threads and duplicates", s->group == GROUP_THREADS, ACT_GROUP, GROUP_THREADS);
        y += menu_row(doc, x + px(4), y, mw - px(8), "Separate messages", s->group == GROUP_EACH, ACT_GROUP, GROUP_EACH);
        y += menu_row(doc, x + px(4), y, mw - px(8), "Exact thread...", !str_empty(s->filter.thread), ACT_THREAD, 0);
    }
    y += px(6);
    doc->y = y;
    doc_box_end(doc, box, 0);
    doc->y = y - scroll;
    return doc->y;
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
static void layout_row(Inbox *s, Doc *doc, int x, int w, const Convo *c) {
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
    doc_text_at(doc, &sr, subject_of(m), open || c->unread ? FONT_BODY_SEMIBOLD : FONT_BODY, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    char *name, *email; sender_parts(m->sender, &name, &email); free(email);
    char *age = list_age(m->received_at);
    int age_w = age && *age ? text_width(doc->cv, age, FONT_CAPTION) : 0;
    RECT nr = { ix, top + px(8), ix + iw - (age_w ? age_w + px(8) : 0), top + px(8) + sender_h };
    doc_text_at(doc, &nr, name, c->unread || open ? FONT_BODY_SEMIBOLD : FONT_BODY, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    free(name);
    if (age_w) {
        RECT ar = { ix + iw - age_w, nr.top, ix + iw, nr.bottom };
        doc_text_at(doc, &ar, age, FONT_CAPTION, theme.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    free(age);
    if (c->unread) { RECT dr = { x + px(6), nr.top, x + px(18), nr.bottom }; doc_add(doc, &dr, paint_dot); }
    if (chip) {
        RECT cr = { ix + iw - chip_w, subject_y + px(1), ix + iw, subject_y + subject_h - px(1) };
        Item *it = doc_item(doc, doc_add(doc, &cr, paint_count));
        it->text = chip;
    }
    if (has_snip) {
        const MailAccount *a = mail_account_find(&s->accounts, m->account_id);
        char *badge = account_badge(a);
        int bw = text_width(doc->cv, badge, FONT_TINY_SEMIBOLD) + px(10);
        if (bw < px(24)) bw = px(24);
        int sy = subject_y + subject_h + px(2);
        RECT br = { ix, sy, ix + bw, sy + snip_h };
        Item *bt = doc_item(doc, doc_add(doc, &br, paint_tag));
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
static void layout_list(Inbox *s, Doc *doc, const Convo *rows, size_t n, int x, int w, bool available) {
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
        if (!convo_shown(s, &rows[i])) continue;
        if (rows[i].day != last_day) {
            int convos = 0, messages = 0;
            // A hidden conversation between two shown ones on this day is skipped. The tally stops only when the day changes.
            for (size_t k = i; k < n && rows[k].day == rows[i].day; k++) {
                if (!convo_shown(s, &rows[k])) continue;
                convos++;
                messages += (int)rows[k].count;
            }
            layout_day(doc, x, w, rows[i].day, convos, messages);
            last_day = rows[i].day;
        }
        layout_row(s, doc, x, w, &rows[i]);
    }
    if (s->messages.next_cursor) {
        const char *label = older_label(s);
        int bw = text_width(doc->cv, label, FONT_FOOTNOTE) + px(28);
        int bx = x + (w - bw) / 2; if (bx < x) bx = x;
        doc_space(doc, px(8));
        doc_button(doc, bx, 0, label, BUTTON_BORDERED, ACT_MORE, 0, available && !s->list_read);
    }
}
static void layout_block(Inbox *s, Doc *doc, int x, int w, int index) {
    const MailMessage *m = &s->messages.messages[index];
    bool selected = row_open(s, m);
    if (selected) { s->reader_block_y = doc->y; s->reader_block_first = s->reader_blocks == 0; }
    s->reader_blocks++;
    const MailMessage *full = selected && s->body.id ? &s->body : m;
    char *name, *email; sender_parts(full->sender && *full->sender ? full->sender : m->sender, &name, &email);
    const MailAccount *account = mail_account_find(&s->accounts, m->account_id);
    bool mine = account && account->email && !str_empty(email) && str_ieq(email, account->email);
    int y = doc->y, av = px(32);
    Avatar *ad = xcalloc(1, sizeof *ad);
    initials_of(name, ad->initials); ad->mine = mine;
    RECT ar = { x, y, x + av, y + av };
    Item *ai = doc_item(doc, doc_add(doc, &ar, paint_avatar));
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
        doc_text(doc, x, w, str_empty(s->body.text) ? "No plain-text body is available in the synced copy." : s->body.text, FONT_BODY, theme.ink, DT_WORDBREAK);
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
static void layout_reader(Inbox *s, Doc *doc, const Convo *rows, size_t n, int x, int w, bool available) {
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
    add_icon(doc, &close_rc, 0xE711, false, false, "Close message", ACT_CLOSE, "Close message");
    bool url = s->body.id && mail_message_web_url_safe(s->body.web_url);
    if (url) {
        right -= bh;
        RECT rc = { right, bar, right + bh, bar + bh };
        add_icon(doc, &rc, 0xE8A7, false, false, "Open at provider", ACT_PROVIDER, "Open at provider");
    }
    if (url) {
        const char *label = "Delete at provider";
        int bw = text_width(doc->cv, label, FONT_CAPTION) + px(36);
        // Put the command on its own row in a stacked/narrow reader.
        int command_y = bar;
        if (bw + bh * 2 + px(16) > iw) { command_y += bh + px(8); right = ix + iw; }
        RECT rc = { right - px(8) - bw, command_y, right - px(8), command_y + bh };
        add_command(doc, &rc, 0xE74D, label, ACT_DELETE_PROVIDER, "Open this message in your mailbox to delete it; the synced list updates after the next sync.");
        bar = command_y;
    }
    doc->y = bar + bh + px(12);
    if (s->body_error) doc_notice(doc, ix, iw, s->body_error);
    if (s->body_read) doc_loading(doc, ix, iw, "Loading message body...");
    else if (!s->body.id) doc_button(doc, ix, 0, "Retry message", BUTTON_BORDERED, ACT_RETRY_BODY, 0, available && store_supports("mail_message"));
    if (s->body.id) {
        const Convo *c = convo_for_selection(s, rows, n);
        const MailMessage *head = c ? &s->messages.messages[c->primary] : &s->body;
        doc_text(doc, ix, iw, subject_of(head), FONT_TITLE, theme.ink, DT_WORDBREAK);
        doc_space(doc, px(6));
        const MailAccount *a = mail_account_find(&s->accounts, s->body.account_id);
        char *badge = account_badge(a);
        int count = c ? (int)c->count : 1;
        char *meta = xstrfmt("%d message%s", count, count == 1 ? "" : "s");
        int y = doc->y, mh = font_height(doc->cv, FONT_CAPTION);
        int bw = text_width(doc->cv, badge, FONT_TINY_SEMIBOLD) + px(12); if (bw < px(24)) bw = px(24);
        RECT br = { ix, y, ix + bw, y + mh };
        Item *bt = doc_item(doc, doc_add(doc, &br, paint_tag)); bt->text = badge;
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
            for (size_t i = 0; i < s->messages.count; i++) if (row_open(s, &s->messages.messages[i])) { index = (int)i; break; }
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
        doc_text(doc, ix, iw, "Reply, forward, archive and star messages in your mailbox.", FONT_CAPTION, theme.muted, DT_WORDBREAK);
        doc_space(doc, px(8));
        doc_button(doc, ix, 0, "Reply at provider", BUTTON_BORDERED, ACT_PROVIDER, 0, url);
    }
    if (s->limit_note) { doc_space(doc, px(8)); doc_text(doc, ix, iw, s->limit_note, FONT_FOOTNOTE, theme.muted, DT_WORDBREAK); }
    doc_box_end(doc, box, pad);
    doc_space(doc, px(12));
}
// Distance from the reader card top to the opened block, or -1 when this layout drew no block.
static int open_block_offset(const Inbox *s) {
    if (s->reader_block_y < 0 || s->reader_y < 0 || s->reader_block_y < s->reader_y) return -1;
    return s->reader_block_y - s->reader_y;
}
// A fresh open shows the block, and stays at the card top when that block is already first.
static int open_block_target(const Inbox *s) {
    int offset = open_block_offset(s);
    if (offset < 0 || s->reader_block_first) return 0;
    return offset;
}
// Older mail is laid out above the opened block, and a stacked list sits above the reader, so that block's y grows
// after the one-shot reveal. Add only that inserted height. Swapping wide and stacked layouts has no common origin,
// so the offset is taken from the block row_open matches. A throwaway layout must not record either: draw() lays
// the screen out into another document. A later paint in the same layout adds nothing, so a scroll the reader
// itself moved stays where the reader left it.
static void track_open_block(Inbox *s, Doc *doc, bool stack) {
    if (!s->base.pane || doc != pane_doc(s->base.pane)) return;
    int offset = open_block_offset(s);
    bool have = offset >= 0;
    bool revealing = s->reveal_focus || s->reveal_open || s->reveal_block;
    if (have && !revealing && s->block_held) {
        if (s->block_stacked != stack) {
            if (!stack) doc->sticky_scroll = open_block_target(s);
            else s->page_follow = 1;
        } else if (!stack) {
            int inserted = offset - s->block_offset;
            if (inserted > 0) doc->sticky_scroll += inserted;
        } else if (s->reader_block_y > s->block_page) {
            s->page_follow = 2;
            s->page_delta = s->reader_block_y - s->block_page;
        }
    }
    if (have) {
        s->block_held = true;
        s->block_offset = offset;
        s->block_page = s->reader_block_y;
        s->block_stacked = stack;
    }
}
static void layout(Screen *base, Doc *doc) {
    Inbox *s = (Inbox *)base; int x = px(16), w = doc->width - x * 2;
    s->page_follow = 0;
    if (w < px(120)) w = px(120);
    s->focus_y = -1; s->open_y = -1; s->reader_y = -1; s->reader_block_y = -1; s->reader_blocks = 0; s->reader_block_first = false; s->stacked = false;
    doc_space(doc, px(12));
    if (!mail_inbox_offered()) { clear_private(s); doc_notice(doc, x, w, "Mail needs an Admin token and the deployed account and message-list routes."); return; }
    if (!store_supports("mail_message")) clear_selection(s);
    bool available = ready(s);
    Convo *rows = NULL; size_t nrow = 0;
    build_convos(s, &rows, &nrow);
    Anchors anchors; memset(&anchors, 0, sizeof anchors);
    layout_chrome(s, doc, x, w, rows, nrow, available, &anchors);
    if (s->notice) { doc_notice(doc, x, w, s->notice); doc_space(doc, px(8)); }
    if (g_store.mail_retry_until > GetTickCount64()) { doc_notice(doc, x, w, "Mail cooldown is active. Retry buttons become available when it ends."); doc_space(doc, px(8)); }
    if (s->account_error) { doc_notice(doc, x, w, s->account_error); doc_button(doc, x, 0, "Retry account status", BUTTON_BORDERED, ACT_RETRY_ACCOUNTS, 0, available && !s->account_read); doc_space(doc, px(8)); }
    int pin_at = (int)doc->count; s->chrome_bottom = doc->y;
    if (!s->accounts_loaded) {
        if (s->account_read) doc_loading(doc, x, w, "Loading mail accounts...");
        int menu_bottom = layout_menu(s, doc, &anchors);
        if (doc->y < menu_bottom) doc->y = menu_bottom;
        doc_pin(doc, pin_at, s->chrome_bottom);
        free_convos(rows, nrow);
        return;
    }
    if (!s->accounts.count && !s->account_error) {
        doc_text(doc, x, w, "No mailbox is connected. Add Gmail or Outlook in Settings, under Mail.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
        doc_space(doc, px(8));
    }
    bool stack = w < px(680);
    int list_w = w, reader_x = x, reader_w = w, top = doc->y;
    if (!stack) {
        list_w = w * 34 / 100;
        if (list_w < px(250)) list_w = px(250);
        if (list_w > px(380)) list_w = px(380);
        if (list_w > w - px(280)) list_w = w - px(280);
        reader_x = x + list_w + px(14);
        reader_w = w - list_w - px(14);
    }
    layout_list(s, doc, rows, nrow, x, list_w, available);
    int list_bottom = doc->y;
    if (!stack) doc->y = top;
    s->stacked = stack;
    s->reader_y = doc->y;
    int reader_first = (int)doc->count;
    layout_reader(s, doc, rows, nrow, reader_x, reader_w, available);
    if (!stack) {
        // The reader stays beside the list and, once it is taller than the view, scrolls on its own. Opening or
        // moving in the list can no longer carry a short message above the viewport. The page is as long as the
        // list, or as the reader's window when the reader needs that.
        if (s->base.pane) {
            RECT view = pane_content_rect(s->base.pane);
            int room = (view.bottom - view.top) - top - px(12);
            if (room < 0) room = 0;
            int shown = doc->y - top;
            if (shown > room) shown = room;
            int page = top + shown;
            if (page < list_bottom) page = list_bottom;
            doc->y = page;
            // Members are oldest-first, so the message just opened is below older previews. Scroll to that block, and
            // keep the top when it is already first. The loading paint has no block yet; this stays set until the body
            // is laid out.
            if (s->reveal_block) doc->sticky_scroll = open_block_target(s);
            track_open_block(s, doc, false);
            doc_sticky(doc, reader_first, (int)doc->count, doc->y);
        } else if (list_bottom > doc->y) doc->y = list_bottom;
    } else track_open_block(s, doc, true);
    int content_bottom = doc->y;
    int menu_bottom = layout_menu(s, doc, &anchors);
    if (content_bottom > doc->y) doc->y = content_bottom;
    if (menu_bottom > doc->y) doc->y = menu_bottom;
    doc_pin(doc, pin_at, s->chrome_bottom);
    free_convos(rows, nrow);
}
static void refresh(Screen *base) { Inbox *s = (Inbox *)base; if (ready(s)) reload(s); }
static void timer(Screen *base, UINT id) {
    if (id != TIMER_INBOX) return;
    Inbox *s = (Inbox *)base; if (window(s)) KillTimer(window(s), id);
    load_accounts(s); repaint(s);
}
static void visible(Screen *base, bool shown) {
    Inbox *s = (Inbox *)base; s->shown = shown;
    if (shown) load_accounts(s); else clear_private(s);
}
static void activated(Screen *base, bool active) {
    Inbox *s = (Inbox *)base;
    if (!active) cancel(s);
    else if (s->shown && !s->modal) { load_accounts(s); if (s->selected_id && !s->body.id) load_body(s); }
}
static bool key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    Inbox *s = (Inbox *)base;
    if (ctrl || shift || !s->shown || s->modal || !mail_inbox_offered()) return false;
    if (vk == VK_ESCAPE) {
        if (s->menu) { s->menu = MENU_NONE; repaint(s); return true; }
        if (s->selected_id) { clear_selection(s); repaint(s); return true; }
        return false;
    }
    if (vk == 'J' || vk == VK_DOWN) { move_focus(s, 1); return true; }
    if (vk == 'K' || vk == VK_UP) { move_focus(s, -1); return true; }
    if (vk == VK_RETURN) { if (s->focus_message >= 0) action(base, ACT_SELECT, s->focus_message, (POINT){0}); return true; }
    if (vk == 'M') { open_provider(s); return true; }
    if (vk == 'A') { cycle_account(s); return true; }
    if (vk == 'R') { open_provider(s); return true; }
    if (vk == VK_DELETE) { action(base, ACT_DELETE_PROVIDER, 0, (POINT){0}); return true; }
    if (vk == VK_OEM_2) { edit_filter(s, ACT_Q); return true; }
    return false;
}
static void scrolled(Screen *base, bool at_bottom) {
    (void)at_bottom; Inbox *s = (Inbox *)base;
    if (s->in_scroll) return;
    s->in_scroll = true;
    if (s->reveal_focus || s->reveal_open || s->reveal_block) {
        // j/k keeps the focused list row under the chrome. Beside the list the reader scrolls to the opened block on
        // its own. A stacked reader has no sticky scroll, so opening one scrolls the page to that block, or to the
        // card top while the body is loading and when the block is already first.
        bool to_block = s->reveal_block && s->reader_block_y >= 0 && !s->reader_block_first;
        int row = s->reveal_focus ? s->focus_y : s->stacked ? (to_block ? s->reader_block_y : s->reader_y) : s->open_y;
        bool move = s->reveal_focus || s->reveal_open || (s->stacked && to_block);
        s->reveal_focus = false; s->reveal_open = false;
        if (s->reveal_block && (s->reader_block_y >= 0 || !s->body_read)) s->reveal_block = false;
        if (move && s->base.pane && row >= 0) {
            int target = row - s->chrome_bottom;
            if (target < 0) target = 0;
            pane_scroll_to(s->base.pane, target);
        }
    }
    if (s->page_follow && s->base.pane) {
        int follow = s->page_follow, delta = s->page_delta;
        s->page_follow = 0;
        if (follow == 1) {
            bool to_block = s->reader_block_y >= 0 && !s->reader_block_first;
            int row = to_block ? s->reader_block_y : s->reader_y;
            if (row >= 0) {
                int target = row - s->chrome_bottom;
                if (target < 0) target = 0;
                pane_scroll_to(s->base.pane, target);
            }
        } else if (follow == 2 && delta > 0) pane_scroll_to(s->base.pane, pane_scroll_y(s->base.pane) + delta + px(8));
    }
    if (s->base.pane && s->menu) {
        int y = pane_scroll_y(s->base.pane);
        if (y != s->scroll_seen) { s->scroll_seen = y; s->menu = MENU_NONE; repaint(s); }
    }
    s->in_scroll = false;
}
static int footer_height(Screen *base, int width) {
    (void)width; Inbox *s = (Inbox *)base;
    return s->shown && mail_inbox_offered() ? px(36) : 0;
}
static void keycap(Canvas *cv, int *x, int y, int h, const char *key) {
    int kw = text_width(cv, key, FONT_CAPTION2) + px(10);
    if (kw < px(18)) kw = px(18);
    RECT k = { *x, y, *x + kw, y + h };
    fill_round_rect(cv, &k, px(4), theme.field, theme.line);
    draw_text(cv, key, &k, FONT_CAPTION2, theme.ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    *x += kw + px(4);
}
static void keylabel(Canvas *cv, int *x, int y, int h, int right, const char *label) {
    int lw = text_width(cv, label, FONT_CAPTION2);
    if (*x + lw > right) return;
    RECT l = { *x, y, *x + lw, y + h };
    draw_text(cv, label, &l, FONT_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    *x += lw + px(12);
}
static void footer_paint(Screen *base, Canvas *cv, const RECT *rc) {
    (void)base;
    fill_rect(cv, rc, theme.canvas);
    draw_line(cv, rc->left, rc->top, rc->right, rc->top, theme.line);
    int h = px(18), y = rc->top + (rc->bottom - rc->top - h) / 2, x = rc->left + px(14);
    keycap(cv, &x, y, h, "j"); keycap(cv, &x, y, h, "k"); keylabel(cv, &x, y, h, rc->right, "move");
    keycap(cv, &x, y, h, "Enter"); keylabel(cv, &x, y, h, rc->right, "open");
    keycap(cv, &x, y, h, "Esc"); keylabel(cv, &x, y, h, rc->right, "close");
    keycap(cv, &x, y, h, "r"); keylabel(cv, &x, y, h, rc->right, "reply at provider");
    keycap(cv, &x, y, h, "Del"); keylabel(cv, &x, y, h, rc->right, "delete at provider");
    keycap(cv, &x, y, h, "m"); keylabel(cv, &x, y, h, rc->right, "open at provider");
    keycap(cv, &x, y, h, "a"); keylabel(cv, &x, y, h, rc->right, "switch mailbox");
}
static void destroy(Screen *base) {
    Inbox *s = (Inbox *)base; clear_private(s); s->shown = false;
    if (s->modal) { s->retired = true; return; }
    release(s);
}
static const ScreenVTable vt = { .destroy = destroy, .layout = layout, .header = header, .action = action,
    .refresh = refresh, .timer = timer, .visible = visible, .activated = activated, .key = key, .scrolled = scrolled,
    .footer_height = footer_height, .footer_paint = footer_paint };
Screen *mail_screen_new(void) {
    Inbox *s = xcalloc(1, sizeof *s); s->base.vt = &vt; s->base.id = xstrdup("mail");
    mail_filter_init(&s->filter); s->focus_message = -1; s->focus_y = s->open_y = s->reader_y = -1; return &s->base;
}
