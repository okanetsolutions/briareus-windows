#include "screen_findings_internal.h"

static void write_done(void *owner, Request *req);
static void write_start(FindingsScreen *s, int kind, const char *operation, const Session *session, int round, const char *key, Json *args);
static Json *verdicts_for(FindingsScreen *s, const Session *ses, const Json *held, bool completing);

// MARK: - Writing

static void write_done(void *owner, Request *req) {
    FindingsScreen *s = owner;
    int kind = s->write_kind; s->write_kind = WRITE_NONE;
    char *sid = s->write_session; s->write_session = NULL;
    char *key = s->write_key; s->write_key = NULL;
    char *title = s->write_title; s->write_title = NULL;
    int round = s->write_round;
    char *text = req->ok ? NULL : request_error_text(req);
    Card *card = screen_findings_card_get(s, sid);
    switch (kind) {
    case WRITE_COMPLETE:
        if (req->ok) {
            screen_findings_drop_round_drafts(s, sid, round);
            const Json *r = req->result;
            // A dismissal of somebody else's review says nothing more; anything else is worth a line once the card is gone.
            if (!json_is_set(json_get(r, "dismissed")) || json_is_set(json_get(r, "approved"))) {
                const Json *os = json_get(r, "session");
                const char *oid = json_str_nonempty(json_get(os, "id")), *ot = json_str_nonempty(json_get(os, "title"));
                bool danger; char *line = triage_outcome_text(r, &danger);
                screen_findings_outcome_set(s, oid ? oid : sid, ot ? ot : title, line, danger);
                free(line);
            }
        } else { set_string(&card->error, text); set_string(&s->unsent, sid); set_string(&s->unsent_title, title); s->unsent_round = round; }
        screen_findings_cycle_now(s);
        break;
    case WRITE_SAVE:
        if (req->ok) {
            const char *warning = json_str_nonempty(json_get(req->result, "warning")), *url = json_str(json_get(req->result, "url"));
            set_string(&card->saved, warning ? warning : "Saved"); card->saved_failed = warning != NULL;
            set_string(&card->saved_url, !warning && safe_web_url(url) ? url : NULL);
        } else { char *m = xstrfmt("Not saved: %s", text); set_string(&card->saved, m); free(m); card->saved_failed = true; set_string(&card->saved_url, NULL); }
        break;
    case WRITE_REPLY: {
        Replied *r = screen_findings_replied_get(s, sid, key);
        if (req->ok) {
            const char *url = json_str(json_get(req->result, "url"));
            set_string(&r->error, NULL); set_string(&r->url, safe_web_url(url) ? url : NULL);
            Draft *d = screen_findings_draft_find(s, sid, round, key);
            if (d) set_string(&d->reason, NULL);
        } else set_string(&r->error, text);
        break;
    }
    case WRITE_DELETE:
        if (req->ok) {
            const char *warning = json_str_nonempty(json_get(req->result, "warning"));
            if (warning) { char *m = xstrfmt("Deleted, but the review still declares it: %s", warning); set_string(&card->info, m); free(m); card->info_danger = true; }
            else { set_string(&card->info, json_is_set(json_get(req->result, "commentDeleted")) ? "Deleted from the review" : "The review no longer declares it; it had no comment of its own to delete"); card->info_danger = false; }
        } else { char *m = xstrfmt("Not deleted: %s", text); set_string(&card->error, m); free(m); }
        screen_findings_cycle_now(s);
        break;
    }
    free(text); free(sid); free(key); free(title);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}

/// One write at a time; the card it is on says what is happening until the answer comes.
static void write_start(FindingsScreen *s, int kind, const char *operation, const Session *session, int round, const char *key, Json *args) {
    if (s->req_write) { json_free(args); return; }
    s->write_kind = kind; s->write_round = round;
    set_string(&s->write_session, session_id(session)); set_string(&s->write_key, key); set_string(&s->write_title, session_display_title(session));
    json_set_str(args, "sessionId", session_id(session));
    Card *c = screen_findings_card_find(s, session_id(session));
    if (c && kind != WRITE_REPLY) set_string(&c->error, NULL);
    if (c && kind == WRITE_DELETE) set_string(&c->info, NULL);
    store_call(operation, args, 0, s, write_done, TAG_WRITE, &s->req_write);
    pane_relayout(s->base.pane);
}

bool screen_findings_writing(FindingsScreen *s, int kind, const char *sid, const char *key) {
    return s->req_write && s->write_kind == kind && str_eq(s->write_session, sid) && (!key || str_eq(s->write_key, key));
}

static Json *verdicts_for(FindingsScreen *s, const Session *ses, const Json *held, bool completing) {
    Json *verdicts = json_array();
    const Json *findings = json_get(held, "findings");
    int round = screen_findings_round_number(held);
    for (size_t i = 0; i < json_count(findings); i++) {
        const Json *f = json_at(findings, i);
        const char *key = json_str(json_get(f, "key"));
        if (!key) continue;
        const char *decision = screen_findings_decision_of(s, session_id(ses), round, f), *reason = screen_findings_reason_of(s, session_id(ses), round, f);
        Json *v = json_object();
        json_set_str(v, "key", key);
        // Completing rules on every finding: an unmarked one goes as optional, which the loop never offers again.
        if (*decision) json_set_str(v, "decision", decision);
        else if (completing) json_set_str(v, "decision", "optional");
        else json_object_set(v, "decision", json_null());
        if (*reason || !completing) json_set_str(v, "reason", reason);
        json_array_push(verdicts, v);
    }
    return verdicts;
}

void screen_findings_complete_round(FindingsScreen *s, size_t r) {
    const Session *ses = screen_findings_round_session(s, r);
    const Json *held = s->rounds[r].held;
    int round = screen_findings_round_number(held), pr = held_round_pr_number(held);
    bool mine = held_round_is_mine(held);
    char *sid = xstrdup(session_id(ses));
    Json *args = json_object();
    char *title, *message;
    if (mine) {
        size_t fixes = 0;
        const Json *findings = json_get(held, "findings");
        for (size_t i = 0; i < json_count(findings); i++) if (str_eq(screen_findings_decision_of(s, sid, round, json_at(findings, i)), "fix")) fixes++;
        json_object_set(args, "verdicts", verdicts_for(s, ses, held, true));
        const char *note = screen_findings_note_of(s, sid, round);
        if (*note) json_set_str(args, "note", note);
        title = fixes ? xstrfmt("Start a paid fix session for %zu finding%s?", fixes, fixes == 1 ? "" : "s") : xstrdup("Complete with nothing to fix?");
        message = fixes ? xstrfmt("Every verdict and comment is recorded on PR #%d; what is marked fix goes to the fix session.", pr)
                        : xstrfmt("Every verdict is recorded on PR #%d, which is approved and its loop closed.", pr);
    } else {
        title = xstrdup("Take this review off the queue?");
        message = xstrfmt("What it found stays on PR #%d for its author.", pr);
    }
    bool ok = app_confirm(title, message, "Complete", false);
    free(title); free(message);
    int again = ok ? screen_findings_find_round(s, sid, round) : -1;
    if (again < 0) { json_free(args); free(sid); return; }
    write_start(s, WRITE_COMPLETE, "complete_findings", screen_findings_round_session(s, (size_t)again), round, NULL, args);
    free(sid);
}

void screen_findings_save_round(FindingsScreen *s, size_t r) {
    const Session *ses = screen_findings_round_session(s, r);
    const Json *held = s->rounds[r].held;
    int round = screen_findings_round_number(held);
    Json *args = json_object();
    json_object_set(args, "verdicts", verdicts_for(s, ses, held, false));
    json_set_str(args, "note", screen_findings_note_of(s, session_id(ses), round));
    screen_findings_card_forget_saved(s, session_id(ses));
    write_start(s, WRITE_SAVE, "save_findings", ses, round, NULL, args);
}

void screen_findings_reply_on(FindingsScreen *s, size_t r, size_t finding) {
    const Session *ses = screen_findings_round_session(s, r);
    const Json *held = s->rounds[r].held;
    const Json *f = json_at(json_get(held, "findings"), finding);
    const char *key = json_str(json_get(f, "key"));
    if (!key) return;
    int round = screen_findings_round_number(held);
    char *sid = xstrdup(session_id(ses)), *fkey = xstrdup(key);
    const char *ft = json_str(json_get(f, "title"));
    char *typed = dialog_text(app_dialog_owner(), "Reply on this finding\xE2\x80\x99s thread", ft ? ft : "Reply", "Reply", screen_findings_reason_of(s, sid, round, f));
    int again = typed ? screen_findings_find_round(s, sid, round) : -1;
    if (again >= 0) {
        char *text = str_trim(typed);
        Draft *d = screen_findings_draft_get(s, sid, round, fkey);
        set_string(&d->reason, text);
        if (*text) {
            screen_findings_replied_drop(s, sid, fkey);
            Json *args = json_object(); json_set_str(args, "key", fkey); json_set_str(args, "text", text);
            write_start(s, WRITE_REPLY, "reply_finding", screen_findings_round_session(s, (size_t)again), round, fkey, args);
        }
        free(text);
        pane_relayout(s->base.pane);
    }
    free(typed); free(sid); free(fkey);
}

void screen_findings_delete_finding(FindingsScreen *s, size_t r, size_t finding) {
    const Session *ses = screen_findings_round_session(s, r);
    const Json *held = s->rounds[r].held;
    const Json *f = json_at(json_get(held, "findings"), finding);
    const char *key = json_str(json_get(f, "key"));
    if (!key) return;
    int round = screen_findings_round_number(held);
    char *sid = xstrdup(session_id(ses)), *fkey = xstrdup(key);
    const char *ft = json_str(json_get(f, "title"));
    char *message = xstrfmt("\xE2\x80\x9C%s\xE2\x80\x9D \xE2\x80\x94 its comment on PR #%d is deleted on GitHub and the review stops declaring it. This cannot be undone.", ft ? ft : "Finding", held_round_pr_number(held));
    bool ok = app_confirm("Delete this finding from the review?", message, "Delete from the review", true);
    free(message);
    int again = ok ? screen_findings_find_round(s, sid, round) : -1;
    if (again >= 0) {
        Json *args = json_object(); json_set_str(args, "key", fkey);
        write_start(s, WRITE_DELETE, "delete_finding", screen_findings_round_session(s, (size_t)again), round, fkey, args);
    }
    free(sid); free(fkey);
}
