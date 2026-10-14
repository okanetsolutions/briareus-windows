#include "project_files_internal.h"


bool project_files_offered(void) { return store_supports("repo_tree") && store_supports("repo_file"); }

void project_files_relayout(ProjectFiles *p) {
    if (!p->host->pane) return;
    pane_relayout(p->host->pane);
    pane_header_changed(p->host->pane);
}

FileTab *project_files_active_file(ProjectFiles *p) { return p->active >= 0 && (size_t)p->active < p->tab_count ? p->tabs[p->active] : NULL; }

bool project_files_command(ProjectFiles *p, int id, int code) {
    if (id != ID_FIND || !p->find) return false;
    if (code == EN_CHANGE) {
        int len = GetWindowTextLengthW(p->find);
        wchar_t *w = xcalloc((size_t)len + 1, sizeof *w);
        GetWindowTextW(p->find, w, len + 1);
        char *text = wide_to_utf8(w); free(w);
        set_string(&p->query, str_empty(text) ? NULL : text);
        free(text);
        p->pick = 0;
        project_files_find_update(p);
        project_files_relayout(p);
    } else if ((code == EN_SETFOCUS || code == EN_KILLFOCUS) && p->host->pane) pane_repaint(p->host->pane);
    return true;
}

bool project_files_key(ProjectFiles *p, WPARAM vk, bool ctrl, bool shift) {
    int mode = project_files_mode_for_key(vk, ctrl, shift);
    if (mode < 0 || !p->find) return false;
    project_files_set_mode(p, mode);
    if (p->host->pane) pane_scroll_to_top(p->host->pane);
    SetFocus(p->find);
    SendMessageW(p->find, EM_SETSEL, 0, -1);
    return true;
}

bool project_files_timer(ProjectFiles *p, UINT id) {
    if (id != p->timer || !p->host->pane) return false;
    KillTimer(pane_hwnd(p->host->pane), id);
    pane_scroll_to(p->host->pane, p->goto_y);
    return true;
}

void project_files_place(ProjectFiles *p, const RECT *content, int scroll_y, bool shown) {
    // The line a finder opened, once its file is laid out: scrolled to after this layout, not inside it.
    if (shown && p->goto_scroll && p->goto_y >= 0 && p->host->pane) { p->goto_scroll = false; SetTimer(pane_hwnd(p->host->pane), p->timer, 1, NULL); }
    if (!p->find) return;
    if (!shown || !content || !p->find_laid) { ShowWindow(p->find, SW_HIDE); return; }
    RECT rc; GetClientRect(pane_hwnd(p->host->pane), &rc);
    int m = (rc.right - rc.left - pane_content_width(p->host->pane)) / 2 - pane_scroll_x(p->host->pane);
    RECT r = { content->left + m + p->find_rc.left, content->top + p->find_rc.top - scroll_y, content->left + m + p->find_rc.right, content->top + p->find_rc.bottom - scroll_y };
    RECT visible;
    if (!IntersectRect(&visible, &r, content)) { ShowWindow(p->find, SW_HIDE); return; }
    MoveWindow(p->find, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
    bool clipped = !EqualRect(&visible, &r);
    if (clipped) SetWindowRgn(p->find, CreateRectRgn(visible.left - r.left, visible.top - r.top, visible.right - r.left, visible.bottom - r.top), TRUE);
    else if (p->clipped) SetWindowRgn(p->find, NULL, TRUE);
    p->clipped = clipped;
    ShowWindow(p->find, SW_SHOWNA);
}

void project_files_layout(ProjectFiles *p, Doc *doc, int w) {
    project_files_open(p);
    p->find_laid = false;
    if (p->error) {
        doc_notice(doc, 0, w, p->error);
        doc_space(doc, px(8));
        doc_button(doc, 0, 0, "Try again", BUTTON_BORDERED, p->base + A_RETRY, 0, !p->req);
        doc_space(doc, px(12));
    }
    if (!p->has_tree) {
        if (!p->error) doc_loading(doc, 0, w, "Loading the files\xE2\x80\xA6");
        return;
    }
    bool wide = w >= px(720);
    int tree_w = w * 28 / 100, gap = px(16);
    if (tree_w < px(240)) tree_w = px(240);
    if (tree_w > px(340)) tree_w = px(340);
    if (!wide) tree_w = w;
    int file_top = doc->y;
    // The finder, and the branch being read when it changes.
    project_files_layout_find(p, doc, 0, tree_w);
    if (p->req && p->loaded) {
        char *line = xstrfmt("Reading %s\xE2\x80\xA6", project_files_shown_ref(p));
        doc_space(doc, px(4));
        RECT r = { 0, doc->y, tree_w, doc->y + px(18) };
        doc->y = r.bottom;
        doc_text_at(doc, &r, line, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        free(line);
    }
    doc_space(doc, px(8));
    project_files_layout_modes(p, doc, 0, tree_w);
    doc_space(doc, px(10));
    int top = doc->y, first = (int)doc->count;
    if (!str_empty(p->query)) project_files_layout_results(p, doc, 0, tree_w); else project_files_layout_tree(p, doc, 0, tree_w);
    int last = (int)doc->count, bottom = doc->y;
    if (!wide) {
        doc_space(doc, px(14));
        if (p->tab_count) { project_files_layout_tabs(p, doc, 0, w); doc_space(doc, px(10)); }
        project_files_layout_file(p, doc, 0, w);
        doc_space(doc, px(16));
        return;
    }
    doc->y = file_top;
    int fx = tree_w + gap, fw = w - fx;
    if (p->tab_count) { project_files_layout_tabs(p, doc, fx, fw); doc_space(doc, px(10)); }
    project_files_layout_file(p, doc, fx, fw);
    doc_space(doc, px(16));
    // The tree stays in view beside the file and scrolls on its own, as the changed files' tree does: the page is as
    // long as the file, or the view's height when the tree needs that.
    if (p->host->pane) {
        RECT view = pane_content_rect(p->host->pane);
        int tree_h = bottom - top, room = view.bottom - view.top - px(24);
        if (tree_h > room) tree_h = room;
        if (doc->y < top + tree_h) doc->y = top + tree_h;
    }
    doc_sticky(doc, first, last, doc->y);
}

void project_files_header(ProjectFiles *p, HeaderInfo *info) {
    if (p->has_tree) {
        size_t files = repo_tree_file_count(&p->tree);
        char sha[8] = "";
        if (p->tree.sha) snprintf(sha, sizeof sha, "%.7s", p->tree.sha);
        snprintf(info->subtitle, sizeof info->subtitle, "%s \xC2\xB7 %s%s%zu file%s", p->repo, sha, sha[0] ? " \xC2\xB7 " : "", files, files == 1 ? "" : "s");
    }
    // The branch picker, as the other tabs' pickers are.
    if (info->button_count < HEADER_BUTTONS) {
        HeaderButton *b = &info->buttons[info->button_count++];
        snprintf(b->label, sizeof b->label, "\xE2\x8E\x87 %s \xE2\x96\xBE", project_files_shown_ref(p));
        b->action = p->base + A_BRANCH; b->enabled = true; b->tip = "Browse another branch";
    }
}

bool project_files_action(ProjectFiles *p, int action, intptr_t arg, POINT pt) {
    if (action < p->base || action >= p->base + PROJECT_FILES_ACTIONS) return false;
    FileTab *f = project_files_active_file(p);
    switch (action - p->base) {
    case A_ROW:
        if (!p->has_tree || (size_t)arg >= p->tree.count) break;
        if (p->tree.entries[arg].folder) { p->tree.entries[arg].open = !p->tree.entries[arg].open; project_files_relayout(p); }
        else project_files_open_at(p, p->tree.entries[arg].path, 0);
        break;
    case A_RESULT: project_files_open_result(p, (size_t)arg); break;
    case A_TAB: if ((size_t)arg < p->tab_count) { set_string(&p->goto_path, NULL); project_files_activate(p, (int)arg); project_files_state_save(p); project_files_relayout(p); } break;
    case A_MODE: project_files_set_mode(p, (int)arg); if (p->find) SetFocus(p->find); break;
    case A_INDEX_RETRY: set_string(&p->index_error, NULL); project_files_index_sync(p); project_files_relayout(p); break;
    case A_TAB_CLOSE: project_files_tab_close(p, (size_t)arg); project_files_state_save(p); project_files_relayout(p); break;
    case A_BRANCH: if (p->host->pane) project_files_pick_branch(p, pt); break;
    case A_OPEN_GITHUB: if (f && safe_web_url(f->file.url)) open_web_url(f->file.url); break;
    case A_COPY: if (f && f->file.content && p->host->pane) copy_to_clipboard(pane_hwnd(p->host->pane), f->file.content); break;
    case A_RETRY: set_string(&p->error, NULL); project_files_load(p); project_files_relayout(p); break;
    case A_RETRY_FILE: if (f) { project_files_file_ensure(p, f, true); project_files_relayout(p); } break;
    case A_FOCUS_FIND: if (p->find) SetFocus(p->find); break;
    case A_CLEAR_FIND: project_files_find_clear(p); project_files_relayout(p); break;
    }
    return true;
}

/// A tab's menu: close it, the others, or all of them.
bool project_files_context(ProjectFiles *p, int action, intptr_t arg, POINT pt) {
    if ((action != p->base + A_TAB && action != p->base + A_TAB_CLOSE) || (size_t)arg >= p->tab_count || !p->host->pane) return false;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Close");
    AppendMenuW(menu, MF_STRING | (p->tab_count > 1 ? 0 : MF_GRAYED), 2, L"Close others");
    AppendMenuW(menu, MF_STRING, 3, L"Close all");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, 4, L"Copy path");
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(p->host->pane), NULL);
    DestroyMenu(menu);
    size_t index = (size_t)arg;
    switch (chosen) {
    case 1: project_files_tab_close(p, index); break;
    case 2: {
        FileTab *keep = p->tabs[index];
        for (size_t i = 0; i < p->tab_count; i++) if (p->tabs[i] != keep) project_files_file_free(p->tabs[i]);
        p->tabs[0] = keep; p->tab_count = 1;
        project_files_activate(p, 0);
        break;
    }
    case 3:
        for (size_t i = 0; i < p->tab_count; i++) project_files_file_free(p->tabs[i]);
        p->tab_count = 0; p->active = -1;
        break;
    case 4: copy_to_clipboard(pane_hwnd(p->host->pane), p->tabs[index]->path); break;
    }
    if (chosen >= 1 && chosen <= 3) { project_files_state_save(p); project_files_relayout(p); }
    return true;
}

// MARK: - Lifetime

ProjectFiles *project_files_new(const char *repo, Screen *host, int action_base, UINT timer) {
    ProjectFiles *p = xcalloc(1, sizeof *p);
    p->repo = xstrdup(repo); p->host = host; p->base = action_base; p->active = -1; p->timer = timer; p->goto_y = -1;
    p->mode = FIND_FILES;
    repo_index_init(&p->index);
    // The branch and tabs open last time; each file is read once the tree says at which commit.
    char *key = project_files_state_key(p);
    Json *saved = cache_value(g_store.cache, key);
    free(key);
    set_string(&p->ref, json_str_nonempty(json_get(saved, "ref")));
    const Json *tabs = json_get(saved, "tabs");
    for (size_t i = 0; i < json_count(tabs) && p->tab_count < (size_t)MAX_TABS; i++) { const char *path = json_str_nonempty(json_at(tabs, i)); if (path) project_files_tab_add(p, path); }
    int active = json_int_or(json_get(saved, "active"), 0);
    p->active = p->tab_count ? (active >= 0 && (size_t)active < p->tab_count ? active : 0) : -1;
    int mode = json_int_or(json_get(saved, "mode"), FIND_FILES);
    if (mode >= 0 && mode < FIND_MODES) p->mode = mode;
    json_free(saved);
    return p;
}

void project_files_free(ProjectFiles *p) {
    if (!p) return;
    request_cancel(&p->req); request_cancel(&p->req_branches);
    // What was read so far is kept for next time.
    project_files_index_drop(p);
    if (p->host->pane) KillTimer(pane_hwnd(p->host->pane), p->timer);
    for (size_t i = 0; i < p->tab_count; i++) project_files_file_free(p->tabs[i]);
    free(p->tabs);
    if (p->find) DestroyWindow(p->find);
    if (p->has_tree) repo_tree_free(&p->tree);
    str_array_free(p->branches, p->branch_count);
    free(p->repo); free(p->ref); free(p->error); free(p->default_branch); free(p->query); free(p->results); free(p->hits);
    free(p->goto_path);
    free(p);
}
