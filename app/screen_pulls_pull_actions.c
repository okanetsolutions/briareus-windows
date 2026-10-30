#include "screen_pulls_internal.h"


void screen_pulls_pull_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    PullScreen *s = (PullScreen *)base;
    switch (action) {
    case ACT_OPEN_URL: {
        const char *url = json_str(json_get(s->pr, "url"));
        if (arg == 1) { char *files = xstrfmt("%s/files", url); open_web_url(files); free(files); } else open_web_url(url);
        break;
    }
    case ACT_STACK_ITEM: app_push_from(&s->base, pull_detail_screen_new(&s->project, (int)arg, s->has_stack ? &s->stack : NULL, NULL)); break;
    case ACT_STACK_TOGGLE: s->stack_open = !s->stack_open; pane_relayout(base->pane); break;
    case ACT_PR_TAB:
        if (arg == PR_TAB_RUN) { screen_pulls_run_open(s); break; }
        // A review's findings keep their place across tabs; the open Code reviews tab leads back to its table.
        if (arg == PR_TAB_CODE_REVIEWS && s->tab == PR_TAB_CODE_REVIEWS) { screen_pulls_review_close(s); break; }
        switch (arg) {
        case PR_TAB_BODY: case PR_TAB_CONVERSATION: case PR_TAB_SESSIONS: case PR_TAB_FILES: case PR_TAB_CODE_REVIEWS:
            s->tab = (int)arg; break;
        default: s->tab = PR_TAB_BODY; break;
        }
        s->reviews.reset_inspector = true; s->reviews.reveal_row = true;
        screen_pulls_reviews_load(s, false);
        screen_pulls_reviews_poll(s);
        if (s->tab == PR_TAB_FILES) pull_files_load(s->files);
        if (s->tab == PR_TAB_CONVERSATION) screen_pulls_conv_load(s);
        pane_relayout(base->pane); pane_header_changed(base->pane);
        break;
    case ACT_REVIEW_SUMMARY:
        s->reviews.summary_id = s->reviews.summary_id == s->reviews.review_id ? 0 : s->reviews.review_id; pane_relayout(base->pane); break;
    case ACT_REVIEW_ROW: screen_pulls_review_open(s, (int64_t)arg); break;
    case ACT_REVIEW_BACK: screen_pulls_review_close(s); break;
    case ACT_REVIEW_FINDING:
        if (code_review_finding(code_review_find(s->reviews.items, s->reviews.review_id), (int64_t)arg)) { s->reviews.finding_id = (int64_t)arg; s->reviews.reset_inspector = true; }
        pane_relayout(base->pane); break;
    case ACT_WEB_RELOAD: if (s->web) webview_reload(s->web); break;
    case ACT_WEB_BACK: if (s->web) webview_back(s->web); break;
    case ACT_WEB_FORWARD: if (s->web) webview_forward(s->web); break;
    case ACT_RUN_PROFILE: screen_pulls_run_pick_profile(s, pt); break;
    case ACT_RUN_RETRY: screen_pulls_run_start(s); break;
    case ACT_WEB_BROWSER: open_web_url(s->web && webview_url(s->web) ? webview_url(s->web) : s->run_url); break;
    case ACT_CONV_URL: {
        const Json *feed = s->conv[arg >> 24].items;
        open_web_url(json_str(json_get(json_at(feed, (size_t)(arg & 0xFFFFFF)), "url")));
        break;
    }
    case ACT_ISSUE_URL: {
        // An issue of this repository opens here, which reads the rest itself; any other on GitHub.
        const PullSummary *row = screen_pulls_board_row(s);
        BoardLink parsed; memset(&parsed, 0, sizeof parsed);
        const BoardLink *link = row && (size_t)arg < row->issue_count ? &row->issues[arg]
            : board_link_parse(json_at(json_get(s->pr, "issues"), (size_t)arg), &parsed) ? &parsed : NULL;
        if (link && !board_link_is_foreign(link, s->project.repo) && store_supports("issue")) {
            IssueSummary bare; memset(&bare, 0, sizeof bare);
            bare.number = link->number; bare.title = link->title; bare.url = link->url;
            app_push_from(&s->base, issue_detail_screen_new(&s->project, &bare));
        } else if (link && safe_web_url(link->url)) open_web_url(link->url);
        board_link_free(&parsed);
        break;
    }
    case ACT_PR_PROJECT: open_web_url(json_str(json_get(json_at(json_get(json_at(s->issue_projects, (size_t)(arg >> 16)), "projects"), (size_t)(arg & 0xFFFF)), "url"))); break;
    case ACT_MERGE: screen_pulls_merge(s, "squash"); break;
    case ACT_UPDATE_BRANCH: screen_pulls_update_branch(s); break;
    case ACT_EDIT: {
        const char *title = json_str(json_get(s->pr, "title"));
        if (!title && screen_pulls_board_row(s)) title = screen_pulls_board_row(s)->title;
        if (s->editing || !screen_pulls_pull_body(s)) break;
        char *t = xstrdup(title ? title : ""), *b = xstrdup(screen_pulls_pull_body(s));
        s->dialog_open = true;
        Json *fields = screen_pulls_details_edit("pull request", s->number, t, b);
        s->dialog_open = false;
        free(t); free(b);
        screen_pulls_pull_edit(s, fields);
        break;
    }
    case ACT_EDIT_ASSIGNEES: case ACT_EDIT_LABELS: {
        const PullSummary *row = screen_pulls_board_row(s);
        if (!row || s->editing) break;
        s->dialog_open = true;
        Json *list = action == ACT_EDIT_LABELS ? screen_pulls_labels_edit(row->labels, row->label_count, s->number)
                                               : screen_pulls_assignees_edit(pane_hwnd(base->pane), pt, row->assignees, row->assignee_count, s->number);
        s->dialog_open = false;
        if (list) { Json *fields = json_object(); json_object_set(fields, action == ACT_EDIT_LABELS ? "labels" : "assignees", list); screen_pulls_pull_edit(s, fields); }
        pane_relayout(base->pane);
        break;
    }
    case ACT_RELOAD: screen_pulls_pull_refresh(base); break;
    case ACT_START_ACTION: {
        if ((size_t)arg >= s->action_count || s->busy || s->uncertain || s->dialog_open) break;
        // The list may be rebuilt while the prompt is up, so the errand is copied first.
        BoardAction a; board_action_copy(&a, &s->actions[arg]);
        char *input = NULL; RuntimeChoice runtime = {0};
        Pane *source_pane = base->pane; HWND source_window = pane_hwnd(source_pane);
        s->dialog_open = true;
        bool ok = screen_pulls_action_prompt(base, s->project.repo, &a, s->number, &input, &runtime);
        bool live = IsWindow(source_window) && pane_contains_screen(source_pane, base);
        bool current = live && pane_top(source_pane) == base;
        if (live) s->dialog_open = false;
        if (ok && current) screen_pulls_start_action(s, &a, input, &runtime);
        runtime_choice_free(&runtime); free(input); board_action_free(&a);
        break;
    }
    case ACT_DELETE_SERVED: {
        if (!screen_pulls_run_target(s) || s->deleting_run || !store_supports("delete")) break;
        char *id = xstrdup(screen_pulls_run_target(s));
        s->dialog_open = true;
        bool ok = app_confirm("Delete this run?", "Its workspace stops serving the pull request, and its conversation and transcript are deleted permanently.", "Delete", true);
        s->dialog_open = false;
        // The run may have changed while the dialog was open: delete the one asked about only if it is still shown.
        if (ok && !s->deleting_run && str_eq(id, screen_pulls_run_target(s))) {
            set_string(&s->deleting_run, id); set_string(&s->run_error, NULL);
            Json *args = json_object(); json_set_str(args, "sessionId", id);
            store_call("delete", args, 0, s, screen_pulls_delete_run_done, TAG_DELETE_RUN, &s->req_delete_run);
            pane_relayout(s->base.pane);
        }
        free(id);
        break;
    }
    case ACT_OPEN_RUN: if ((size_t)arg < s->run_count) app_push_from(&s->base, conversation_screen_new(&s->runs[arg])); break;
    case ACT_DELETE_RUN: {
        if ((size_t)arg >= s->run_count || s->deleting_run || !store_supports("delete")) break;
        char *id = xstrdup(session_id(&s->runs[arg]));
        char *message = xstrfmt("\xE2\x80\x9C%s\xE2\x80\x9D", session_display_title(&s->runs[arg]));
        s->dialog_open = true;
        bool ok = app_confirm("Permanently delete this conversation and its transcript?", message, "Delete", true);
        s->dialog_open = false;
        free(message);
        // The list may have been read again while the dialog was open: delete by id, not by row.
        if (ok && !s->deleting_run) {
            set_string(&s->deleting_run, id); set_string(&s->run_error, NULL);
            Json *args = json_object(); json_set_str(args, "sessionId", id);
            store_call("delete", args, 0, s, screen_pulls_delete_run_done, TAG_DELETE_RUN, &s->req_delete_run);
            pane_relayout(s->base.pane);
        }
        free(id);
        break;
    }
    default: pull_files_action(s->files, action, arg); break;
    }
}
