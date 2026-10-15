#include "screen_panel_internal.h"

static void panel_save(PanelScreen *s);
static void panel_finish(PanelScreen *s);
static void findings_done(void *owner, Request *req);
static void pull_done(void *owner, Request *req);
static void panel_load(PanelScreen *s);
static void decide_done(void *owner, Request *req);
static void decide(PanelScreen *s, const char *key, const char *decision);
static void panel_destroy(Screen *base);
static void panel_layout(Screen *base, Doc *doc);
static void panel_header(Screen *base, HeaderInfo *info);
static void panel_action(Screen *base, int action, intptr_t arg, POINT pt);
static void panel_timer(Screen *base, UINT id);
static void panel_visible(Screen *base, bool shown);
static void panel_refresh(Screen *base);
static void panel_set_pull(PanelScreen *s, const char *repo, int number);

static void panel_save(PanelScreen *s) {
    if (json_is_null(s->pr)) return;
    Json *saved = json_object();
    json_object_set(saved, "pr", json_clone(s->pr));
    json_object_set(saved, "findings", json_clone(s->findings));
    json_object_set(saved, "row", json_null());
    char *key = xstrfmt("pull:%s#%d", s->project.repo, s->number);
    Json *old = cache_value(g_store.cache, key);
    if (old && !json_is_null(json_get(old, "row"))) json_object_set(saved, "row", json_clone(json_get(old, "row")));
    json_free(old);
    cache_store(g_store.cache, saved, key);
    json_free(saved); free(key);
}

static void panel_finish(PanelScreen *s) {
    if (s->req_pull || s->req_findings) return;
    panel_save(s);
    poller_finished(&s->poller, s->error != NULL, -1);
    pane_relayout(s->base.pane);
}

static void findings_done(void *owner, Request *req) {
    PanelScreen *s = owner;
    if (req->ok) { json_free(s->findings); s->findings = json_clone(json_get(req->result, "findings")); }
    panel_finish(s);
}

static void pull_done(void *owner, Request *req) {
    PanelScreen *s = owner;
    if (!req->ok) { request_error_into(&s->error, req); panel_finish(s); return; }
    json_free(s->pr); s->pr = json_clone(json_get(req->result, "pr"));
    set_string(&s->error, NULL);
    if (store_supports("findings")) {
        Json *args = json_object(); json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number);
        store_call("findings", args, 0, s, findings_done, TAG_FINDINGS, &s->req_findings);
    }
    panel_finish(s);
}

static void panel_load(PanelScreen *s) {
    if (!s->number) { poller_finished(&s->poller, false, -1); return; }
    if (json_is_null(s->pr)) {
        char *key = xstrfmt("pull:%s#%d", s->project.repo, s->number);
        Json *saved = cache_value(g_store.cache, key);
        if (saved) { json_free(s->pr); s->pr = json_clone(json_get(saved, "pr")); json_free(s->findings); s->findings = json_clone(json_get(saved, "findings")); json_free(saved); pane_relayout(s->base.pane); }
        free(key);
    }
    if (s->req_pull) return;
    Json *args = json_object(); json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number);
    store_call("pull", args, 0, s, pull_done, TAG_PULL, &s->req_pull);
}

static void decide_done(void *owner, Request *req) {
    PanelScreen *s = owner;
    set_string(&s->deciding, NULL);
    if (req->ok) { json_free(s->findings); s->findings = json_clone(json_get(req->result, "findings")); panel_save(s); }
    else { request_error_into(&s->error, req); }
    pane_relayout(s->base.pane);
}

static void decide(PanelScreen *s, const char *key, const char *decision) {
    if (s->deciding) return;
    set_string(&s->deciding, key);
    Json *args = json_object();
    json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number); json_set_str(args, "key", key);
    json_object_set(args, "decision", json_string_or_null(decision));
    store_call("finding_decision", args, 0, s, decide_done, TAG_DECIDE, &s->req_decide);
    pane_relayout(s->base.pane);
}

static void panel_destroy(Screen *base) {
    PanelScreen *s = (PanelScreen *)base;
    request_cancel(&s->req_pull); request_cancel(&s->req_findings); request_cancel(&s->req_decide); poller_stop(&s->poller);
    session_free(&s->session); project_free(&s->project); json_free(s->pr); json_free(s->findings); free(s->error); free(s->deciding);
    screen_release(base);
}

static void panel_layout(Screen *base, Doc *doc) {
    PanelScreen *s = (PanelScreen *)base;
    int w = doc->width;
    doc_space(doc, px(14));
    bool pr = s->number && store_supports("pull");
    if (pr) screen_panel_pr_layout(s, doc, w);
    screen_panel_usage_layout(s, doc, w, pr);
    doc_space(doc, px(14));
}

static void panel_header(Screen *base, HeaderInfo *info) { (void)base; (void)info; }

static void panel_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    PanelScreen *s = (PanelScreen *)base;
    switch (action) {
    case ACT_OPEN_PR: open_web_url(json_str(json_get(s->pr, "url"))); break;
    case ACT_VIEW_PR: app_push_detail(pull_detail_screen_new(&s->project, s->number, NULL, NULL)); break;
    case ACT_COMMITS: s->commits_open = !s->commits_open; pane_relayout(base->pane); break;
    case ACT_FINDINGS: s->findings_open = !s->findings_open; pane_relayout(base->pane); break;
    case ACT_COMMIT_URL: open_web_url(json_str(json_get(json_at(json_get(s->pr, "commitList"), (size_t)arg), "url"))); break;
    case ACT_FINDING_URL: open_web_url(json_str(json_get(json_at(s->findings, (size_t)arg), "url"))); break;
    case ACT_DECIDE: {
        size_t index = (size_t)(arg / 4); int d = (int)(arg % 4);
        const Json *f = json_at(s->findings, index);
        const char *key = json_str(json_get(f, "key")), *current = json_str(json_get(f, "decision"));
        // The same pick twice clears it, as the dashboard does.
        if (key && d >= 0 && d < FINDING_DECISION_COUNT) decide(s, key, str_eq(current, finding_decision_ids[d]) ? NULL : finding_decision_ids[d]);
        break;
    }
    case ACT_AUTO_COMPACT: {
        Json *extra = json_object();
        json_set_bool(extra, "autoCompact", !json_bool_is(json_get(s->session.raw, "autoCompact"), true));
        conversation_session_op(session_id(&s->session), "rename", extra);
        break;
    }
    case ACT_INSTRUCTIONS: {
        const char *current = json_str(json_get(s->session.raw, "compactInstructions"));
        char *text = dialog_text(app_dialog_owner(), "Compaction instructions", "What every compaction of this session must keep (empty clears them):", "Save", current ? current : "");
        if (!text) break;
        char *trimmed = str_trim(text);
        if (!str_eq(trimmed, current ? current : "")) {
            Json *extra = json_object(); json_set_str(extra, "compactInstructions", trimmed);
            conversation_session_op(session_id(&s->session), "rename", extra);
        }
        free(trimmed); free(text);
        break;
    }
    case ACT_COMPACT:
        if (app_confirm("Compact this conversation?", "Summarizes the conversation to free context. This uses the provider and may incur usage.", "Compact", false))
            conversation_session_op(session_id(&s->session), "compact", NULL);
        break;
    case ACT_CLEAR:
        if (app_confirm("Clear the transcript?", "Hides the transcript so far from this chat. Nothing is deleted and the agent's context is unchanged.", "Clear", false))
            conversation_session_op(session_id(&s->session), "clear", NULL);
        break;
    }
}

static void panel_timer(Screen *base, UINT id) { PanelScreen *s = (PanelScreen *)base; if (poller_fired(&s->poller, id)) { if (s->deciding) poller_finished(&s->poller, false, -1); else panel_load(s); } }

static void panel_visible(Screen *base, bool shown) {
    PanelScreen *s = (PanelScreen *)base;
    if (shown) poller_start(&s->poller, base->pane, TIMER_POLL, 30000);
    else { poller_stop(&s->poller); request_cancel(&s->req_pull); request_cancel(&s->req_findings); }
}

static void panel_refresh(Screen *base) { PanelScreen *s = (PanelScreen *)base; request_cancel(&s->req_pull); panel_load(s); }

static const ScreenVTable panel_vt = {
    .destroy = panel_destroy, .layout = panel_layout, .header = panel_header, .action = panel_action, .timer = panel_timer,
    .visible = panel_visible, .refresh = panel_refresh,
};

/// The pull request the panel shows, or none; a session that links one later gets its sections then.
static void panel_set_pull(PanelScreen *s, const char *repo, int number) {
    if (!repo || !store_supports("pull")) number = 0;
    if (number == s->number) return;
    request_cancel(&s->req_pull); request_cancel(&s->req_findings);
    project_free(&s->project); memset(&s->project, 0, sizeof s->project);
    s->project.repo = xstrdup(repo ? repo : "");
    s->number = number;
    json_free(s->pr); s->pr = json_null(); json_free(s->findings); s->findings = json_array();
    set_string(&s->error, NULL);
}

Screen *session_panel_screen_new(const Session *session) {
    PanelScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &panel_vt; s->base.id = xstrfmt("panel:%s", session_id(session));
    session_copy(&s->session, session);
    s->project.repo = xstrdup(session_repo(session) ? session_repo(session) : "");
    s->pr = json_null(); s->findings = json_array();
    s->findings_open = true;
    panel_set_pull(s, session_repo(session), session_pull_number(session));
    return &s->base;
}

void session_panel_update(const Session *session) {
    Screen *root = app_panel_pane() ? pane_root(app_panel_pane()) : NULL;
    char *id = xstrfmt("panel:%s", session_id(session));
    bool ours = root && root->id && str_eq(root->id, id);
    free(id);
    if (!ours) return;
    PanelScreen *s = (PanelScreen *)root;
    session_free(&s->session); session_copy(&s->session, session);
    int before = s->number;
    panel_set_pull(s, session_repo(session), session_pull_number(session));
    if (s->number && s->number != before) panel_load(s);
    pane_relayout(root->pane);
}
