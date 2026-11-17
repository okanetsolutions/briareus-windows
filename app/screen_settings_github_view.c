#include "screen_settings_github_internal.h"

static bool github_has_token(const GithubForm *s);
static void github_fill_edits(GithubForm *s);
static void paint_github_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void github_field(GithubForm *s, Doc *doc, int x, int w, int f);
static LRESULT CALLBACK github_field_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);

static const FieldDef GITHUB_FIELDS[GH_COUNT] = {
    [GH_LABEL] = { "label", K_TEXT, "Label", "Acme production", "Leave empty to name it after the verified login.", 0, false },
    [GH_TOKEN] = { "token", K_TEXT, "API token", "Paste a GitHub token",
        "Required for a new account. Leave blank when editing to keep the stored token; a replacement is verified by GitHub.", 0, true },
};

static bool github_has_token(const GithubForm *s) { return json_bool_is(json_get(s->row, "hasToken"), true); }

char *screen_settings_github_field_text(const Json *row, int f) {
    if (f == GH_TOKEN) return xstrdup("");
    const char *v = json_str(json_get(row, GITHUB_FIELDS[f].key));
    return xstrdup(v ? v : "");
}

/// The edits take the row's text, and the token's box says whether one is stored.
static void github_fill_edits(GithubForm *s) {
    s->filling = true;
    for (int f = 0; f < GH_COUNT; f++) {
        if (!s->edits[f]) continue;
        char *text = screen_settings_github_field_text(s->row, f);
        screen_settings_set_edit_text(s->edits[f], text);
        free(text);
    }
    if (s->edits[GH_TOKEN]) {
        wchar_t *w = utf8_to_wide(github_has_token(s) ? "Stored \xC2\xB7 type a new token to replace it" : GITHUB_FIELDS[GH_TOKEN].cue);
        SendMessageW(s->edits[GH_TOKEN], EM_SETCUEBANNER, TRUE, (LPARAM)w);
        free(w);
    }
    s->filling = false;
}

void screen_settings_github_fill(GithubForm *s) {
    json_free(s->repos);
    s->repos = json_is_array(json_get(s->row, "repos")) ? json_clone(json_get(s->row, "repos")) : json_array();
    github_fill_edits(s);
    s->dirty = false;
}

/// The body a save sends; NULL with `*why` when the form cannot be sent as it is.
Json *screen_settings_github_body(GithubForm *s, char **why) {
    Json *body = json_object();
    for (int f = 0; f < GH_COUNT; f++) {
        char *text = s->edits[f] ? screen_settings_edit_text(s->edits[f]) : screen_settings_github_field_text(s->row, f), *t = str_trim(text);
        free(text);
        // Preserve the server's automatic-label marker when only rotating a token or assigning projects.
        bool unchanged_label = f == GH_LABEL && s->id && str_eq(t, json_str_or(json_get(s->row, "label"), ""));
        bool blank_token = f == GH_TOKEN && !*t;
        if (!unchanged_label && !blank_token) json_set_str(body, GITHUB_FIELDS[f].key, t);
        free(t);
    }
    json_object_set(body, "repos", json_clone(s->repos));
    const char *missing = NULL;
    if (!s->id && str_empty(json_str(json_get(body, "token")))) missing = "Paste a GitHub token for this account.";
    if (missing) { *why = xstrdup(missing); json_free(body); return NULL; }
    return body;
}

// MARK: Layout

static void paint_github_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    GithubForm *s = it->data;
    fill_round_rect(cv, rc, px(6), theme.raise, s->focused == (int)it->arg ? theme.accent_dim : theme.line);
}

/// A labelled box with its edit, the hint behind a help icon beside the label; advances.
static void github_field(GithubForm *s, Doc *doc, int x, int w, int f) {
    const FieldDef *d = &GITHUB_FIELDS[f];
    doc_field_label(doc, x, w, d->label, theme.ink, d->hint);
    doc_space(doc, px(6));
    FontId fid = d->mono ? FONT_MONO : FONT_BODY;
    int fh = edit_line_height(fid), h = px(36);
    RECT box = { x, doc->y, x + w, doc->y + h };
    Item *it = doc_item(doc, doc_add(doc, &box, paint_github_box));
    it->data = s; it->arg = f; it->action = ACT_GITHUB_FOCUS;
    s->rects[f] = (RECT){ x + px(10), box.top + (h - fh) / 2, x + w - px(10), box.top + (h - fh) / 2 + fh };
    s->laid[f] = true;
    doc->y = box.bottom;
    doc_space(doc, px(14));
}

void screen_settings_github_layout(Screen *base, Doc *doc) {
    GithubForm *s = (GithubForm *)base;
    memset(s->laid, 0, sizeof s->laid);
    int col = doc->width, x = 0;
    doc_space(doc, px(8));
    int h = px(42), tx = x, ty = doc->y;
    s->tab_dot = s->dirty && screen_settings_github_form_changed(s);
    doc_tab(doc, &tx, &ty, x, x + col, h, 0xE753, s->tab_dot ? "GitHub account \xE2\x80\xA2" : "GitHub account", NULL, true, 0, 0);
    doc->y = ty + h;
    doc_rule(doc, x, col);
    doc_space(doc, px(18));
    if (s->error) { doc_notice_box(doc, x, col, s->error); doc_space(doc, px(16)); }
    github_field(s, doc, x, col, GH_LABEL);
    github_field(s, doc, x, col, GH_TOKEN);
    screen_settings_repos_layout(doc, x, col, s->repos, screen_settings_github_busy(s) ? 0 : ACT_GITHUB_REPO, "These projects use this GitHub account. Each project may be assigned to only one account.");
    if (s->id) {
        char *identity = xstrfmt("Verified login: %s\nGitHub id: %.0f", json_str_or(json_get(s->row, "login"), ""), json_num_or(json_get(s->row, "githubId"), 0));
        screen_settings_note(doc, x, col, identity); free(identity);
        doc_field_label(doc, x, col, "Scopes", theme.ink, NULL);
        char *scopes = github_account_scopes(s->row);
        screen_settings_note(doc, x, col, scopes); free(scopes);
    }
    doc_space(doc, px(40));
}

void screen_settings_github_header(Screen *base, HeaderInfo *info) {
    GithubForm *s = (GithubForm *)base;
    const char *label = json_str_nonempty(json_get(s->row, "label")), *login = json_str_nonempty(json_get(s->row, "login"));
    if (s->id) {
        snprintf(info->title, sizeof info->title, "%s", label ? label : login ? login : "GitHub account");
        size_t n = json_count(json_get(s->row, "repos"));
        snprintf(info->subtitle, sizeof info->subtitle, "@%s \xC2\xB7 %zu project%s%s", login ? login : "", n, n == 1 ? "" : "s",
                 github_has_token(s) ? "" : " \xC2\xB7 no token");
    } else {
        snprintf(info->title, sizeof info->title, "New GitHub account");
        snprintf(info->subtitle, sizeof info->subtitle, "A verified GitHub identity and its assigned projects.");
    }
    bool busy = s->req_save || s->req_delete;
    HeaderButton *b = &info->buttons[info->button_count++];
    snprintf(b->label, sizeof b->label, "%s", s->req_save ? "Saving\xE2\x80\xA6" : "Save");
    b->glyph = 0xE74E; b->action = ACT_GITHUB_SAVE; b->prominent = true; b->tip = "Save this GitHub account (Ctrl+S)";
    b->enabled = screen_settings_github_current(s) && !busy && (s->dirty || !s->id) && store_supports(s->id ? "update_github_account" : "create_github_account");
    if (!s->id) return;
    HeaderButton *d = &info->buttons[info->button_count++];
    d->glyph = 0xE74D; d->action = ACT_GITHUB_DELETE; d->destructive = true; d->enabled = screen_settings_github_current(s) && !busy && store_supports("delete_github_account"); d->tip = "Delete this GitHub account";
}

// MARK: The edits

void screen_settings_github_place(Screen *base, const RECT *content, int scroll_y) {
    GithubForm *s = (GithubForm *)base;
    int m = screen_settings_margin_of(base->pane);
    for (int f = 0; f < GH_COUNT; f++) {
        HWND e = s->edits[f];
        if (!e) continue;
        if (!s->shown || !s->laid[f]) { ShowWindow(e, SW_HIDE); continue; }
        RECT r = { content->left + m + s->rects[f].left, content->top + s->rects[f].top - scroll_y, content->left + m + s->rects[f].right, content->top + s->rects[f].bottom - scroll_y };
        RECT visible;
        if (!IntersectRect(&visible, &r, content)) { ShowWindow(e, SW_HIDE); continue; }
        MoveWindow(e, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
        bool clipped = !EqualRect(&visible, &r);
        if (clipped) SetWindowRgn(e, CreateRectRgn(visible.left - r.left, visible.top - r.top, visible.right - r.left, visible.bottom - r.top), TRUE);
        else if (s->clipped[f]) SetWindowRgn(e, NULL, TRUE);
        s->clipped[f] = clipped;
        ShowWindow(e, SW_SHOWNA);
    }
}

static LRESULT CALLBACK github_field_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    GithubForm *s = (GithubForm *)ref;
    int f = (int)id;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_TAB) { SetFocus(s->edits[(f + ((GetKeyState(VK_SHIFT) & 0x8000) ? GH_COUNT - 1 : 1)) % GH_COUNT]); return 0; }
        if (ctrl && wp == 'S') { screen_settings_github_save(s); return 0; }
        if (ctrl && wp == 'A') { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        if (wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); return 0; }
        if (wp == VK_RETURN) { SetFocus(s->edits[(f + 1) % GH_COUNT]); return 0; }
        break;
    case WM_CHAR:
        if (wp == '\t' || wp == 0x13 || wp == 0x01 || wp == 0x1B || wp == '\r') return 0;
        break;
    case WM_MOUSEWHEEL: SendMessageW(GetParent(hwnd), msg, wp, lp); return 0;
    case WM_NCDESTROY: RemoveWindowSubclass(hwnd, github_field_proc, id); break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void screen_settings_github_ensure_controls(GithubForm *s) {
    if (s->edits[0]) return;
    HWND owner = pane_hwnd(s->base.pane);
    for (int f = 0; f < GH_COUNT; f++) {
        DWORD style = WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | (f == GH_TOKEN ? ES_PASSWORD : 0);
        HWND e = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, owner, (HMENU)(INT_PTR)(ID_GITHUB_FIELD + f), GetModuleHandleW(NULL), NULL);
        SendMessageW(e, WM_SETFONT, (WPARAM)font(GITHUB_FIELDS[f].mono ? FONT_MONO : FONT_BODY), TRUE);
        SendMessageW(e, EM_SETLIMITTEXT, f == GH_TOKEN ? 4096 : 200, 0);
        wchar_t *w = utf8_to_wide(GITHUB_FIELDS[f].cue); SendMessageW(e, EM_SETCUEBANNER, TRUE, (LPARAM)w); free(w);
        SetWindowSubclass(e, github_field_proc, (UINT_PTR)f, (DWORD_PTR)s);
        theme_apply_control(e);
        s->edits[f] = e;
    }
    // The edits take the row's text; the projects ticked before they existed stay as they are.
    github_fill_edits(s);
    screen_settings_github_controls(s);
}

void screen_settings_github_controls(GithubForm *s) {
    bool enabled = screen_settings_github_current(s) && !screen_settings_github_busy(s);
    for (int f = 0; f < GH_COUNT; f++) if (s->edits[f]) EnableWindow(s->edits[f], enabled);
    pane_relayout(s->base.pane);
}

void screen_settings_github_toggle_repo(GithubForm *s, size_t index) {
    if (screen_settings_github_busy(s) || !screen_settings_github_current(s)) return;
    if (screen_settings_repos_toggle(&s->repos, index)) {
        screen_settings_github_changed(s);
        pane_relayout(s->base.pane);
    }
}
