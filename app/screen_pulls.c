#include "screen_pulls_internal.h"

static void pulls_destroy(Screen *base);
static void pulls_action(Screen *base, int action, intptr_t arg, POINT pt);
static void pulls_timer(Screen *base, UINT id);
static void pulls_place(Screen *base, const RECT *content, int scroll_y);
static void pulls_command(Screen *base, int id, int code, HWND control);
static bool pulls_key(Screen *base, WPARAM vk, bool ctrl, bool shift);
static bool pulls_can_leave(Screen *base);
static void pulls_context(Screen *base, int action, intptr_t arg, POINT pt);
static void pulls_visible(Screen *base, bool shown);
static void pulls_refresh(Screen *base);
static void pulls_activated(Screen *base, bool active);
static void pulls_drag(Screen *base, int action, intptr_t arg, POINT pt, DragPhase phase);

/// Where the issue screen keeps its read of issue `number`, which the board and the pull request screen share.
char *screen_pulls_saved_issue_key(const char *repo, int number) { return xstrfmt("issue:%s#%d", repo, number); }

/// An issue's Status on the first of its project boards that gives it one; NULL on none.
const char *screen_pulls_issue_project_status(const Json *issue) {
    const Json *projects = json_get(issue, "projects");
    for (size_t i = 0; i < json_count(projects); i++) { const char *st = json_str(json_get(json_at(projects, i), "status")); if (!str_empty(st)) return st; }
    return NULL;
}

static void pulls_destroy(Screen *base) {
    PullsScreen *s = (PullsScreen *)base;
    request_cancel(&s->req); request_cancel(&s->req_actions); request_cancel(&s->req_start); request_cancel(&s->req_merge); poller_stop(&s->poller); screen_pulls_list_retry_stop(&s->poller, &s->list_retry_armed);
    json_free(s->board); json_free(s->catalog); pull_summaries_free(s->pulls, s->pull_count); issue_summaries_free(s->issues, s->issue_count);
    request_cancel(&s->req_runs); sessions_free(s->runs, s->run_count);
    request_cancel(&s->req_status); json_free(s->issue_status); json_free(s->status_read);
    project_ssh_free(s->ssh);
    project_sftp_free(s->sftp);
    project_run_free(s->run);
    project_db_free(s->db);
    project_forge_free(s->forge);
    board_tab_free(s->board_tab);
    project_files_free(s->files);
    board_filter_free(&s->pull_filter); board_filter_free(&s->issue_filter); board_filter_free(&s->opening);
    project_free(&s->project); free(s->error); free(s->write_error); free(s->starting_id);
    screen_release(base);
}

static void pulls_action(Screen *base, int action, intptr_t arg, POINT pt) {
    PullsScreen *s = (PullsScreen *)base;
    if (action == ACT_MEET) { meeting_menu(&s->project, pane_hwnd(base->pane), pt); pane_header_changed(base->pane); return; }
    if (project_ssh_action(s->ssh, action, arg, pt)) return;
    if (project_sftp_action(s->sftp, action, arg, pt)) return;
    if (project_run_action(s->run, action, arg, pt)) return;
    if (project_db_action(s->db, action, arg, pt)) return;
    if (project_forge_action(s->forge, action, arg, pt)) return;
    if (board_tab_action(s->board_tab, action, arg, pt)) return;
    if (project_files_action(s->files, action, arg, pt)) return;
    switch (action) {
    case ACT_FILTER_STACK:
        s->hide_later_stacked = !s->hide_later_stacked; s->has_opening = false;
        screen_pulls_filters_save(s); pane_relayout(base->pane); pane_header_changed(base->pane); break;
    case ACT_FILTER_AUTHOR: screen_pulls_filter_pick(s, FILTER_AUTHOR, pt); break;
    case ACT_FILTER_REVIEWER: screen_pulls_filter_pick(s, FILTER_REVIEWER, pt); break;
    case ACT_FILTER_ASSIGNEE: screen_pulls_filter_pick(s, FILTER_ASSIGNEE, pt); break;
    case ACT_FILTER_LABEL: screen_pulls_filter_pick(s, FILTER_LABEL, pt); break;
    case ACT_REFRESH: pulls_refresh(base); break;
    case ACT_REVIEW_LOGIN: if (screen_pulls_github_login_ask()) { pane_relayout(base->pane); pane_header_changed(base->pane); } break;
    case ACT_MERGE_PULL: screen_pulls_board_merge(s, (size_t)arg); break;
    case ACT_RUNS: if ((size_t)arg < s->pull_count) app_push_from(&s->base, pull_detail_screen_new(&s->project, s->pulls[arg].number, NULL, &s->pulls[arg])); break;
    case ACT_TAB:
        // The side panel belongs to the Board tab's cards.
        if (s->tab == TAB_BOARD && arg != TAB_BOARD) app_set_overlay(NULL);
        s->tab = arg == TAB_ISSUES || ((arg == TAB_SSH || arg == TAB_SFTP) && project_ssh_offered()) || (arg == TAB_RUN && project_run_offered()) || (arg == TAB_DB && project_db_offered()) || (arg == TAB_FORGE && project_forge_offered()) || (arg == TAB_BOARD && board_tab_offered(&s->project)) || (arg == TAB_FILES && project_files_offered()) || arg == TAB_MEETING || arg == TAB_REVIEW ? (int)arg : TAB_PULLS;
        if (s->tab == TAB_RUN) project_run_open(s->run);
        if (s->tab == TAB_BOARD) board_tab_open(s->board_tab);
        if (s->tab == TAB_FILES) project_files_open(s->files);
        // The transcript, like a conversation's, keeps to its latest line.
        pane_stick_to_bottom(base->pane, s->tab == TAB_MEETING);
        if (s->tab == TAB_MEETING) pane_scroll_to_bottom(base->pane);
        pane_relayout(base->pane); pane_header_changed(base->pane);
        break;
    case ACT_CLEAR: { if (s->tab == TAB_PULLS) s->hide_later_stacked = false; BoardFilter *f = screen_pulls_current_filter(s); board_filter_free(f); board_filter_init(f); s->has_opening = false; screen_pulls_filters_save(s); pane_relayout(base->pane); pane_header_changed(base->pane); break; }
    case ACT_OPEN_PULL: {
        if ((size_t)arg >= s->pull_count) break;
        StackPosition stack; bool has_stack = stack_position_parse(json_get(s->pulls[arg].raw, "stack"), json_get(s->board, "stacks"), &stack);
        app_push_from(&s->base, pull_detail_screen_new(&s->project, s->pulls[arg].number, has_stack ? &stack : NULL, &s->pulls[arg]));
        if (has_stack) stack_position_free(&stack);
        break;
    }
    case ACT_OPEN_ISSUE: if ((size_t)arg < s->issue_count) app_push_from(&s->base, issue_detail_screen_new(&s->project, &s->issues[arg])); break;
    case ACT_PULL_ACTION: {
        size_t index = (size_t)arg / ACTION_STRIDE, k = (size_t)arg % ACTION_STRIDE;
        if (index >= s->pull_count || s->busy || s->uncertain) break;
        // The rows may be replaced while the prompt is up, so what the start needs is copied first.
        int number = s->pulls[index].number; char *branch = xstrdup(s->pulls[index].branch ? s->pulls[index].branch : "");
        size_t an; BoardAction *actions = screen_pulls_row_actions(s->catalog, &s->pulls[index], 0, &an);
        if (k < an && str_eq(actions[k].id, "run")) screen_pulls_board_open_run(s, index);
        else if (k < an) {
            char *input = NULL;
            s->dialog_open = true;
            bool ok = screen_pulls_action_prompt(&actions[k], number, &input);
            s->dialog_open = false;
            if (ok) screen_pulls_board_start(s, number, branch, &actions[k], input);
            free(input);
        }
        board_actions_free(actions, an); free(branch);
        break;
    }
    }
}

static void pulls_timer(Screen *base, UINT id) {
    PullsScreen *s = (PullsScreen *)base;
    if (screen_pulls_list_retry_fired(&s->poller, &s->list_retry_armed, s->project.repo, id, s->dialog_open)) {
        if (screen_pulls_pulls_read(s->project.repo, s->fresh_pending, s, screen_pulls_pulls_done, &s->req)) { s->fresh_pending = false; s->list_retry_read = true; }
        pane_header_changed(base->pane);
        return;
    }
    if (project_run_timer(s->run, id)) return;
    if (project_files_timer(s->files, id)) return;
    if (poller_fired(&s->poller, id)) { if (s->dialog_open) poller_finished(&s->poller, false, -1); else screen_pulls_pulls_load(s, false); }
}

static void pulls_place(Screen *base, const RECT *content, int scroll_y) {
    PullsScreen *s = (PullsScreen *)base;
    project_ssh_place(s->ssh, content, scroll_y, s->shown && s->tab == TAB_SSH);
    project_sftp_place(s->sftp, content, scroll_y, s->shown && s->tab == TAB_SFTP);
    project_run_place(s->run, content, scroll_y, s->shown && s->tab == TAB_RUN);
    project_forge_place(s->forge, content, scroll_y, s->shown && s->tab == TAB_FORGE);
    project_files_place(s->files, content, scroll_y, s->shown && s->tab == TAB_FILES);
}

static void pulls_command(Screen *base, int id, int code, HWND control) {
    (void)control;
    PullsScreen *s = (PullsScreen *)base;
    if (!project_files_command(s->files, id, code)) project_forge_command(s->forge, id, code);
}

static bool pulls_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    PullsScreen *s = (PullsScreen *)base;
    if (s->tab == TAB_FILES) return project_files_key(s->files, vk, ctrl, shift);
    return s->tab == TAB_FORGE && project_forge_key(s->forge, vk, ctrl);
}

/// The board gives way to another screen unless a Forge site's unsaved changes are kept.
static bool pulls_can_leave(Screen *base) { return project_forge_can_leave(((PullsScreen *)base)->forge); }

static void pulls_context(Screen *base, int action, intptr_t arg, POINT pt) {
    PullsScreen *s = (PullsScreen *)base;
    if (!project_files_context(s->files, action, arg, pt)) project_sftp_context(s->sftp, action, arg, pt);
}

static void pulls_visible(Screen *base, bool shown) {
    PullsScreen *s = (PullsScreen *)base;
    s->shown = shown;
    if (!shown) { project_ssh_place(s->ssh, NULL, 0, false); project_sftp_place(s->sftp, NULL, 0, false); project_run_place(s->run, NULL, 0, false); project_forge_place(s->forge, NULL, 0, false); project_files_place(s->files, NULL, 0, false); }
    if (shown) { poller_start(&s->poller, base->pane, TIMER_POLL, 45000); screen_pulls_list_retry_arm(&s->poller, &s->list_retry_armed, s->project.repo); }
    else { poller_stop(&s->poller); screen_pulls_list_retry_stop(&s->poller, &s->list_retry_armed); s->list_retry_read = false; request_cancel(&s->req); request_cancel(&s->req_actions); request_cancel(&s->req_runs); request_cancel(&s->req_status); }
}

static void pulls_refresh(Screen *base) {
    PullsScreen *s = (PullsScreen *)base;
    if (s->tab == TAB_SSH) { project_ssh_refresh(s->ssh); pane_relayout(base->pane); return; }
    if (s->tab == TAB_SFTP) { project_sftp_refresh(s->sftp); pane_relayout(base->pane); return; }
    if (s->tab == TAB_DB) { project_db_refresh(s->db); pane_relayout(base->pane); return; }
    if (s->tab == TAB_FORGE) { project_forge_refresh(s->forge); pane_relayout(base->pane); return; }
    if (s->tab == TAB_RUN) { project_run_refresh(s->run); return; }
    if (s->tab == TAB_BOARD) { board_tab_refresh(s->board_tab); return; }
    if (s->tab == TAB_FILES) { project_files_refresh(s->files); return; }
    // Refreshing is how an uncertain start is checked: its conversation is listed in the project if it began.
    s->uncertain = false; set_string(&s->write_error, NULL);
    request_cancel(&s->req_status); json_free(s->status_read); s->status_read = json_object();
    screen_pulls_pulls_load(s, true);
}

static void pulls_activated(Screen *base, bool active) { if (active) pulls_visible(base, true); }

/// The Board tab's cards are carried between its columns.
static void pulls_drag(Screen *base, int action, intptr_t arg, POINT pt, DragPhase phase) {
    PullsScreen *s = (PullsScreen *)base;
    if (s->tab == TAB_BOARD) board_tab_drag(s->board_tab, action, arg, pt, phase);
}

static const ScreenVTable pulls_vt = {
    .destroy = pulls_destroy, .layout = screen_pulls_pulls_layout, .header = screen_pulls_pulls_header, .action = pulls_action, .timer = pulls_timer,
    .visible = pulls_visible, .refresh = pulls_refresh, .activated = pulls_activated, .place = pulls_place, .context = pulls_context,
    .command = pulls_command, .key = pulls_key, .can_leave = pulls_can_leave, .drag = pulls_drag,
};

Screen *pulls_screen_new(const Project *project) {
    PullsScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &pulls_vt; s->base.id = xstrfmt("pulls:%s", project->repo);
    project_copy(&s->project, project);
    s->board = json_null(); s->catalog = json_array(); s->issue_status = json_object(); s->status_read = json_object();
    board_filter_init(&s->pull_filter); board_filter_init(&s->issue_filter); board_filter_init(&s->opening);
    s->has_opening = !screen_pulls_filters_restore(s);
    s->ssh = project_ssh_new(project->repo, &s->base, ACT_SSH_BASE);
    s->sftp = project_sftp_new(project->repo, &s->base, ACT_SFTP_BASE);
    s->run = project_run_new(project->repo, &s->base, ACT_RUN_BASE, TIMER_BOARD_RUN_LOG);
    s->db = project_db_new(project->repo, &s->base, ACT_DB_BASE);
    s->forge = project_forge_new(project->repo, &s->base, ACT_FORGE_BASE);
    s->board_tab = board_tab_new(project, &s->base, ACT_BOARD_BASE);
    s->files = project_files_new(project->repo, &s->base, ACT_REPO_FILES_BASE, TIMER_FILES_GOTO);
    return &s->base;
}

/// One of the Conversation tab's lists, read page by page: `incoming` fills up and replaces `items` once the last page is in.
const screen_pulls_conv_feeds_type screen_pulls_conv_feeds[CONV_FEEDS] = {
    { "pull_comments", "comments" }, { "pull_reviews", "reviews" }, { "pull_review_comments", "reviewComments" },
};

const char *const screen_pulls_decision_ids[] = { "", "fix", "optional", "dismissed" };
