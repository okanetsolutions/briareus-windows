#include "screen_mail_wake_internal.h"
#include <time.h>

const char *const mail_wake_operations[] = {"session_mail_wake", "settings_mail_accounts", "settings_projects", "set_session_mail_wake"};

bool mail_wake_current(MailWakeScreen *s) {
    if (s->client != g_store.client || !g_store.has_device || !store_supports("session_mail_wake")) return false;
    Json *now = device_json(&g_store.device); bool same = json_equal(now, s->device); json_free(now); return same;
}
const Json *mail_wake_entry(MailWakeScreen *s, int route) { return json_get(g_store.mail_wake_state, s->keys[route]); }
double mail_wake_deadline(MailWakeScreen *s, int route) { return json_num_or(json_get(mail_wake_entry(s, route), "until"), 0); }
static Json *entry(const char *key) {
    if (!g_store.mail_wake_state) g_store.mail_wake_state = json_object();
    if (!json_is_object(json_get(g_store.mail_wake_state, key))) json_object_set(g_store.mail_wake_state, key, json_object());
    return (Json *)json_get(g_store.mail_wake_state, key);
}
bool mail_wake_read_current(MailWakeScreen *s) {
    const Json *write = mail_wake_entry(s, MW_WRITE);
    return s->fresh[MW_READ] && s->record && s->read_epoch == json_num_or(json_get(write, "epoch"), 0)
        && !json_bool_is(json_get(write, "pending"), true);
}
static bool locked(MailWakeScreen *s) {
    const Json *e = mail_wake_entry(s, MW_WRITE);
    return json_bool_is(json_get(e, "pending"), true) || json_bool_is(json_get(e, "uncertain"), true);
}
bool mail_wake_editable(MailWakeScreen *s) {
    return s->shown && !s->modal && mail_wake_current(s) && s->draft.rules
        && !s->requests[MW_WRITE] && store_supports("set_session_mail_wake");
}
bool mail_wake_savable(MailWakeScreen *s) {
    if (!mail_wake_editable(s) || locked(s) || !mail_wake_dirty(&s->draft)) return false;
    for (int i = 0; i < MW_ROUTES; i++) {
        if (s->requests[i] || mail_wake_deadline(s, i) > (double)time(NULL)) return false;
        if (i != MW_WRITE && (!s->fresh[i] || !store_supports(mail_wake_operations[i]))) return false;
    }
    return true;
}
void mail_wake_repaint(MailWakeScreen *s) {
    if (s->base.pane) { pane_relayout(s->base.pane); pane_header_changed(s->base.pane); }
}
void mail_wake_cancel(MailWakeScreen *s) {
    s->generation++;
    for (int i = 0; i < MW_ROUTES; i++) request_cancel(&s->requests[i]);
}
typedef struct { Json *device; char *key, *write_key; double epoch, write_epoch; bool write, wake_read; } Completion;
static bool completion_current(Completion *c, Request *r) {
    if (!g_store.has_device || r->client != g_store.client) return false;
    Json *now = device_json(&g_store.device); bool same = json_equal(now, c->device); json_free(now); return same;
}
static void completed(void *ctx, Request *r) {
    Completion *c = ctx; r->auth_checked = true;
    if (completion_current(c, r)) {
        Json *e = entry(c->key);
        double epoch = json_num_or(json_get(e, "epoch"), 0);
        r->arg = c->epoch != epoch;
        if (c->wake_read) {
            const Json *w = json_get(g_store.mail_wake_state, c->write_key);
            if (c->write_epoch != json_num_or(json_get(w, "epoch"), 0) || json_bool_is(json_get(w, "pending"), true)) r->arg = 1;
        }
        if (!r->ok) {
            json_set_num(e, "epoch", epoch + 1);
            double retry = r->error.retry_after;
            if (r->error.status == 429 && retry < 0) retry = 60;
            if (isfinite(retry) && retry > 0) {
                double until = (double)time(NULL) + retry;
                if (until > json_num_or(json_get(e, "until"), 0)) json_set_num(e, "until", until);
            }
        }
        if (c->write) {
            json_set_bool(e, "pending", false);
            json_set_bool(e, "uncertain", r->cancelled || request_outcome_unknown(r));
            // A successful transport response is still uncertain until the owner validates its record.
            if (r->ok) json_set_bool(e, "uncertain", true);
        }
        if (!r->ok && api_error_unauthorized(&r->error)) store_invalidate_credentials(&r->error);
    }
    json_free(c->device); free(c->key); free(c->write_key); free(c); r->completion_ctx = NULL;
}
static void track(MailWakeScreen *s, Request *r, int route) {
    Completion *c = xcalloc(1, sizeof *c); c->key = xstrdup(s->keys[route]); c->device = json_clone(s->device);
    c->epoch = json_num_or(json_get(entry(c->key), "epoch"), 0); c->write = route == MW_WRITE;
    c->wake_read = route == MW_READ; c->write_key = xstrdup(s->keys[MW_WRITE]);
    c->write_epoch = json_num_or(json_get(mail_wake_entry(s, MW_WRITE), "epoch"), 0);
    r->completion_ctx = c; r->completed = completed;
}
static bool response_identity(MailWakeScreen *s, Request *r) {
    const char *id = json_str(json_get(r->result, "sessionId")), *repo = json_str(json_get(r->result, "repo"));
    return (!id || str_eq(id, session_id(&s->session))) && (!repo || str_eq(repo, session_repo(&s->session)));
}
static bool adopt(MailWakeScreen *s, Request *r, int route) {
    if (route == MW_ACCOUNTS) {
        MailAccounts next;
        if (!mail_accounts_parse(r->result, &next)) return false;
        mail_accounts_free(&s->accounts); s->accounts = next; return true;
    }
    if (route == MW_PROJECTS) { s->tools = mail_wake_tools(r->result, session_repo(&s->session)); return s->tools >= 0; }
    MailWake next = {0};
    if (!response_identity(s, r) || !mail_wake_read(&next, r->result)) return false;
    json_free(s->record); s->record = json_clone(r->result);
    bool preserve = s->draft.rules && (mail_wake_dirty(&s->draft) || locked(s));
    if (!preserve) { mail_wake_free(&s->draft); s->draft = next; mail_wake_fill_controls(s); }
    else mail_wake_free(&next);
    return true;
}
static void read_done(void *owner, Request *r) {
    MailWakeScreen *s = owner;
    if (!s->shown || s->modal || !mail_wake_current(s) || r->tag != s->generation) return;
    int route = str_eq(r->operation, mail_wake_operations[MW_READ]) ? MW_READ
        : str_eq(r->operation, mail_wake_operations[MW_ACCOUNTS]) ? MW_ACCOUNTS : MW_PROJECTS;
    bool ok = !r->arg && r->ok && adopt(s, r, route);
    s->fresh[route] = ok;
    if (ok && route == MW_READ) s->read_epoch = json_num_or(json_get(mail_wake_entry(s, MW_WRITE), "epoch"), 0);
    char *error = ok ? NULL : request_error_or_unexpected(r); set_string(&s->errors[route], error); free(error);
    mail_wake_repaint(s);
}
void mail_wake_load(MailWakeScreen *s) {
    if (!s->shown || s->modal || !g_store.active || !mail_wake_current(s) || s->requests[MW_WRITE]) return;
    for (int i = 0; i < MW_WRITE; i++) {
        if (!store_supports(mail_wake_operations[i])) {
            s->fresh[i] = false; set_string(&s->errors[i], "This token or server catalog does not allow this read."); continue;
        }
        if (s->requests[i]) continue;
        if (i == MW_READ && json_bool_is(json_get(mail_wake_entry(s, MW_WRITE), "pending"), true)) { s->fresh[i] = false; continue; }
        if (mail_wake_deadline(s, i) > (double)time(NULL)) {
            s->fresh[i] = false; set_string(&s->errors[i], "Read is cooling down after Retry-After; try again later."); continue;
        }
        Json *args = json_object(); if (i == MW_READ) json_set_str(args, "sessionId", session_id(&s->session));
        Request *r = store_call(mail_wake_operations[i], args, 0, s, read_done, s->generation, &s->requests[i]); track(s, r, i);
    }
    mail_wake_repaint(s);
}
static void write_done(void *owner, Request *r) {
    MailWakeScreen *s = owner;
    if (!s->shown || !mail_wake_current(s) || s->generation != r->tag) return;
    MailWake saved = {0};
    bool valid = r->ok && !r->arg && response_identity(s, r) && mail_wake_read(&saved, r->result);
    if (valid) valid = mail_wake_saved_matches(&saved, r->args);
    if (valid) {
        json_set_bool(entry(s->keys[MW_WRITE]), "uncertain", false);
        mail_wake_free(&s->draft); s->draft = saved; memset(&saved, 0, sizeof saved);
        json_free(s->record); s->record = json_clone(r->result); mail_wake_fill_controls(s);
        set_string(&s->notice, "Saved."); set_string(&s->errors[MW_WRITE], NULL);
        s->fresh[MW_READ] = true;
        s->read_epoch = json_num_or(json_get(mail_wake_entry(s, MW_WRITE), "epoch"), 0);
    } else {
        char *error = request_error_or_unexpected(r); set_string(&s->errors[MW_WRITE], error); free(error);
    }
    mail_wake_free(&saved); mail_wake_repaint(s);
}
void mail_wake_save(MailWakeScreen *s) {
    if (!mail_wake_savable(s)) return;
    char *error = NULL; Json *body = mail_wake_body(&s->draft, &s->accounts, s->tools, &error);
    set_string(&s->errors[MW_WRITE], error); free(error);
    if (!body) { mail_wake_repaint(s); return; }
    json_set_str(body, "sessionId", session_id(&s->session));
    mail_wake_cancel(s); set_string(&s->notice, NULL);
    Json *write = entry(s->keys[MW_WRITE]);
    json_set_num(write, "epoch", json_num_or(json_get(write, "epoch"), 0) + 1);
    json_set_bool(write, "pending", true);
    s->fresh[MW_READ] = false;
    Request *r = store_call("set_session_mail_wake", body, 0, s, write_done, s->generation, &s->requests[MW_WRITE]); track(s, r, MW_WRITE);
    mail_wake_repaint(s);
}
