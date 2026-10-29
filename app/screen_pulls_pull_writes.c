#include "screen_pulls_internal.h"

static void start_done(void *owner, Request *req);
static void merge_done(void *owner, Request *req);
static void edit_done(void *owner, Request *req);
static void update_branch_done(void *owner, Request *req);
static void decide_done(void *owner, Request *req);

static void start_done(void *owner, Request *req) {
    PullScreen *s = owner;
    s->busy = false;
    if (req->client != g_store.client || !str_eq(s->start_device, g_store.device.id)) { pane_relayout(s->base.pane); return; }
    if (!str_eq(json_str(json_get(req->args, "repo")), s->project.repo) || !store_can_manage()) { pane_relayout(s->base.pane); return; }
    if (req->ok) {
        set_string(&s->write_error, NULL);
        // The pull request stays shown; the new session joins its runs.
        if (store_supports("sessions")) { request_cancel(&s->req_sessions); Json *a = json_object(); json_set_str(a, "repo", s->project.repo); store_call("sessions", a, 0, s, screen_pulls_sessions_done_pull, TAG_SESSIONS, &s->req_sessions); }
    } else {
        request_error_into(&s->write_error, req);
        // A refusal is definite; anything else may have started the session.
        if (request_outcome_unknown(req)) s->uncertain = true;
    }
    pane_relayout(s->base.pane);
}

void screen_pulls_start_action(PullScreen *s, const BoardAction *action, const char *input, const RuntimeChoice *runtime) {
    const char *branch = json_str(json_get(s->pr, "headRef"));
    if (s->busy || s->uncertain || !store_can_manage() || !branch) return;
    if (!str_empty(action->binary) && !runtime) return;
    set_string(&s->start_device, g_store.device.id);
    s->busy = true;
    char *op = board_action_operation(action);
    Json *args = board_action_arguments(action, s->project.repo, s->number, branch, input);
    if (runtime && runtime->provider_id) { Json *rt = runtime_choice_arguments(runtime); json_object_merge(args, rt); json_free(rt); }
    store_call(op, args, board_action_timeout_ms(action), s, start_done, TAG_START, &s->req_start);
    free(op);
    pane_relayout(s->base.pane);
}

static void merge_done(void *owner, Request *req) {
    PullScreen *s = owner;
    s->merging = false;
    if (!req->ok) {
        char *t = request_error_text(req);
        if (!request_outcome_unknown(req)) set_string(&s->merge_error, t);
        else { char *m = xstrfmt("%s The merge may still have completed; check the state above before trying again.", t); set_string(&s->merge_error, m); free(m); }
        free(t);
    } else set_string(&s->merge_error, NULL);
    request_cancel(&s->req_pull); screen_pulls_pull_load(s);
    pane_relayout(s->base.pane);
}

void screen_pulls_merge(PullScreen *s, const char *method) {
    const char *head = json_str(json_get(s->pr, "headSha")), *base_ref = json_str(json_get(s->pr, "baseRef"));
    if (s->merging || !head || !base_ref) return;
    s->merging = true; set_string(&s->merge_error, NULL);
    Json *args = json_object();
    json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number);
    json_set_str(args, "headSha", head); json_set_str(args, "baseRef", base_ref); json_set_str(args, "method", method);
    store_call("merge_pull", args, 0, s, merge_done, TAG_MERGE, &s->req_merge);
    pane_relayout(s->base.pane);
}

static void edit_done(void *owner, Request *req) {
    PullScreen *s = owner;
    s->editing = false;
    if (req->ok) {
        set_string(&s->edit_error, NULL);
        // The title and description show as saved at once; the labels and assignees come back with the board's row.
        const Json *pr = json_get(req->result, "pr");
        const char *title = json_str(json_get(pr, "title")), *body = json_str(json_get(pr, "body"));
        if (title && json_is_object(s->pr)) json_set_str(s->pr, "title", title);
        if (body && json_get(req->args, "body")) { set_string(&s->body, body); if (json_str(json_get(s->pr, "body"))) json_set_str(s->pr, "body", body); }
        screen_pulls_save_pull(s);
    } else request_error_into(&s->edit_error, req);   // an edit sets what it names, so trying again is safe
    request_cancel(&s->req_pull); screen_pulls_pull_load(s);
    pane_relayout(s->base.pane);
}

/// Sends `fields` (taken) as an edit of this pull request.
void screen_pulls_pull_edit(PullScreen *s, Json *fields) {
    if (!fields) return;
    if (s->editing) { json_free(fields); return; }
    s->editing = true; set_string(&s->edit_error, NULL);
    json_set_str(fields, "repo", s->project.repo); json_set_num(fields, "pr", s->number);
    store_call("update_pull", fields, 0, s, edit_done, TAG_EDIT, &s->req_edit);
    pane_relayout(s->base.pane);
}

static void update_branch_done(void *owner, Request *req) {
    PullScreen *s = owner;
    s->updating_branch = false;
    if (req->ok) {
        set_string(&s->merge_error, NULL);
        const char *base_ref = json_str(json_get(req->args, "baseRef"));
        char *note = xstrfmt("GitHub is merging %s into this branch; its new commit and checks show once it has.", base_ref ? base_ref : "the base");
        set_string(&s->branch_note, note); free(note);
    } else {
        set_string(&s->branch_note, NULL);
        char *t = request_error_text(req);
        if (!request_outcome_unknown(req)) set_string(&s->merge_error, t);
        else { char *m = xstrfmt("%s The update may still have been made; refresh before trying again.", t); set_string(&s->merge_error, m); free(m); }
        free(t);
    }
    request_cancel(&s->req_pull); screen_pulls_pull_load(s);
    pane_relayout(s->base.pane);
}

/// Merges the base into the branch on GitHub, at the head and base last read, once asked.
void screen_pulls_update_branch(PullScreen *s) {
    const char *head = json_str(json_get(s->pr, "headSha")), *base_ref = json_str(json_get(s->pr, "baseRef")), *head_ref = json_str(json_get(s->pr, "headRef"));
    if (s->updating_branch || s->merging || !head || !base_ref) return;
    char *title = xstrfmt("Update the branch of #%d?", s->number);
    char *message = xstrfmt("GitHub merges the latest changes from %s into %s, as a new commit on the branch. A session working on it needs to pull before it pushes again.", base_ref, head_ref ? head_ref : "its branch");
    s->dialog_open = true;
    bool ok = app_confirm(title, message, "Update branch", false);
    s->dialog_open = false;
    free(title); free(message);
    // The pull request may have been read again while the dialog was open.
    head = json_str(json_get(s->pr, "headSha")); base_ref = json_str(json_get(s->pr, "baseRef"));
    if (!ok || s->updating_branch || !head || !base_ref) return;
    s->updating_branch = true; set_string(&s->merge_error, NULL); set_string(&s->branch_note, NULL);
    Json *args = json_object();
    json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number);
    json_set_str(args, "headSha", head); json_set_str(args, "baseRef", base_ref);
    store_call("update_pull_branch", args, 0, s, update_branch_done, TAG_UPDATE_BRANCH, &s->req_update_branch);
    pane_relayout(s->base.pane);
}

static void decide_done(void *owner, Request *req) {
    PullScreen *s = owner;
    set_string(&s->deciding, NULL);
    if (req->ok) { json_free(s->findings); s->findings = json_clone(json_get(req->result, "findings")); set_string(&s->findings_error, NULL); }
    else { request_error_into(&s->findings_error, req); }
    pane_relayout(s->base.pane);
}

void screen_pulls_decide(PullScreen *s, const char *key, const char *decision) {
    if (s->deciding) return;
    set_string(&s->deciding, key);
    Json *args = json_object();
    json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number); json_set_str(args, "key", key);
    json_object_set(args, "decision", json_string_or_null(decision));
    store_call("finding_decision", args, 0, s, decide_done, TAG_DECIDE, &s->req_decide);
    pane_relayout(s->base.pane);
}
