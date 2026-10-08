#include "mail.h"
#include "board.h"
#include "str.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static double timestamp(const Json *j) { double n; return json_num(j, &n) && isfinite(n) && n > 0 ? n : 0; }
void mail_account_free(MailAccount *a) {
    free(a->provider); free(a->email); free(a->label); free(a->status); free(a->last_sync_error);
    memset(a, 0, sizeof *a);
}
bool mail_account_parse(const Json *j, MailAccount *a) {
    memset(a, 0, sizeof *a);
    int id = json_int_or(json_get(j, "id"), 0);
    const char *provider = json_str_nonempty(json_get(j, "provider"));
    const char *email = json_str_nonempty(json_get(j, "email"));
    if (!json_is_object(j) || id <= 0 || !provider || !email) return false;
    a->id = id; a->provider = xstrdup(provider); a->email = xstrdup(email);
    a->label = xstrdup(json_str(json_get(j, "label")));
    a->status = xstrdup(json_str(json_get(j, "status")));
    a->last_sync_error = xstrdup(json_str(json_get(j, "lastSyncError")));
    a->enabled = json_bool_is(json_get(j, "enabled"), true);
    a->syncing = json_bool_is(json_get(j, "syncing"), true);
    a->sync_days = json_int_or(json_get(j, "syncDays"), 30);
    a->messages = json_int_or(json_get(j, "messages"), 0); a->unread = json_int_or(json_get(j, "unread"), 0);
    a->last_sync_at = timestamp(json_get(j, "lastSyncAt"));
    a->created_at = timestamp(json_get(j, "createdAt")); a->updated_at = timestamp(json_get(j, "updatedAt"));
    return true;
}
void mail_accounts_free(MailAccounts *a) {
    for (size_t i = 0; i < a->count; i++) mail_account_free(&a->accounts[i]);
    free(a->accounts); free(a->default_label); memset(a, 0, sizeof *a);
}
bool mail_accounts_parse(const Json *j, MailAccounts *a) {
    memset(a, 0, sizeof *a);
    const Json *rows = json_get(j, "accounts"), *providers = json_get(j, "providers"), *defaults = json_get(j, "defaults");
    if (!json_is_array(rows) || !json_is_array(providers)) return false;
    a->accounts = xcalloc(json_count(rows), sizeof *a->accounts);
    for (size_t i = 0; i < json_count(rows); i++) {
        if (!mail_account_parse(json_at(rows, i), &a->accounts[a->count])) { mail_accounts_free(a); return false; }
        a->count++;
    }
    for (size_t i = 0; i < json_count(providers); i++) {
        const char *p = json_str(json_at(providers, i));
        if (str_eq(p, "gmail")) a->gmail = true;
        if (str_eq(p, "outlook")) a->outlook = true;
    }
    a->default_label = xstrdup(json_str(json_get(defaults, "label")));
    a->default_enabled = !json_bool_is(json_get(defaults, "enabled"), false);
    a->default_sync_days = json_int_or(json_get(defaults, "syncDays"), 30);
    if (a->default_sync_days < 1 || a->default_sync_days > 365) a->default_sync_days = 30;
    return true;
}
const MailAccount *mail_account_find(const MailAccounts *a, int id) {
    for (size_t i = 0; i < a->count; i++) if (a->accounts[i].id == id) return &a->accounts[i];
    return NULL;
}
bool mail_provider_available(const MailAccounts *a, const char *p) { return (str_eq(p, "gmail") && a->gmail) || (str_eq(p, "outlook") && a->outlook); }
Json *mail_settings_body(const char *label, bool enabled, const char *days) {
    char *trim = str_trim(days); char *end; long n = strtol(trim, &end, 10);
    bool ok = *trim && !*end && n >= 1 && n <= 365;
    for (const char *p = trim; *p; p++) if (*p < '0' || *p > '9') ok = false;
    free(trim);
    if (!ok) return NULL;
    Json *j = json_object(); json_set_str(j, "label", label ? label : "");
    json_set_bool(j, "enabled", enabled); json_set_num(j, "syncDays", (double)n); return j;
}
void mail_sign_in_free(MailSignIn *s) {
    free(s->state); free(s->redirect_uri); free(s->provider); mail_accounts_free(&s->before); memset(s, 0, sizeof *s);
}
static bool oauth_web_url(const char *uri) {
    if (!safe_web_url(uri)) return false;
    for (const char *p = uri; *p; p++) if ((unsigned char)*p <= 32 || *p == '\\' || (unsigned char)*p == 127) return false;
    return true;
}
static bool redirect_safe(const char *uri) {
    if (oauth_web_url(uri)) return !strchr(uri, '#') && !strchr(uri, '?');
    if (!str_has_prefix(uri, "http://")) return false;
    const char *host = uri + 7, *end;
    if (str_has_prefix(host, "127.0.0.1")) end = host + 9;
    else if (str_has_prefix(host, "localhost")) end = host + 9;
    else if (str_has_prefix(host, "[::1]")) end = host + 5;
    else return false;
    if (*end == ':') {
        const char *port = ++end; unsigned n = 0;
        while (*end >= '0' && *end <= '9') {
            n = n * 10 + (unsigned)(*end++ - '0');
            if (n > 65535) return false;
        }
        if (end == port || n == 0) return false;
    }
    if (*end && *end != '/') return false;
    for (const char *p = uri; *p; p++) if ((unsigned char)*p <= 32 || (unsigned char)*p == 127 || *p == '@' || *p == '?' || *p == '#' || *p == '\\') return false;
    return true;
}
bool mail_sign_in_parse(const Json *j, const char *provider, int id, const MailAccounts *before, MailSignIn *s) {
    memset(s, 0, sizeof *s);
    const char *state = json_str_nonempty(json_get(j, "state")), *redirect = json_str_nonempty(json_get(j, "redirectUri"));
    double expiry = timestamp(json_get(j, "expiresAt"));
    if (!oauth_web_url(json_str(json_get(j, "url"))) || !state || !redirect || !redirect_safe(redirect) || !expiry
        || json_bool_tristate(json_get(j, "finishesOnServer")) < 0 || !mail_provider_available(before, provider)) return false;
    s->state = xstrdup(state); s->redirect_uri = xstrdup(redirect); s->provider = xstrdup(provider);
    s->expires_at = expiry; s->account_id = id; s->server_finish = json_bool_is(json_get(j, "finishesOnServer"), true);
    s->before.count = before->count; s->before.accounts = xcalloc(before->count, sizeof *s->before.accounts);
    for (size_t i = 0; i < before->count; i++) {
        const MailAccount *a = &before->accounts[i]; MailAccount *b = &s->before.accounts[i];
        b->id = a->id; b->provider = xstrdup(a->provider); b->status = xstrdup(a->status);
    }
    return true;
}
static int hex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static char *decode(const char *p, size_t n) {
    Str out; str_init(&out);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c == '%') {
            if (i + 2 >= n || hex(p[i + 1]) < 0 || hex(p[i + 2]) < 0) { str_free(&out); return NULL; }
            c = (unsigned char)(hex(p[i + 1]) * 16 + hex(p[i + 2])); i += 2;
        } else if (c == '+') c = ' ';
        if (c < 32 || c == 127) { str_free(&out); return NULL; }
        str_appendc(&out, (char)c);
    }
    return out.data ? str_detach(&out) : xstrdup("");
}
Json *mail_sign_in_finish(const MailSignIn *s, const char *url, double now) {
    if (!s->state || s->server_finish || now >= s->expires_at || !isfinite(now) || !url || strchr(url, '#')) return NULL;
    const char *q = strchr(url, '?');
    if (!q) return NULL;
    size_t destination = (size_t)(q - url), expected = strlen(s->redirect_uri);
    const char *path = strchr(s->redirect_uri + (str_has_prefix(s->redirect_uri, "https://") ? 8 : 7), '/');
    // Browsers serialize an empty HTTP(S) path as /; nonempty paths still match exactly.
    if (!path || !path[1]) {
        if (path) expected--;
        if (destination && url[destination - 1] == '/') destination--;
    }
    if (destination != expected || strncmp(url, s->redirect_uri, expected)) return NULL;
    char *state = NULL, *code = NULL; bool ok = true;
    for (const char *p = q + 1; *p;) {
        const char *end = strchr(p, '&'); if (!end) end = p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (!eq) { ok = false; break; }
        char *key = decode(p, (size_t)(eq - p)), *value = decode(eq + 1, (size_t)(end - eq - 1));
        if (!key || !value) ok = false;
        else if (str_eq(key, "state")) { if (state) ok = false; else { state = value; value = NULL; } }
        else if (str_eq(key, "code")) { if (code) ok = false; else { code = value; value = NULL; } }
        else if (str_eq(key, "error")) ok = false;
        free(key); free(value); p = *end ? end + 1 : end;
    }
    Json *body = NULL;
    if (ok && str_eq(state, s->state) && !str_empty(code)) { body = json_object(); json_set_str(body, "state", state); json_set_str(body, "code", code); }
    free(state); free(code); return body;
}
bool mail_sign_in_completed(const MailSignIn *s, const MailAccounts *a) {
    if (!s->server_finish) return false;
    for (size_t i = 0; i < a->count; i++) {
        const MailAccount *current = &a->accounts[i], *old = mail_account_find(&s->before, current->id);
        if (s->account_id && current->id != s->account_id) continue;
        if (!str_eq(current->provider, s->provider) || !str_eq(current->status, "connected")) continue;
        if (!old || str_eq(old->status, "reauth")) return true;
    }
    return false;
}
const char *mail_error_message(int status, bool finishing, const char *detail) {
    if (status == 400 && !finishing && str_eq(detail, "Label too long")) return "Mailbox labels must be at most 200 characters after trimming whitespace.";
    switch (status) {
    case 400: return finishing ? "Sign-in expired, was already used, or was refused. Start sign-in again." : "The server refused these settings; check its OAuth and encryption configuration.";
    case 401: return "This token expired or was revoked. Reconnect to the server.";
    case 403: return "Mail account settings require an Admin token.";
    case 404: return "This mail route or account is no longer available. Refresh or reconnect to the server.";
    case 409: return finishing ? "A different mailbox was selected (HTTP 409). The original account is unchanged; start again with its mailbox, or connect the other mailbox separately." : "This account needs sign-in again (HTTP 409).";
    case 429: return "The server is rate limiting mail requests. Wait before trying again.";
    case 503: return "This mail provider is unavailable on the server (HTTP 503). Check the server configuration.";
    default: return "The request did not complete. Refresh account status before trying a write again.";
    }
}

bool mail_message_web_url_safe(const char *url) {
    if (!safe_web_url(url)) return false;
    const char *host = url + 8;
    size_t authority = strcspn(host, "/?#");
    for (size_t i = 0; i < authority; i++) {
        unsigned char c = (unsigned char)host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '.' || c == '-' || c == ':' || c == '[' || c == ']')) return false;
    }
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
        if (*p <= 32 || *p == 127 || *p == '\\') return false;
    return true;
}
static char *address_text(const Json *j) {
    const char *name = json_str_nonempty(json_get(j, "name")), *email = json_str_or(json_get(j, "address"), "");
    return name ? xstrfmt("%s <%s>", name, email) : xstrdup(email);
}
static char *addresses_text(const Json *rows) {
    Str text; str_init(&text);
    for (size_t i = 0; i < json_count(rows); i++) {
        char *a = address_text(json_at(rows, i));
        if (i) str_appendz(&text, ", ");
        str_appendz(&text, a); free(a);
    }
    return text.data ? str_detach(&text) : xstrdup("");
}
void mail_message_free(MailMessage *m) {
    free(m->id); free(m->thread_id); free(m->sender); free(m->to); free(m->cc); free(m->reply_to);
    free(m->subject); free(m->snippet); free(m->web_url); free(m->text);
    for (size_t i = 0; i < m->label_count; i++) free(m->labels[i]);
    free(m->labels);
    for (size_t i = 0; i < m->attachment_count; i++) {
        free(m->attachments[i].id); free(m->attachments[i].name); free(m->attachments[i].mime_type);
    }
    free(m->attachments); memset(m, 0, sizeof *m);
}
bool mail_message_parse(const Json *j, bool body, MailMessage *m) {
    memset(m, 0, sizeof *m);
    int account = json_int_or(json_get(j, "accountId"), 0);
    const char *id = json_str_nonempty(json_get(j, "id"));
    if (!json_is_object(j) || account <= 0 || !id || (body && !json_is_object(json_get(j, "body")))) return false;
    m->account_id = account; m->id = xstrdup(id); m->thread_id = json_dup_str(json_get(j, "threadId"));
    m->sender = address_text(json_get(j, "from")); m->to = addresses_text(json_get(j, "to"));
    m->cc = addresses_text(json_get(j, "cc")); m->reply_to = addresses_text(json_get(j, "replyTo"));
    m->subject = json_dup_str(json_get(j, "subject")); m->snippet = json_dup_str(json_get(j, "snippet"));
    m->received_at = timestamp(json_get(j, "receivedAt"));
    m->is_read = json_bool_is(json_get(j, "isRead"), true); m->in_inbox = json_bool_is(json_get(j, "inInbox"), true);
    m->is_starred = json_bool_is(json_get(j, "isStarred"), true);
    m->labels = json_dup_strings(json_get(j, "labels"), &m->label_count);
    const char *url = json_str(json_get(j, "webUrl"));
    if (mail_message_web_url_safe(url)) m->web_url = xstrdup(url);
    const Json *attachments = json_get(j, "attachments");
    if (json_is_array(attachments)) {
        m->attachment_count = json_count(attachments); m->attachments = xcalloc(m->attachment_count, sizeof *m->attachments);
        for (size_t i = 0; i < m->attachment_count; i++) {
            const Json *a = json_at(attachments, i); MailAttachment *v = &m->attachments[i];
            v->id = json_dup_str(json_get(a, "id")); v->name = json_dup_str(json_get(a, "name"));
            v->mime_type = json_dup_str(json_get(a, "mimeType")); v->size = timestamp(json_get(a, "size"));
        }
    }
    if (body) {
        const Json *b = json_get(j, "body"); m->text = json_dup_str(json_get(b, "text"));
        m->truncated = json_bool_is(json_get(b, "truncated"), true);
    }
    return true;
}
void mail_messages_free(MailMessages *m) {
    for (size_t i = 0; i < m->count; i++) mail_message_free(&m->messages[i]);
    free(m->messages); free(m->next_cursor); memset(m, 0, sizeof *m);
}
bool mail_messages_parse(const Json *j, MailMessages *m) {
    memset(m, 0, sizeof *m);
    const Json *rows = json_get(j, "messages"), *cursor = json_get(j, "nextCursor");
    if (!json_is_array(rows) || (!json_is_null(cursor) && !json_str_nonempty(cursor))) return false;
    m->messages = xcalloc(json_count(rows), sizeof *m->messages);
    for (size_t i = 0; i < json_count(rows); i++) {
        if (!mail_message_parse(json_at(rows, i), false, &m->messages[m->count])) { mail_messages_free(m); return false; }
        m->count++;
    }
    m->next_cursor = json_dup_str(cursor); return true;
}
const MailMessage *mail_message_find(const MailMessages *m, int account, const char *id) {
    for (size_t i = 0; i < m->count; i++)
        if (m->messages[i].account_id == account && str_eq(m->messages[i].id, id)) return &m->messages[i];
    return NULL;
}
static int message_order(const void *left, const void *right) {
    const MailMessage *a = left, *b = right;
    if (a->received_at != b->received_at) return a->received_at > b->received_at ? -1 : 1;
    if (a->account_id != b->account_id) return a->account_id > b->account_id ? -1 : 1;
    return strcmp(b->id, a->id);
}
void mail_messages_append(MailMessages *m, MailMessages *page) {
    m->messages = xrealloc(m->messages, (m->count + page->count) * sizeof *m->messages);
    for (size_t i = 0; i < page->count; i++) {
        MailMessage *row = &page->messages[i];
        if (mail_message_find(m, row->account_id, row->id)) mail_message_free(row);
        else { m->messages[m->count++] = *row; memset(row, 0, sizeof *row); }
    }
    if (m->count > 1) qsort(m->messages, m->count, sizeof *m->messages, message_order);
    free(m->next_cursor); m->next_cursor = page->next_cursor; page->next_cursor = NULL;
    mail_messages_free(page);
}
bool mail_account_readable(const MailAccounts *a, int id) {
    const MailAccount *account = mail_account_find(a, id);
    return account && str_eq(account->status, "connected");
}
bool mail_messages_prune(MailMessages *m, const MailAccounts *a) {
    size_t n = 0; bool changed = false;
    for (size_t i = 0; i < m->count; i++) {
        if (!mail_account_readable(a, m->messages[i].account_id)) { mail_message_free(&m->messages[i]); changed = true; }
        else {
            if (n != i) { m->messages[n] = m->messages[i]; memset(&m->messages[i], 0, sizeof *m->messages); }
            n++;
        }
    }
    m->count = n;
    if (changed) { free(m->next_cursor); m->next_cursor = NULL; }
    return changed;
}
void mail_filter_init(MailFilter *f) { memset(f, 0, sizeof *f); f->unread = f->inbox = f->starred = -1; }
void mail_filter_free(MailFilter *f) { free(f->q); free(f->label); free(f->thread); mail_filter_init(f); }
Json *mail_filter_args(const MailFilter *f, const char *cursor) {
    Json *args = json_object();
    if (f->account > 0) json_set_num(args, "account", f->account);
    if (!str_empty(f->q)) json_set_str(args, "q", f->q);
    if (!str_empty(f->label)) json_set_str(args, "label", f->label);
    if (!str_empty(f->thread)) json_set_str(args, "thread", f->thread);
    if (f->unread >= 0) json_set_num(args, "unread", f->unread);
    if (f->inbox >= 0) json_set_num(args, "inbox", f->inbox);
    if (f->starred >= 0) json_set_num(args, "starred", f->starred);
    if (!str_empty(cursor)) json_set_str(args, "cursor", cursor);
    json_set_num(args, "limit", 50); return args;
}
const char *mail_read_error(int status, bool detail) {
    switch (status) {
    case 400: return "The server refused these filters or cursor. Reset filters or refresh the list (search is limited to 200 characters).";
    case 401: return "This token expired or was revoked. Reconnect to the server.";
    case 403: return "Mail requires an Admin token. Reconnect with an authorized token.";
    case 404: return detail ? "This message or account was removed from the synced copy. Refresh the list." : "Mail is no longer available. Refresh account status or reconnect to the server.";
    case 409: return "This mailbox needs sign-in again. Check Mail account settings.";
    case 429: return "The server is rate limiting mail requests. Retry after the cooldown.";
    case 503: return "Mail is unavailable on the server. Retry after the cooldown.";
    default: return "Mail could not be loaded. Retry after the cooldown.";
    }
}
