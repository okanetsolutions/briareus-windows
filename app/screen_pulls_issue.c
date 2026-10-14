#include "screen_pulls_internal.h"

static void issue_destroy(Screen *base);
static void issue_start_done(void *owner, Request *req);
static void issue_close_done(void *owner, Request *req);
static void issue_edit_done(void *owner, Request *req);
static void issue_edit(IssueScreen *s, Json *fields);
static void issue_close(IssueScreen *s, POINT pt);
static void issue_refresh(Screen *base);
static void issue_action(Screen *base, int action, intptr_t arg, POINT pt);
static Json *link_json(const BoardLink *l);
static void issue_timer(Screen *base, UINT id);
static void issue_visible(Screen *base, bool shown);
static void issue_activated(Screen *base, bool active);

void screen_pulls_issue_subs_free(IssueScreen *s) {
    for (size_t i = 0; i < s->sub_count; i++) board_link_free(&s->subs[i]);
    free(s->subs); s->subs = NULL; s->sub_count = 0;
}

static void issue_destroy(Screen *base) {
    IssueScreen *s = (IssueScreen *)base;
    request_cancel(&s->req); request_cancel(&s->req_board); request_cancel(&s->req_runs); request_cancel(&s->req_close);
    request_cancel(&s->req_detail); request_cancel(&s->req_timeline); poller_stop(&s->poller); screen_pulls_list_retry_stop(&s->poller, &s->list_retry_armed);
    request_cancel(&s->req_edit); free(s->closed_reason);
    issue_summaries_free(s->board_issues, s->board_issue_count); pull_summaries_free(s->board_pulls, s->board_pull_count);
    sessions_free(s->runs, s->run_count);
    json_free(s->detail); json_free(s->events); screen_pulls_issue_subs_free(s);
    free(s->timeline_error); free(s->timeline_seen); free(s->timeline_want); free(s->detail_error);
    project_free(&s->project); issue_summary_free(&s->issue); free(s->write_error); free(s->edit_error); free(s->load_error);
    screen_release(base);
}

static void issue_start_done(void *owner, Request *req) {
    IssueScreen *s = owner;
    s->busy = false;
    if (req->ok) {
        // The new conversation joins the Sessions list on the way back.
        if (store_supports("sessions")) { request_cancel(&s->req_runs); Json *a = json_object(); json_set_str(a, "repo", s->project.repo); store_call("sessions", a, 0, s, screen_pulls_issue_runs_done, 0, &s->req_runs); }
        Session started;
        if (session_parse(json_get(req->result, "session"), &started)) { pane_relayout(s->base.pane); app_push_from(&s->base, conversation_screen_new(&started)); session_free(&started); return; }
    } else {
        request_error_into(&s->write_error, req);
        if (request_outcome_unknown(req)) s->uncertain = true;
    }
    pane_relayout(s->base.pane);
}

static void issue_close_done(void *owner, Request *req) {
    IssueScreen *s = owner;
    s->closing = false;
    if (req->ok) {
        s->closed = true; s->gone = false;
        const char *reason = json_str(json_get(json_get(req->result, "issue"), "stateReason"));
        set_string(&s->closed_reason, reason ? reason : json_str(json_get(req->args, "reason")));
        set_string(&s->write_error, NULL);
        // The board drops it; reading it again keeps the epic's counts and its siblings true.
        screen_pulls_issue_load(s, true);
    } else request_error_into(&s->write_error, req);   // closing twice only restates the reason, so trying again is safe
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}

static void issue_edit_done(void *owner, Request *req) {
    IssueScreen *s = owner;
    s->editing = false;
    if (req->ok) {
        set_string(&s->edit_error, NULL);
        // The title and description show as saved at once; reading it again brings the rest.
        const Json *issue = json_get(req->result, "issue");
        const char *title = json_str(json_get(issue, "title")), *body = json_str(json_get(issue, "body"));
        if (title) set_string(&s->issue.title, title);
        if (body && s->detail && json_get(req->args, "body")) json_set_str(s->detail, "body", body);
        request_cancel(&s->req_detail);   // one already under way may have been read before the edit
        screen_pulls_issue_load(s, true);
    } else request_error_into(&s->edit_error, req);   // an edit sets what it names, so trying again is safe
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}

/// Sends `fields` (taken) as an edit of this issue.
static void issue_edit(IssueScreen *s, Json *fields) {
    if (!fields) return;
    if (s->editing) { json_free(fields); return; }
    s->editing = true; set_string(&s->edit_error, NULL);
    json_set_num(fields, "issue", s->issue.number); json_set_str(fields, "repo", s->project.repo);
    store_call("update_issue", fields, 0, s, issue_edit_done, 0, &s->req_edit);
    pane_relayout(s->base.pane);
}

/// The ▾ menu: why it is closed, and whether a comment goes first. Then a confirmation naming what stays open.
static void issue_close(IssueScreen *s, POINT pt) {
    if (s->closing || s->closed) return;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Close as completed");
    AppendMenuW(menu, MF_STRING, 2, L"Close as not planned");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, 3, L"Close as completed with a comment\x2026");
    AppendMenuW(menu, MF_STRING, 4, L"Close as not planned with a comment\x2026");
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(s->base.pane), NULL);
    DestroyMenu(menu);
    if (chosen < 1) return;
    bool not_planned = chosen == 2 || chosen == 4;
    char *comment = NULL;
    if (chosen >= 3) {
        char *caption = xstrfmt("Comment on #%d before closing it", s->issue.number);
        comment = dialog_text(app_dialog_owner(), caption, "Comment", "Next", "");
        free(caption);
        if (!comment) return;
        if (str_empty(comment)) { free(comment); comment = NULL; }
    }
    int open = s->issue.sub_issues - s->issue.sub_issues_done;
    Str why; str_init(&why);
    if (open > 0) str_appendf(&why, "%d of its sub-issues %s still open and stay%s open. ", open, open == 1 ? "is" : "are", open == 1 ? "s" : "");
    if (screen_pulls_issue_run_active(s)) str_appendz(&why, "A session is still working on it; closing does not stop it. ");
    if (comment) str_appendz(&why, "Your comment is posted first.");
    char *title = xstrfmt("Close issue #%d as %s?", s->issue.number, not_planned ? "not planned" : "completed");
    bool ok = app_confirm(title, why.len ? why.data : NULL, "Close issue", false);
    free(title); str_free(&why);
    if (!ok || s->closed) { free(comment); return; }
    s->closing = true; set_string(&s->write_error, NULL);
    Json *args = json_object();
    json_set_num(args, "issue", s->issue.number); json_set_str(args, "repo", s->project.repo);
    json_set_str(args, "reason", not_planned ? "not_planned" : "completed");
    if (comment) json_set_str(args, "comment", comment);
    free(comment);
    store_call("close_issue", args, 0, s, issue_close_done, 0, &s->req_close);
    pane_relayout(s->base.pane);
}

static void issue_refresh(Screen *base) {
    IssueScreen *s = (IssueScreen *)base;
    request_cancel(&s->req_runs); request_cancel(&s->req_detail);
    set_string(&s->timeline_seen, NULL);   // the timeline is read again with it
    set_string(&s->edit_error, NULL);
    screen_pulls_issue_load(s, true);
    pane_relayout(base->pane); pane_header_changed(base->pane);
}

static void issue_action(Screen *base, int action, intptr_t arg, POINT pt) {
    IssueScreen *s = (IssueScreen *)base;
    switch (action) {
    case ACT_ISSUE_OPEN: open_web_url(s->issue.url); break;
    case ACT_ISSUE_COPY: if (safe_web_url(s->issue.url)) copy_to_clipboard(pane_hwnd(base->pane), s->issue.url); break;
    case ACT_ISSUE_REFRESH: issue_refresh(base); break;
    case ACT_ISSUE_CLOSE: issue_close(s, pt); break;
    case ACT_ISSUE_EDIT: {
        const char *body = json_str(json_get(s->detail, "body"));
        if (s->editing || !body) break;
        char *t = xstrdup(s->issue.title ? s->issue.title : ""), *b = xstrdup(body);
        issue_edit(s, screen_pulls_details_edit("issue", s->issue.number, t, b));
        free(t); free(b);
        break;
    }
    case ACT_ISSUE_ASSIGNEES: case ACT_ISSUE_LABELS: {
        if (s->editing) break;
        int number = s->issue.number;
        Json *list = action == ACT_ISSUE_LABELS ? screen_pulls_labels_edit(s->issue.labels, s->issue.label_count, number)
                                                : screen_pulls_assignees_edit(pane_hwnd(base->pane), pt, s->issue.assignees, s->issue.assignee_count, number);
        if (list) { Json *fields = json_object(); json_object_set(fields, action == ACT_ISSUE_LABELS ? "labels" : "assignees", list); issue_edit(s, fields); }
        pane_relayout(base->pane);
        break;
    }
    case ACT_ISSUE_PARENT: if (s->issue.has_parent) screen_pulls_issue_open_link(s, &s->issue.parent); break;
    case ACT_ISSUE_SUB_LINK: if ((size_t)arg < s->sub_count) screen_pulls_issue_open_link(s, &s->subs[arg]); break;
    case ACT_ISSUE_EVENT: if ((size_t)arg < json_count(s->events)) screen_pulls_event_open(s, json_at(s->events, (size_t)arg)); break;
    case ACT_ISSUE_PROJECT: open_web_url(json_str(json_get(json_at(json_get(s->detail, "projects"), (size_t)arg), "url"))); break;
    case ACT_ISSUE_MORE: if (s->next_page && !s->req_timeline) { screen_pulls_issue_timeline_load(s, s->next_page); pane_relayout(base->pane); } break;
    case ACT_ISSUE_SUB: if ((size_t)arg < s->board_issue_count) app_push_from(&s->base, issue_detail_screen_new(&s->project, &s->board_issues[arg])); break;
    case ACT_ISSUE_RUN: if ((size_t)arg < s->run_count) app_push_from(&s->base, conversation_screen_new(&s->runs[arg])); break;
    case ACT_ISSUE_PULL: {
        if ((size_t)arg >= s->issue.pull_count) break;
        screen_pulls_issue_open_pull(s, &s->issue.pulls[arg]);
        break;
    }
    case ACT_ISSUE_CHECKED: s->uncertain = false; set_string(&s->write_error, NULL); pane_relayout(base->pane); break;
    case ACT_ISSUE_START: {
        if (s->busy || s->uncertain) break;
        s->busy = true;
        char *prompt = issue_prompt(&s->issue, s->project.repo);
        Json *args = json_object();
        json_set_str(args, "repo", s->project.repo); json_set_str(args, "prompt", prompt); json_set_str(args, "activity", "issue");
        free(prompt);
        store_call("start_session", args, 0, s, issue_start_done, 0, &s->req);
        pane_relayout(base->pane);
        break;
    }
    }
}

static Json *link_json(const BoardLink *l) {
    Json *o = json_object();
    json_set_num(o, "number", l->number); json_set_str(o, "title", l->title); json_set_str(o, "url", l->url); json_set_str(o, "repo", l->repo);
    json_set_bool(o, "draft", l->draft); json_set_str(o, "state", l->state); json_set_str(o, "stateReason", l->state_reason);
    Json *labels = json_array(); for (size_t i = 0; i < l->label_count; i++) { Json *x = json_object(); json_set_str(x, "name", l->labels[i].name); json_set_str(x, "color", l->labels[i].color); json_array_push(labels, x); } json_object_set(o, "labels", labels);
    return o;
}

static void issue_timer(Screen *base, UINT id) {
    IssueScreen *s = (IssueScreen *)base;
    if (screen_pulls_list_retry_fired(&s->poller, &s->list_retry_armed, s->project.repo, id, false)) {
        if (screen_pulls_pulls_read(s->project.repo, s->fresh_pending, s, screen_pulls_issue_board_done, &s->req_board)) { s->fresh_pending = false; s->list_retry_read = true; }
        pane_header_changed(base->pane);
        return;
    }
    if (poller_fired(&s->poller, id)) { request_cancel(&s->req_runs); screen_pulls_issue_load(s, false); }
}

static void issue_visible(Screen *base, bool shown) {
    IssueScreen *s = (IssueScreen *)base;
    if (shown) { poller_start(&s->poller, base->pane, TIMER_POLL, 45000); screen_pulls_list_retry_arm(&s->poller, &s->list_retry_armed, s->project.repo); }
    else { poller_stop(&s->poller); screen_pulls_list_retry_stop(&s->poller, &s->list_retry_armed); s->list_retry_read = false; request_cancel(&s->req_board); request_cancel(&s->req_runs); request_cancel(&s->req_detail); }
}

static void issue_activated(Screen *base, bool active) { if (active) issue_visible(base, true); }

static const ScreenVTable issue_vt = {
    .destroy = issue_destroy, .layout = screen_pulls_issue_layout, .header = screen_pulls_issue_header, .action = issue_action, .timer = issue_timer,
    .visible = issue_visible, .refresh = issue_refresh, .activated = issue_activated,
};

Screen *issue_detail_screen_new(const Project *project, const IssueSummary *issue) {
    IssueScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &issue_vt; s->base.id = xstrfmt("issue:%s#%d", project->repo, issue->number);
    project_copy(&s->project, project);
    // A deep copy through JSON keeps the row's own parsing in one place.
    Json *j = json_object();
    json_set_num(j, "number", issue->number); json_set_str(j, "title", issue->title); json_set_str(j, "url", issue->url); json_set_str(j, "author", issue->author);
    json_set_str(j, "milestone", issue->milestone); json_set_num(j, "comments", issue->comments);
    Json *assignees = json_array(); for (size_t i = 0; i < issue->assignee_count; i++) json_array_push(assignees, json_string(issue->assignees[i])); json_object_set(j, "assignees", assignees);
    Json *labels = json_array(); for (size_t i = 0; i < issue->label_count; i++) { Json *l = json_object(); json_set_str(l, "name", issue->labels[i].name); json_set_str(l, "color", issue->labels[i].color); json_array_push(labels, l); } json_object_set(j, "labels", labels);
    struct { bool has; time_t when; const char *key; } dates[] = { { issue->has_created, issue->created_at, "createdAt" }, { issue->has_updated, issue->updated_at, "updatedAt" } };
    for (size_t k = 0; k < 2; k++) {
        if (!dates[k].has) continue;
        char iso[40]; struct tm *tm = gmtime(&dates[k].when);
        if (tm) { strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", tm); json_set_str(j, dates[k].key, iso); }
    }
    Json *sub = json_object(); json_set_num(sub, "total", issue->sub_issues); json_set_num(sub, "completed", issue->sub_issues_done); json_object_set(j, "subIssues", sub);
    if (issue->has_parent) json_object_set(j, "parent", link_json(&issue->parent));
    Json *pulls = json_array(); for (size_t i = 0; i < issue->pull_count; i++) json_array_push(pulls, link_json(&issue->pulls[i])); json_object_set(j, "pulls", pulls);
    issue_summary_parse(j, &s->issue);
    json_free(j);
    // The board as last saved fills the sub-issues and pull requests in before the first read answers.
    char *key = xstrfmt("pulls:%s", project->repo);
    Json *saved = cache_value(g_store.cache, key);
    free(key);
    if (saved) { screen_pulls_issue_board_show(s, saved, true); json_free(saved); }
    // So does the issue's own read, body and timeline, as last seen.
    if (store_supports("issue")) {
        key = screen_pulls_issue_cache_key(s, "issue"); saved = cache_value(g_store.cache, key); free(key);
        if (saved) { screen_pulls_issue_detail_show(s, json_get(saved, "issue")); json_free(saved); }
    }
    if (store_supports("issue_timeline")) {
        key = screen_pulls_issue_cache_key(s, "issue-timeline"); saved = cache_value(g_store.cache, key); free(key);
        if (saved) { screen_pulls_issue_timeline_show(s, saved, true); json_free(saved); }
    }
    return &s->base;
}
