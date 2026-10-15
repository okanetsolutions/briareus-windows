#include "screen_settings_internal.h"

typedef struct { char *text; bool on, gone; } ForgeCheck;

static bool forge_has_token(const ForgeForm *s);
static bool repos_has(const Json *repos, const char *repo);
static Json *forge_choices(ForgeForm *s);
static void forge_fill_edits(ForgeForm *s);
static void paint_forge_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void forge_field(ForgeForm *s, Doc *doc, int x, int w, int f);
static void forge_check_free(void *p);
static void paint_forge_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void forge_projects(ForgeForm *s, Doc *doc, int x, int w);
static LRESULT CALLBACK forge_field_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);

static const FieldDef FORGE_FIELDS[G_COUNT] = {
    [G_LABEL] = { "label", K_TEXT, "Label", "Acme production", "Leave empty to name it after the organization.", 0, false },
    [G_ORG] = { "organization", K_TEXT, "Organization", "acme", "The slug in your Forge URLs: forge.laravel.com/<organization>/\xE2\x80\xA6", 0, true },
    [G_TOKEN] = { "token", K_TEXT, "API token", "Paste a Forge API token",
        "Create one in Forge under your profile's API tokens. The server stores it encrypted and never sends it back.", 0, true },
};

static bool forge_has_token(const ForgeForm *s) { return json_bool_is(json_get(s->row, "hasToken"), true); }

char *screen_settings_forge_field_text(const Json *row, int f) {
    if (f == G_TOKEN) return xstrdup("");
    const char *v = json_str(json_get(row, FORGE_FIELDS[f].key));
    return xstrdup(v ? v : "");
}

static bool repos_has(const Json *repos, const char *repo) {
    for (size_t i = 0; i < json_count(repos); i++) if (str_eq(json_str(json_at(repos, i)), repo)) return true;
    return false;
}

/// Whether two project lists hold the same projects, in any order.
bool screen_settings_repos_same(const Json *a, const Json *b) {
    if (json_count(a) != json_count(b)) return false;
    for (size_t i = 0; i < json_count(a); i++) if (!repos_has(b, json_str(json_at(a, i)))) return false;
    return true;
}

/// What the Projects list offers: every project, then any ticked one that is no longer a project (the server refuses it
/// on save, so it stays visible to be unticked).
static Json *forge_choices(ForgeForm *s) {
    Json *out = json_array();
    const Json *rows = screen_settings_g_settings ? screen_settings_settings_rows(screen_settings_g_settings) : NULL;
    for (size_t i = 0; i < json_count(rows); i++) {
        const char *repo = json_str_nonempty(json_get(json_at(rows, i), "repo"));
        if (repo && !repos_has(out, repo)) json_array_push(out, json_string(repo));
    }
    for (size_t i = 0; i < json_count(s->repos); i++) {
        const char *repo = json_str(json_at(s->repos, i));
        if (repo && !repos_has(out, repo)) json_array_push(out, json_string(repo));
    }
    return out;
}

/// The edits take the row's text, and the token's box says whether one is stored.
static void forge_fill_edits(ForgeForm *s) {
    s->filling = true;
    for (int f = 0; f < G_COUNT; f++) {
        if (!s->edits[f]) continue;
        char *text = screen_settings_forge_field_text(s->row, f);
        screen_settings_set_edit_text(s->edits[f], text);
        free(text);
    }
    if (s->edits[G_TOKEN]) {
        wchar_t *w = utf8_to_wide(forge_has_token(s) ? "Stored \xC2\xB7 type a new token to replace it" : FORGE_FIELDS[G_TOKEN].cue);
        SendMessageW(s->edits[G_TOKEN], EM_SETCUEBANNER, TRUE, (LPARAM)w);
        free(w);
    }
    s->filling = false;
}

void screen_settings_forge_fill(ForgeForm *s) {
    json_free(s->repos);
    s->repos = json_is_array(json_get(s->row, "repos")) ? json_clone(json_get(s->row, "repos")) : json_array();
    forge_fill_edits(s);
    s->dirty = false;
}

/// The body a save sends; NULL with `*why` when the form cannot be sent as it is.
Json *screen_settings_forge_body(ForgeForm *s, char **why) {
    Json *body = json_object();
    for (int f = 0; f < G_COUNT; f++) {
        char *text = s->edits[f] ? screen_settings_edit_text(s->edits[f]) : screen_settings_forge_field_text(s->row, f), *t = str_trim(text);
        free(text);
        // An empty token is left out, so the stored one stays.
        if (f != G_TOKEN || *t) json_set_str(body, FORGE_FIELDS[f].key, t);
        free(t);
    }
    json_object_set(body, "repos", json_clone(s->repos));
    const char *missing = NULL;
    if (str_empty(json_str(json_get(body, "organization")))) missing = "Enter the organization slug from your Forge URLs.";
    else if (!forge_has_token(s) && str_empty(json_str(json_get(body, "token")))) missing = "Paste a Forge API token for this organization.";
    if (missing) { *why = xstrdup(missing); json_free(body); return NULL; }
    return body;
}

// MARK: Layout

static void paint_forge_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    ForgeForm *s = it->data;
    fill_round_rect(cv, rc, px(6), theme.raise, s->focused == (int)it->arg ? theme.accent_dim : theme.line);
}

/// A labelled box with its edit, the hint behind a help icon beside the label; advances.
static void forge_field(ForgeForm *s, Doc *doc, int x, int w, int f) {
    const FieldDef *d = &FORGE_FIELDS[f];
    doc_field_label(doc, x, w, d->label, theme.ink, d->hint);
    doc_space(doc, px(6));
    FontId fid = d->mono ? FONT_MONO : FONT_BODY;
    int fh = edit_line_height(fid), h = px(36);
    RECT box = { x, doc->y, x + w, doc->y + h };
    Item *it = doc_item(doc, doc_add(doc, &box, paint_forge_box));
    it->data = s; it->arg = f; it->action = ACT_FORGE_FOCUS;
    s->rects[f] = (RECT){ x + px(10), box.top + (h - fh) / 2, x + w - px(10), box.top + (h - fh) / 2 + fh };
    s->laid[f] = true;
    doc->y = box.bottom;
    doc_space(doc, px(14));
}

static void forge_check_free(void *p) { ForgeCheck *d = p; free(d->text); free(d); }

static void paint_forge_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    ForgeCheck *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    int size = px(15), top = rc->top + (rc->bottom - rc->top - size) / 2;
    RECT b = { rc->left, top, rc->left + size, top + size };
    fill_round_rect(cv, &b, px(3), d->on ? theme.accent : theme.field, d->on ? theme.accent : hovered ? theme.accent_dim : theme.line_strong);
    if (d->on) draw_glyph(cv, 0xE73E, &b, FONT_ICON_SMALL, theme.on_accent);
    RECT t = { b.right + px(8), rc->top, rc->right, rc->bottom };
    draw_text(cv, d->text, &t, FONT_FOOTNOTE, d->gone ? theme.muted : theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

/// The projects the account is available to, one tick box each, as the web form's multiple select.
static void forge_projects(ForgeForm *s, Doc *doc, int x, int w) {
    doc_field_label(doc, x, w, "Projects", theme.ink, "Only these projects offer the account's Forge servers and sites to their clients.");
    doc_space(doc, px(6));
    Json *choices = forge_choices(s);
    const Json *rows = screen_settings_g_settings ? screen_settings_settings_rows(screen_settings_g_settings) : NULL;
    for (size_t i = 0; i < json_count(choices); i++) {
        const char *repo = json_str(json_at(choices, i));
        bool listed = false;
        for (size_t k = 0; k < json_count(rows); k++) listed |= str_eq(json_str(json_get(json_at(rows, k), "repo")), repo);
        ForgeCheck *d = xcalloc(1, sizeof *d);
        d->on = repos_has(s->repos, repo); d->gone = !listed;
        d->text = listed ? xstrdup(repo) : xstrfmt("%s (not a project)", repo);
        int it = doc_custom(doc, x, w, px(28), paint_forge_check, d, forge_check_free, ACT_FORGE_REPO, (intptr_t)i);
        doc_item(doc, it)->hand = true;
    }
    if (!json_count(choices)) doc_text(doc, x, w, "No projects yet. Add one under Projects first.", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
    json_free(choices);
    doc_space(doc, px(14));
}

void screen_settings_forge_layout(Screen *base, Doc *doc) {
    ForgeForm *s = (ForgeForm *)base;
    memset(s->laid, 0, sizeof s->laid);
    int col = doc->width, x = 0;
    doc_space(doc, px(8));
    int h = px(42), tx = x, ty = doc->y;
    s->tab_dot = s->dirty && screen_settings_forge_form_changed(s);
    doc_tab(doc, &tx, &ty, x, x + col, h, 0xE753, s->tab_dot ? "Forge account \xE2\x80\xA2" : "Forge account", NULL, true, 0, 0);
    doc->y = ty + h;
    doc_rule(doc, x, col);
    doc_space(doc, px(18));
    if (s->error) { doc_notice_box(doc, x, col, s->error); doc_space(doc, px(16)); }
    // Label and organization side by side, as the SSH form's `.field-row`; then the token and the projects across.
    int gap = px(14), half = (col - gap) / 2, top = doc->y;
    forge_field(s, doc, x, half, G_LABEL);
    int bottom = doc->y;
    doc->y = top;
    forge_field(s, doc, x + half + gap, col - half - gap, G_ORG);
    if (doc->y < bottom) doc->y = bottom;
    forge_field(s, doc, x, col, G_TOKEN);
    forge_projects(s, doc, x, col);
    doc_space(doc, px(40));
}

void screen_settings_forge_header(Screen *base, HeaderInfo *info) {
    ForgeForm *s = (ForgeForm *)base;
    const char *label = json_str_nonempty(json_get(s->row, "label")), *org = json_str_nonempty(json_get(s->row, "organization"));
    if (s->id) {
        snprintf(info->title, sizeof info->title, "%s", label ? label : org ? org : "Forge account");
        size_t n = json_count(json_get(s->row, "repos"));
        snprintf(info->subtitle, sizeof info->subtitle, "forge.laravel.com/%s \xC2\xB7 %zu project%s%s", org ? org : "", n, n == 1 ? "" : "s",
                 forge_has_token(s) ? "" : " \xC2\xB7 no token");
    } else {
        snprintf(info->title, sizeof info->title, "New Forge account");
        snprintf(info->subtitle, sizeof info->subtitle, "A Laravel Forge organization, and the projects whose clients may use it.");
    }
    bool busy = s->req_save || s->req_delete;
    HeaderButton *b = &info->buttons[info->button_count++];
    snprintf(b->label, sizeof b->label, "%s", s->req_save ? "Saving\xE2\x80\xA6" : "Save");
    b->glyph = 0xE74E; b->action = ACT_FORGE_SAVE; b->prominent = true; b->tip = "Save this Forge account (Ctrl+S)";
    b->enabled = !busy && (s->dirty || !s->id) && store_supports(s->id ? "update_forge_account" : "create_forge_account");
    if (!s->id) return;
    HeaderButton *d = &info->buttons[info->button_count++];
    d->glyph = 0xE74D; d->action = ACT_FORGE_DELETE; d->destructive = true; d->enabled = !busy && store_supports("delete_forge_account"); d->tip = "Delete this Forge account";
}

// MARK: The edits

void screen_settings_forge_place(Screen *base, const RECT *content, int scroll_y) {
    ForgeForm *s = (ForgeForm *)base;
    int m = screen_settings_margin_of(base->pane);
    for (int f = 0; f < G_COUNT; f++) {
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

static LRESULT CALLBACK forge_field_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    ForgeForm *s = (ForgeForm *)ref;
    int f = (int)id;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_TAB) { SetFocus(s->edits[(f + ((GetKeyState(VK_SHIFT) & 0x8000) ? G_COUNT - 1 : 1)) % G_COUNT]); return 0; }
        if (ctrl && wp == 'S') { screen_settings_forge_save(s); return 0; }
        if (ctrl && wp == 'A') { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        if (wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); return 0; }
        if (wp == VK_RETURN) { SetFocus(s->edits[(f + 1) % G_COUNT]); return 0; }
        break;
    case WM_CHAR:
        if (wp == '\t' || wp == 0x13 || wp == 0x01 || wp == 0x1B || wp == '\r') return 0;
        break;
    case WM_MOUSEWHEEL: SendMessageW(GetParent(hwnd), msg, wp, lp); return 0;
    case WM_NCDESTROY: RemoveWindowSubclass(hwnd, forge_field_proc, id); break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void screen_settings_forge_ensure_controls(ForgeForm *s) {
    if (s->edits[0]) return;
    HWND owner = pane_hwnd(s->base.pane);
    for (int f = 0; f < G_COUNT; f++) {
        DWORD style = WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | (f == G_TOKEN ? ES_PASSWORD : 0);
        HWND e = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, owner, (HMENU)(INT_PTR)(ID_FORGE_FIELD + f), GetModuleHandleW(NULL), NULL);
        SendMessageW(e, WM_SETFONT, (WPARAM)font(FORGE_FIELDS[f].mono ? FONT_MONO : FONT_BODY), TRUE);
        SendMessageW(e, EM_SETLIMITTEXT, f == G_TOKEN ? 4096 : 200, 0);
        wchar_t *w = utf8_to_wide(FORGE_FIELDS[f].cue); SendMessageW(e, EM_SETCUEBANNER, TRUE, (LPARAM)w); free(w);
        SetWindowSubclass(e, forge_field_proc, (UINT_PTR)f, (DWORD_PTR)s);
        theme_apply_control(e);
        s->edits[f] = e;
    }
    // The edits take the row's text; the projects ticked before they existed stay as they are.
    forge_fill_edits(s);
}

void screen_settings_forge_toggle_repo(ForgeForm *s, size_t index) {
    Json *choices = forge_choices(s);
    const char *repo = json_str(json_at(choices, index));
    if (repo) {
        Json *next = json_array();
        for (size_t i = 0; i < json_count(s->repos); i++) {
            const char *r = json_str(json_at(s->repos, i));
            if (r && !str_eq(r, repo)) json_array_push(next, json_string(r));
        }
        if (!repos_has(s->repos, repo)) json_array_push(next, json_string(repo));
        json_free(s->repos); s->repos = next;
        screen_settings_forge_changed(s);
        pane_relayout(s->base.pane);
    }
    json_free(choices);
}
