#include "screen_settings_internal.h"

static void form_set_id(FormScreen *s, int id);
static void save_done(void *owner, Request *req);
static void delete_done(void *owner, Request *req);
static void form_delete(FormScreen *s);
static void form_clone(FormScreen *s);
static void form_destroy(Screen *base);
static void form_action(Screen *base, int action, intptr_t arg, POINT pt);
static void form_command(Screen *base, int id, int code, HWND control);
static bool form_key(Screen *base, WPARAM vk, bool ctrl, bool shift);
static void form_visible(Screen *base, bool shown);
static bool form_can_leave(Screen *base);

// MARK: Saving, cloning, deleting

static void form_set_id(FormScreen *s, int id) {
    s->id = id;
    free(s->base.id); s->base.id = screen_settings_form_id(id);
    settings_select_id(s->base.id);
}

static void save_done(void *owner, Request *req) {
    FormScreen *s = owner;
    const Json *row = req->ok ? json_get(req->result, "project") : NULL;
    if (!json_is_object(row)) {
        char *text = request_error_or_unexpected(req);
        set_string(&s->error, text); free(text);
        pane_relayout(s->base.pane); pane_header_changed(s->base.pane); pane_scroll_to_top(s->base.pane);
        return;
    }
    // The server's word on what was saved: it may have tidied a value, and a new project now has an id.
    json_free(s->row); s->row = json_clone(row);
    set_string(&s->error, NULL);
    form_set_id(s, screen_settings_row_id(row));
    screen_settings_form_fill(s);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
    settings_projects_changed(s->id);
}

void screen_settings_form_save(FormScreen *s) {
    if (s->req_save || s->req_delete || !store_supports(s->id ? "update_project" : "create_project")) return;
    char *why = NULL; int tab = screen_settings_g_tab;
    Json *body = screen_settings_form_body(s, &why, &tab);
    if (!body) {
        // The tab holding the field at fault comes up with the reason above it.
        set_string(&s->error, why); free(why);
        screen_settings_g_tab = tab;
        pane_relayout(s->base.pane); pane_scroll_to_top(s->base.pane);
        return;
    }
    if (s->id) json_set_num(body, "id", s->id);
    store_call(s->id ? "update_project" : "create_project", body, 0, s, save_done, 0, &s->req_save);
    pane_header_changed(s->base.pane);
}

static void delete_done(void *owner, Request *req) {
    FormScreen *s = owner;
    if (!req->ok) {
        request_error_into(&s->error, req);
        pane_relayout(s->base.pane); pane_header_changed(s->base.pane); pane_scroll_to_top(s->base.pane);
        return;
    }
    // The list says the project is gone, and the sidebar opens the first one left in its place.
    s->dirty = false;
    app_clear_detail();
    settings_projects_changed(0);
}

static void form_delete(FormScreen *s) {
    if (!s->id || s->req_save || s->req_delete) return;
    const char *name = json_str_nonempty(json_get(s->row, "repo"));
    char *title = xstrfmt("Delete %s?", name ? name : "this project");
    bool ok = app_confirm(title, "Sessions already started against it keep their history, but no new one can be.", "Delete", true);
    free(title);
    if (!ok) return;
    Json *args = json_object(); json_set_num(args, "id", s->id);
    store_call("delete_project", args, 0, s, delete_done, 0, &s->req_delete);
    pane_header_changed(s->base.pane);
}

static void form_clone(FormScreen *s) {
    char *why = NULL; int tab = screen_settings_g_tab;
    Json *copy = screen_settings_form_body(s, &why, &tab);
    if (!copy) { set_string(&s->error, why); free(why); screen_settings_g_tab = tab; pane_relayout(s->base.pane); pane_scroll_to_top(s->base.pane); return; }
    // The copy carries what the form holds now, saved or not; the repository is unique, so it starts without one.
    json_set_str(copy, "repo", "");
    s->dirty = false;
    Screen *clone = project_settings_screen_new(copy, NULL);
    ((FormScreen *)clone)->focus_first = F_REPO;
    screen_settings_g_tab = T_PROJECT;
    json_free(copy);
    app_show_detail(clone);
}

// MARK: The screen

static void form_destroy(Screen *base) {
    FormScreen *s = (FormScreen *)base;
    request_cancel(&s->req_save); request_cancel(&s->req_delete); request_cancel(&s->req_runtimes);
    for (int f = 0; f < F_COUNT; f++) if (s->edits[f]) DestroyWindow(s->edits[f]);
    for (int r = 0; r < R_COUNT; r++) screen_settings_pick_clear(&s->picks[r]);
    if (s->has_catalog) runtime_catalog_free(&s->catalog);
    json_free(s->row); free(s->error);
    screen_release(base);
}

static void form_action(Screen *base, int action, intptr_t arg, POINT pt) {
    FormScreen *s = (FormScreen *)base;
    switch (action) {
    case ACT_SAVE: screen_settings_form_save(s); break;
    case ACT_CLONE: form_clone(s); break;
    case ACT_DELETE: form_delete(s); break;
    case ACT_TAB:
        if (arg < 0 || arg >= T_COUNT) break;
        // The focus leaves with the boxes of the tab that closes.
        if (s->focused >= 0) SetFocus(pane_hwnd(base->pane));
        screen_settings_g_tab = (int)arg;
        pane_relayout(base->pane); pane_scroll_to_top(base->pane);
        break;
    case ACT_TOGGLE: if (arg >= 0 && arg < F_COUNT) { s->bools[arg] = !s->bools[arg]; screen_settings_form_changed(s); pane_relayout(base->pane); } break;
    case ACT_PICK: if (arg >= 0 && arg < R_COUNT * 3) screen_settings_pick(s, (int)arg / 3, (int)arg % 3, pt); break;
    case ACT_FOCUS: if (arg >= 0 && arg < F_COUNT && s->edits[arg] && screen_settings_field_enabled(s, (int)arg)) SetFocus(s->edits[arg]); break;
    }
}

static void form_command(Screen *base, int id, int code, HWND control) {
    (void)control;
    FormScreen *s = (FormScreen *)base;
    int f = id - ID_FIELD;
    if (f < 0 || f >= F_COUNT || !s->edits[f]) return;
    switch (code) {
    case EN_CHANGE:
        screen_settings_form_changed(s);
        if (screen_settings_is_multiline(f)) {
            char *text = screen_settings_edit_text(s->edits[f]);
            int n = screen_settings_line_count(text);
            free(text);
            if (n != s->lines[f]) { s->lines[f] = n; pane_relayout(base->pane); }
        }
        break;
    case EN_SETFOCUS: {
        s->focused = f;
        // Tabbing onto a box out of view brings it into view.
        RECT content = pane_content_rect(base->pane);
        int top = s->rects[f].top - px(40), bottom = s->rects[f].bottom + px(16), y = pane_scroll_y(base->pane);
        if (top < y || bottom > y + (content.bottom - content.top)) pane_scroll_to(base->pane, top);
        pane_repaint(base->pane);
        break;
    }
    case EN_KILLFOCUS: if (s->focused == f) s->focused = -1; pane_repaint(base->pane); break;
    }
}

static bool form_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)shift;
    if (ctrl && vk == 'S') { screen_settings_form_save((FormScreen *)base); return true; }
    return false;
}

static void form_visible(Screen *base, bool shown) {
    FormScreen *s = (FormScreen *)base;
    s->shown = shown;
    if (shown) {
        screen_settings_form_ensure_controls(s);
        // Asked once the screen is up: a screen made and then refused (its predecessor kept its changes) asks nothing.
        if (!s->has_catalog && !s->req_runtimes) screen_settings_load_runtimes(s);
        if (s->focus_first >= 0) { int f = s->focus_first; s->focus_first = -1; SetFocus(s->edits[f]); }
    } else for (int f = 0; f < F_COUNT; f++) if (s->edits[f]) ShowWindow(s->edits[f], SW_HIDE);
}

static bool form_can_leave(Screen *base) {
    FormScreen *s = (FormScreen *)base;
    if (!s->dirty) return true;
    const char *name = json_str_nonempty(json_get(s->row, "label"));
    char *message = s->id ? xstrfmt("The changes to %s have not been saved.", name ? name : "this project") : xstrdup("The new project has not been saved.");
    bool leave = app_confirm("Discard unsaved changes?", message, "Discard", true);
    free(message);
    if (leave) s->dirty = false;
    return leave;
}

static const ScreenVTable form_vt = {
    .destroy = form_destroy, .layout = screen_settings_form_layout, .header = screen_settings_form_header, .action = form_action, .place = screen_settings_form_place,
    .visible = form_visible, .command = form_command, .key = form_key, .can_leave = form_can_leave,
};

Screen *project_settings_screen_new(const Json *row, const Json *defaults) {
    FormScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &form_vt;
    s->focused = -1; s->focus_first = -1;
    // A saved row, a clone's values without an id, or a new project from the server's defaults.
    s->row = json_is_object(row) ? json_clone(row) : json_is_object(defaults) ? json_clone(defaults) : json_object();
    s->id = screen_settings_row_id(s->row);
    s->base.id = screen_settings_form_id(s->id);
    // A new project starts where its repository is typed.
    if (!s->id) { s->focus_first = F_REPO; screen_settings_g_tab = T_PROJECT; }
    screen_settings_form_fill(s);
    return &s->base;
}
