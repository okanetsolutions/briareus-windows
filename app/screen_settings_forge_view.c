#include "screen_settings_internal.h"

static bool forge_has_token(const ForgeForm *s);
static void forge_fill_edits(ForgeForm *s);
static void paint_forge_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void forge_field(ForgeForm *s, Doc *doc, int x, int w, int f);
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
    screen_settings_repos_layout(doc, x, col, s->repos, ACT_FORGE_REPO, "Only these projects offer the account's Forge servers and sites to their clients.");
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
    if (screen_settings_repos_toggle(&s->repos, index)) {
        screen_settings_forge_changed(s);
        pane_relayout(s->base.pane);
    }
}
