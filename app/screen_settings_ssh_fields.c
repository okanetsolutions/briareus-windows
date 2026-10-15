#include "screen_settings_internal.h"

static int ssh_field_tab(int f);
static char *ssh_text(SshForm *s, int f);
static void ssh_password_cue(SshForm *s);
static const char *ssh_saved_pick(const Json *row, int p);
static bool ssh_saved_enabled(const Json *row);
static bool ssh_form_changed(SshForm *s);
static int ssh_next_field(SshForm *s, int from, int step);
static LRESULT CALLBACK ssh_field_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);
static void ssh_login_done(void *owner, Request *req);

static const char *const SSH_PICK_KEYS[SP_COUNT] = { "repo", "permissionMode" };

const char *screen_settings_mode_title(const char *mode) { return str_eq(mode, "allow") ? "Don\xE2\x80\x99t ask anything" : "Ask for all commands"; }

static int ssh_field_tab(int f) { return f >= S_DBHOST ? ST_DATABASE : ST_SERVER; }

char *screen_settings_ssh_field_text(const Json *row, int f) {
    const Json *v = json_get(row, screen_settings_SSH_FIELDS[f].key);
    double n;
    if (screen_settings_SSH_FIELDS[f].kind == K_NUMBER) return json_num(v, &n) && isfinite(n) ? xstrfmt("%.10g", n) : xstrdup("");
    return xstrdup(json_str(v) ? json_str(v) : "");
}

/// A box's text as the form was filled: a saved server's database username is the one read back, never in its row.
static char *ssh_text(SshForm *s, int f) {
    if (f == S_DBUSER && !json_str(json_get(s->row, "dbUsername"))) return xstrdup(s->db_user ? s->db_user : "");
    return screen_settings_ssh_field_text(s->row, f);
}

bool screen_settings_ssh_has_login(SshForm *s) { return s->id && json_bool_is(json_get(s->row, "hasDbCredentials"), true); }

/// Whether the server stores a database login with each SSH server. One from before that drops the fields without a
/// word, so the Database tab is offered only when its catalog names the route that reads a login back.
bool screen_settings_ssh_db_offered(void) { return store_supports("ssh_db_credentials"); }

/// The password box's cue says whether leaving it empty keeps a stored password.
static void ssh_password_cue(SshForm *s) {
    if (!s->edits[S_DBPASS]) return;
    const char *cue = screen_settings_ssh_has_login(s) ? "stored; leave empty to keep it" : "empty = no password";
    wchar_t *w = utf8_to_wide(cue); SendMessageW(s->edits[S_DBPASS], EM_SETCUEBANNER, TRUE, (LPARAM)w); free(w);
}

/// A picker's value as the row has it saved; a server without a mode asks, as the server's own default does.
static const char *ssh_saved_pick(const Json *row, int p) {
    const char *v = json_str_nonempty(json_get(row, SSH_PICK_KEYS[p]));
    return v ? v : p == SP_MODE ? "ask" : "";
}

static bool ssh_saved_enabled(const Json *row) { return !json_bool_is(json_get(row, "enabled"), false); }

void screen_settings_ssh_fill(SshForm *s) {
    s->enabled = ssh_saved_enabled(s->row);
    for (int p = 0; p < SP_COUNT; p++) set_string(&s->picks[p], ssh_saved_pick(s->row, p));
    s->filling = true;
    for (int f = 0; f < S_COUNT; f++) {
        if (!s->edits[f]) continue;
        char *text = ssh_text(s, f);
        screen_settings_set_edit_text(s->edits[f], text);
        free(text);
    }
    s->filling = false;
    s->dirty = false;
    ssh_password_cue(s);
}

/// The body a save sends; NULL with `*why` when a field cannot be sent as it is.
Json *screen_settings_ssh_body(SshForm *s, char **why) {
    s->error_tab = ST_SERVER;
    Json *body = json_is_object(s->row) ? json_clone(s->row) : json_object();
    json_object_remove(body, "id");
    json_object_remove(body, "hasDbCredentials");
    json_object_remove(body, "dbUsername");
    json_object_remove(body, "dbPassword");
    if (!screen_settings_ssh_db_offered()) { json_object_remove(body, "dbHost"); json_object_remove(body, "dbPort"); }
    for (int f = 0; f < S_COUNT; f++) {
        if (!s->edits[f] || (ssh_field_tab(f) == ST_DATABASE && !screen_settings_ssh_db_offered())) continue;
        char *text = screen_settings_edit_text(s->edits[f]), *t = f == S_DBPASS ? xstrdup(text) : str_trim(text);
        free(text);
        if (f == S_DBUSER) {
            // Sent only when it changed: the server keeps the password with a new username, and empty clears the login.
            char *saved = ssh_text(s, f);
            if (!str_eq(t, saved) && (*t || s->db_user || json_str(json_get(s->row, "dbUsername")))) json_set_str(body, "dbUsername", t);
            free(saved);
        } else if (f == S_DBPASS) {
            // Left empty, the stored password stays.
            if (*t) json_set_str(body, "dbPassword", t);
        } else if (f == S_DBPORT && !*t) {
            json_object_remove(body, "dbPort");   // the server's own default, 3306
        } else if (screen_settings_SSH_FIELDS[f].kind == K_NUMBER) {
            char *end; long port = strtol(t, &end, 10);
            if (!*t || *end || port < 1 || port > 65535) {
                *why = xstrdup(f == S_DBPORT ? "Enter a database port from 1 to 65535." : "Enter a port from 1 to 65535.");
                s->error_tab = ssh_field_tab(f);
                free(t); json_free(body);
                return NULL;
            }
            json_set_num(body, screen_settings_SSH_FIELDS[f].key, (double)port);
        } else json_set_str(body, screen_settings_SSH_FIELDS[f].key, t);
        free(t);
    }
    for (int p = 0; p < SP_COUNT; p++) json_set_str(body, SSH_PICK_KEYS[p], s->picks[p] ? s->picks[p] : "");
    json_set_bool(body, "enabled", s->enabled);
    const char *missing = NULL;
    if (str_empty(json_str(json_get(body, "repo")))) missing = "Choose the project whose sessions may use this server.";
    else if (str_empty(json_str(json_get(body, "host")))) missing = "Enter the host: a hostname or an IP address.";
    else if (str_empty(json_str(json_get(body, "username")))) missing = "Enter the username to connect as.";
    else if (json_str_nonempty(json_get(body, "dbPassword"))) {
        // A password goes with a username: the one typed, else the one stored.
        const char *user = json_str(json_get(body, "dbUsername"));
        if (user ? !*user : !s->db_user ? !screen_settings_ssh_has_login(s) : !*s->db_user) { missing = "A database password needs a database username."; s->error_tab = ST_DATABASE; }
    }
    if (missing) { *why = xstrdup(missing); json_free(body); return NULL; }
    return body;
}

/// Whether a tab holds a change not saved yet, for the dot after its title.
bool screen_settings_ssh_tab_changed(SshForm *s, int tab) {
    if (tab == ST_SERVER) {
        if (s->enabled != ssh_saved_enabled(s->row)) return true;
        for (int p = 0; p < SP_COUNT; p++)
            if (!str_eq(s->picks[p] ? s->picks[p] : "", ssh_saved_pick(s->row, p))) return true;
    }
    for (int f = 0; f < S_COUNT; f++) {
        if (!s->edits[f] || ssh_field_tab(f) != tab) continue;
        char *now = screen_settings_edit_text(s->edits[f]), *saved = ssh_text(s, f);
        bool differs = !str_eq(now, saved);
        free(now); free(saved);
        if (differs) return true;
    }
    return false;
}

static bool ssh_form_changed(SshForm *s) { return screen_settings_ssh_tab_changed(s, ST_SERVER) || screen_settings_ssh_tab_changed(s, ST_DATABASE); }

void screen_settings_ssh_changed(SshForm *s) {
    if (s->filling) return;
    if (!s->dirty) { s->dirty = true; pane_header_changed(s->base.pane); }
    bool changed = ssh_form_changed(s);
    if (changed != s->tab_dot) { s->tab_dot = changed; pane_relayout(s->base.pane); }
}

// MARK: The edits

void screen_settings_ssh_place(Screen *base, const RECT *content, int scroll_y) {
    SshForm *s = (SshForm *)base;
    int m = screen_settings_margin_of(base->pane);
    for (int f = 0; f < S_COUNT; f++) {
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

static int ssh_next_field(SshForm *s, int from, int step) {
    for (int k = 1; k <= S_COUNT; k++) {
        int f = ((from + step * k) % S_COUNT + S_COUNT) % S_COUNT;
        if (s->edits[f] && s->laid[f]) return f;
    }
    return from;
}

static LRESULT CALLBACK ssh_field_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    SshForm *s = (SshForm *)ref;
    int f = (int)id;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_TAB) { SetFocus(s->edits[ssh_next_field(s, f, (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1)]); return 0; }
        if (ctrl && wp == 'S') { screen_settings_ssh_save(s); return 0; }
        if (ctrl && wp == 'A') { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        if (wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); return 0; }
        if (wp == VK_RETURN) { SetFocus(s->edits[ssh_next_field(s, f, 1)]); return 0; }
        break;
    case WM_CHAR:
        if (wp == '\t' || wp == 0x13 || wp == 0x01 || wp == 0x1B || wp == '\r') return 0;
        break;
    case WM_MOUSEWHEEL: SendMessageW(GetParent(hwnd), msg, wp, lp); return 0;
    case WM_NCDESTROY: RemoveWindowSubclass(hwnd, ssh_field_proc, id); break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void screen_settings_ssh_ensure_controls(SshForm *s) {
    if (s->edits[0]) return;
    HWND owner = pane_hwnd(s->base.pane);
    for (int f = 0; f < S_COUNT; f++) {
        DWORD style = WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | (screen_settings_SSH_FIELDS[f].kind == K_NUMBER ? ES_NUMBER : 0) | (f == S_DBPASS ? ES_PASSWORD : 0);
        HWND e = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, owner, (HMENU)(INT_PTR)(ID_SSH_FIELD + f), GetModuleHandleW(NULL), NULL);
        SendMessageW(e, WM_SETFONT, (WPARAM)font(screen_settings_SSH_FIELDS[f].mono ? FONT_MONO : FONT_BODY), TRUE);
        SendMessageW(e, EM_SETLIMITTEXT, screen_settings_SSH_FIELDS[f].kind == K_NUMBER ? 5 : 1024, 0);
        wchar_t *w = utf8_to_wide(screen_settings_SSH_FIELDS[f].cue); SendMessageW(e, EM_SETCUEBANNER, TRUE, (LPARAM)w); free(w);
        SetWindowSubclass(e, ssh_field_proc, (UINT_PTR)f, (DWORD_PTR)s);
        theme_apply_control(e);
        s->edits[f] = e;
    }
    // The edits take the row's text; what was picked or ticked before they existed stays as it is.
    s->filling = true;
    for (int f = 0; f < S_COUNT; f++) { char *text = ssh_text(s, f); screen_settings_set_edit_text(s->edits[f], text); free(text); }
    s->filling = false;
    ssh_password_cue(s);
}

/// The stored database username, read back so the box shows it; the password that comes with it is not kept.
static void ssh_login_done(void *owner, Request *req) {
    SshForm *s = owner;
    const Json *login = req->ok ? json_get(req->result, "credentials") : NULL;
    if (!json_is_object(login)) { request_error_into(&s->login_error, req); pane_relayout(s->base.pane); return; }
    set_string(&s->login_error, NULL);
    const char *user = json_str(json_get(login, "username"));
    // The box takes it unless something was typed in it meanwhile.
    char *now = s->edits[S_DBUSER] ? screen_settings_edit_text(s->edits[S_DBUSER]) : xstrdup("");
    bool untouched = !*now;
    free(now);
    set_string(&s->db_user, user ? user : "");
    if (untouched && s->edits[S_DBUSER]) { s->filling = true; screen_settings_set_edit_text(s->edits[S_DBUSER], s->db_user); s->filling = false; }
    // Filling the box is no edit; one typed meanwhile is now measured against the username read back.
    if (s->dirty) screen_settings_ssh_changed(s);
    pane_relayout(s->base.pane);
}

void screen_settings_ssh_load_login(SshForm *s) {
    if (s->db_user || s->req_login || s->login_error || !screen_settings_ssh_has_login(s) || !store_supports("ssh_db_credentials")) return;
    Json *args = json_object(); json_set_num(args, "id", s->id);
    store_call("ssh_db_credentials", args, 0, s, ssh_login_done, 0, &s->req_login);
}

/// The project or permission mode picker, as a menu under the pointer.
void screen_settings_ssh_pick(SshForm *s, int p, POINT pt) {
    HMENU m = CreatePopupMenu();
    const char *now = s->picks[p] ? s->picks[p] : "";
    const Json *rows = screen_settings_g_settings ? screen_settings_settings_rows(screen_settings_g_settings) : NULL;
    size_t n = 0;
    if (p == SP_MODE) {
        screen_settings_append_item(m, 1, screen_settings_mode_title("ask"), str_eq(now, "ask"), true);
        screen_settings_append_item(m, 2, screen_settings_mode_title("allow"), str_eq(now, "allow"), true);
    } else {
        n = json_count(rows);
        bool listed = false;
        for (size_t i = 0; i < n; i++) {
            const char *repo = json_str(json_get(json_at(rows, i), "repo"));
            if (!repo) continue;
            listed |= str_eq(repo, now);
            screen_settings_append_item(m, (UINT)i + 1, repo, str_eq(repo, now), true);
        }
        // A project that is gone stays picked rather than being swapped for another; the server refuses it on save.
        if (*now && !listed) { char *label = xstrfmt("%s (not a project)", now); screen_settings_append_item(m, 9999, label, true, false); free(label); }
        if (!n && !*now) screen_settings_append_item(m, 9999, "No projects yet", false, false);
    }
    int chosen = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(s->base.pane), NULL);
    DestroyMenu(m);
    const char *value = NULL;
    if (p == SP_MODE && (chosen == 1 || chosen == 2)) value = chosen == 1 ? "ask" : "allow";
    else if (p == SP_REPO && chosen >= 1 && (size_t)chosen - 1 < n) value = json_str(json_get(json_at(rows, (size_t)chosen - 1), "repo"));
    if (value && !str_eq(value, now)) { set_string(&s->picks[p], value); screen_settings_ssh_changed(s); pane_relayout(s->base.pane); }
}
