#include "screen_pulls_internal.h"

static void pull_destroy(Screen *base);
static void layout_main(PullScreen *s, Doc *doc, Col c);
static void access_done(void *owner, Request *req);
static void pull_place(Screen *base, const RECT *content, int scroll_y);
static void pull_layout(Screen *base, Doc *doc);
static void pull_header(Screen *base, HeaderInfo *info);
static void pull_timer(Screen *base, UINT id);
static void pull_visible(Screen *base, bool shown);
static void pull_activated(Screen *base, bool active);

static void pull_destroy(Screen *base) {
    PullScreen *s = (PullScreen *)base;
    request_cancel(&s->req_pull); request_cancel(&s->req_findings); request_cancel(&s->req_rows); request_cancel(&s->req_actions);
    request_cancel(&s->req_sessions); request_cancel(&s->req_start); request_cancel(&s->req_merge); request_cancel(&s->req_decide); request_cancel(&s->req_body);
    request_cancel(&s->req_delete_run); free(s->deleting_run); free(s->run_error);
    request_cancel(&s->req_issue); json_free(s->issue_projects); json_free(s->issue_incoming);
    request_cancel(&s->req_edit); request_cancel(&s->req_update_branch); free(s->edit_error); free(s->branch_note);
    screen_pulls_reviews_clear(s);
    poller_stop(&s->poller);
    project_free(&s->project); if (s->has_stack) stack_position_free(&s->stack); if (s->has_summary) pull_summary_free(&s->summary);
    json_free(s->pr); if (s->has_row) pull_summary_free(&s->row); json_free(s->catalog); sessions_free(s->runs, s->run_count); json_free(s->findings);
    free(s->error); free(s->findings_error); free(s->write_error); free(s->start_device); free(s->merge_error); free(s->deciding); free(s->open_findings); free(s->body); free(s->body_author);
    board_actions_free(s->actions, s->action_count);
    pull_files_free(s->files);
    request_cancel(&s->req_run); request_cancel(&s->req_profiles); request_cancel(&s->req_log);
    run_log_clear(&s->log); free(s->log_session);
    run_address_free(&s->address);
    webview_free(s->web);
    request_cancel(&s->req_access); free(s->access_id); free(s->access_secret); free(s->access_suffix);
    free(s->run_url); free(s->run_session); free(s->run_profile); free(s->run_want); free(s->run_asked); free(s->serve_error);
    str_array_free(s->profiles, s->profile_count);
    for (int k = 0; k < CONV_FEEDS; k++) { request_cancel(&s->conv[k].req); json_free(s->conv[k].items); json_free(s->conv[k].incoming); free(s->conv[k].error); }
    screen_release(base);
}

/// The main column: what the selected tab holds.
static void layout_main(PullScreen *s, Doc *doc, Col c) {
    if (s->tab == PR_TAB_FINDINGS || s->tab == PR_TAB_CODE_REVIEWS) { screen_pulls_layout_reviews(s, doc, c); return; }
    if (json_is_null(s->pr)) {
        if (s->tab == PR_TAB_SESSIONS) screen_pulls_layout_runs(s, doc, c);
        else if (!s->error) doc_loading(doc, c.x, c.w, NULL);
        return;
    }
    switch (s->tab) {
    case PR_TAB_FILES: pull_files_layout(s->files, doc, c.x, c.w); break;
    case PR_TAB_CONVERSATION: screen_pulls_layout_conversation(s, doc, c); break;
    case PR_TAB_FINDINGS: case PR_TAB_CODE_REVIEWS: screen_pulls_layout_reviews(s, doc, c); break;
    case PR_TAB_SESSIONS: screen_pulls_layout_runs(s, doc, c); break;
    default:
        screen_pulls_layout_description(s, doc, c);
        break;
    }
}

static void access_done(void *owner, Request *req) {
    PullScreen *s = owner;
    s->access_read = true;
    if (req->ok) {
        set_string(&s->access_id, json_str(json_get(req->result, "clientId")));
        set_string(&s->access_secret, json_str(json_get(req->result, "clientSecret")));
        set_string(&s->access_suffix, json_str(json_get(req->result, "hostSuffix")));
    }
    screen_pulls_run_changed(s);
}

static void pull_place(Screen *base, const RECT *content, int scroll_y) {
    PullScreen *s = (PullScreen *)base;
    bool on = s->shown && s->tab == PR_TAB_RUN && s->run_url && !s->run_busy;
    // The browser is a child of the pane, so it starts once the tab is first shown in one, and once the service token
    // that lets it past Cloudflare Access has been read.
    if (on && !s->web && !s->access_read && store_supports("preview_access")) {
        if (!s->req_access) store_call("preview_access", json_object(), 0, s, access_done, TAG_ACCESS, &s->req_access);
        run_address_place(&s->address, base, &s->web, s->run_url, content, scroll_y, on);  // The address shows meanwhile.
        return;
    }
    if (on && !s->web) {
        WebViewAccess access = { s->access_id, s->access_secret, s->access_suffix };
        s->web = webview_new(pane_hwnd(base->pane), s->run_url, &access, screen_pulls_web_changed, s);
    }
    run_address_place(&s->address, base, &s->web, s->run_url, content, scroll_y, on);
    if (!s->web) return;
    if (on) {
        RECT rc; GetClientRect(pane_hwnd(base->pane), &rc);
        int m = (rc.right - rc.left - pane_content_width(base->pane)) / 2;
        RECT r = { content->left + m + s->web_rc.left, content->top + s->web_rc.top - scroll_y, content->left + m + s->web_rc.right, content->top + s->web_rc.bottom - scroll_y };
        RECT visible;
        if (!IntersectRect(&visible, &r, content)) on = false;
        else webview_set_bounds(s->web, &visible);
    }
    webview_show(s->web, on);
}

static void pull_layout(Screen *base, Doc *doc) {
    PullScreen *s = (PullScreen *)base;
    int w = doc->width;
    doc_space(doc, px(14));
    if (s->error) { doc_notice(doc, px(4), w - px(8), s->error); doc_space(doc, px(10)); }
    if (s->write_error) {
        doc_notice(doc, px(4), w - px(8), s->write_error);
        if (s->uncertain) { doc_space(doc, px(4)); doc_text(doc, px(4), w - px(8), "The request may have completed. Refresh (F5) and look for its conversation in the Sessions tab before starting another agent.", FONT_CAPTION, theme.secondary, DT_WORDBREAK); }
        doc_space(doc, px(10));
    }
    Col head = { 0, w, px(4), w - px(8) };
    screen_pulls_layout_header(s, doc, head);
    doc_space(doc, px(12));
    screen_pulls_layout_tabs(s, doc, head);
    // With the sidebar, the title and the tabs stay at the top while the tab's content scrolls beneath them, unless
    // they would take most of the view (a long stack overview).
    RECT view = pane_content_rect(base->pane);
    int view_h = view.bottom - view.top;
    bool files = s->tab == PR_TAB_FILES && !json_is_null(s->pr);
    if (s->tab != PR_TAB_RUN && !files && doc->y < view_h / 2) doc_pin(doc, (int)doc->count, doc->y);
    doc_space(doc, px(18));
    if (s->tab == PR_TAB_RUN) {
        screen_pulls_layout_run(s, doc, w);
        return;
    } else if (files || s->tab == PR_TAB_FINDINGS || s->tab == PR_TAB_CODE_REVIEWS) {
        // Tables and file diffs use the whole pane so their columns remain readable.
        layout_main(s, doc, screen_pulls_col_make(0, w));
    } else if (w >= px(880)) {
        // Wide: the conversation on the left, GitHub's sidebar on the right. The sidebar stays in view beside the
        // conversation, and scrolls on its own when it is taller than the view.
        int side_w = w * 26 / 100, gap = px(28);
        if (side_w < px(240)) side_w = px(240);
        if (side_w > px(320)) side_w = px(320);
        Col main = screen_pulls_col_make(0, w - side_w - gap), side = { w - side_w, side_w, w - side_w, side_w };
        doc->scroll_width = main.w;
        int top = doc->y;
        layout_main(s, doc, main);
        int main_bottom = doc->y;
        int side_top = doc->y = top - px(14), side_first = (int)doc->count;
        screen_pulls_layout_sidebar(s, doc, side);
        if (doc->pin_last) {
            // The page is as long as the conversation, or as the sidebar's window when that is longer.
            int side_h = doc->y - side_top, room = view_h - side_top - px(12);
            if (side_h > room) {
                // It scrolls on its own, so its bar takes the last 10px instead of covering the buttons' and badges'
                // edges: laid out again that much narrower, with an unpainted spacer keeping its window to the edge.
                // The spacer sits level with the first item, so the window's top stays where that item starts.
                doc_truncate(doc, (size_t)side_first);
                doc->y = side_top;
                Col narrow = { side.x, side.w - px(10), side.ix, side.iw - px(10) };
                screen_pulls_layout_sidebar(s, doc, narrow);
                int first_top = doc->count > (size_t)side_first ? doc->items[side_first].rc.top : side_top;
                RECT spacer = { w - 1, first_top, w, first_top + 1 };
                doc_add(doc, &spacer, NULL);
                side_h = room;
            }
            doc->y = side_top + side_h;
            if (doc->y < main_bottom) doc->y = main_bottom;
            doc_sticky(doc, side_first, (int)doc->count, doc->y);
        } else if (doc->y < main_bottom) doc->y = main_bottom;
    } else {
        layout_main(s, doc, screen_pulls_col_make(0, w));
        doc_space(doc, px(6));
        Col side = { px(4), w - px(8), px(4), w - px(8) };
        screen_pulls_layout_sidebar(s, doc, side);
    }
    doc_space(doc, px(16));
}

static void pull_header(Screen *base, HeaderInfo *info) {
    PullScreen *s = (PullScreen *)base;
    snprintf(info->title, sizeof info->title, "Pull request");
    snprintf(info->subtitle, sizeof info->subtitle, "%s #%d", s->project.repo, s->number);
}

static void pull_timer(Screen *base, UINT id) {
    PullScreen *s = (PullScreen *)base;
    if (pull_files_timer(s->files, id)) return;
    if (id == TIMER_RUN_LOG) { screen_pulls_run_log_tick(s); return; }
    if (poller_fired(&s->poller, id)) {
        bool enabled = !s->busy && !s->merging && !s->deciding && !s->editing && !s->updating_branch && !s->dialog_open;
        if (enabled) {
            screen_pulls_pull_load(s);
            screen_pulls_reviews_poll(s);
        } else poller_finished(&s->poller, false, -1);
    }
}

static void pull_visible(Screen *base, bool shown) {
    PullScreen *s = (PullScreen *)base;
    s->shown = shown;
    if (!shown) run_address_place(&s->address, base, &s->web, s->run_url, NULL, 0, false);
    if (s->web && !shown) webview_show(s->web, false);
    if (shown) screen_pulls_profiles_load(s);
    if (shown && s->run_busy) SetTimer(pane_hwnd(base->pane), TIMER_RUN_LOG, 1500, NULL);
    if (!shown) request_cancel(&s->reviews.req);
    if (!shown) KillTimer(pane_hwnd(base->pane), TIMER_RUN_LOG);
    if (shown) { poller_start(&s->poller, base->pane, TIMER_POLL, 30000); if (s->tab == PR_TAB_FILES) pull_files_load(s->files); if (s->tab == PR_TAB_CONVERSATION) screen_pulls_conv_load(s); }
    else { poller_stop(&s->poller); pull_files_cancel(s->files); screen_pulls_conv_cancel(s); request_cancel(&s->req_pull); request_cancel(&s->req_findings); request_cancel(&s->req_rows); request_cancel(&s->req_actions); request_cancel(&s->req_sessions); request_cancel(&s->req_body); request_cancel(&s->req_issue); }
}

void screen_pulls_pull_refresh(Screen *base) {
    PullScreen *s = (PullScreen *)base;
    // F5 on the Run tab reloads the page, as in a browser.
    if (s->tab == PR_TAB_RUN && s->web) { webview_reload(s->web); return; }
    // Refreshing is how an uncertain start is checked: its conversation is listed in the Sessions tab if it began.
    s->uncertain = false; set_string(&s->write_error, NULL); set_string(&s->edit_error, NULL); set_string(&s->branch_note, NULL);
    request_cancel(&s->req_body); s->body_read = false;
    request_cancel(&s->req_issue); s->issues_read = false; json_free(s->issue_incoming); s->issue_incoming = NULL;
    request_cancel(&s->reviews.req); screen_pulls_reviews_load(s, true);
    request_cancel(&s->req_pull); screen_pulls_pull_load(s);
    if (pull_files_started(s->files)) pull_files_refresh(s->files);
    pane_relayout(base->pane);
}

static void pull_activated(Screen *base, bool active) { if (active) { PullScreen *s = (PullScreen *)base; poller_start(&s->poller, base->pane, TIMER_POLL, 30000); } }

static const ScreenVTable pull_vt = {
    .destroy = pull_destroy, .layout = pull_layout, .header = pull_header, .action = screen_pulls_pull_action, .timer = pull_timer,
    .window_visible = screen_pulls_reviews_visible, .key = screen_pulls_reviews_key, .visible = pull_visible, .refresh = screen_pulls_pull_refresh, .activated = pull_activated, .place = pull_place,
};

Screen *pull_detail_screen_new(const Project *project, int number, const StackPosition *stack, const PullSummary *summary) {
    PullScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &pull_vt; s->base.id = xstrfmt("pull:%s#%d", project->repo, number);
    project_copy(&s->project, project); s->number = number;
    if (stack) { stack_position_copy(&s->stack, stack); s->has_stack = true; }
    if (summary) { pull_summary_copy(&s->summary, summary); s->has_summary = true; }
    s->pr = json_null(); s->catalog = json_array(); s->findings = json_array();
    s->files = pull_files_new(project, number, &s->base, ACT_FILES_BASE, TIMER_FILES_PAGE);
    return &s->base;
}

void screen_pulls_board_open_run(PullsScreen *s, size_t index) {
    const PullSummary *pull = &s->pulls[index];
    StackPosition stack; bool has_stack = stack_position_parse(json_get(pull->raw, "stack"), json_get(s->board, "stacks"), &stack);
    Screen *screen = pull_detail_screen_new(&s->project, pull->number, has_stack ? &stack : NULL, pull);
    if (has_stack) stack_position_free(&stack);
    screen_pulls_run_open((PullScreen *)screen);
    app_push_from(&s->base, screen);
}
