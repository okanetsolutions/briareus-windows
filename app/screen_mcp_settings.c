// MCP registry only; core mounts the tools and runs commands on its own machine.
#include "screens.h"
#include "mcp.h"
#include "str.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool mcp_settings_supported(const char *operation) {
    return g_store.has_device && str_eq(g_store.device.permission, "admin") && store_supports(operation);
}
enum { F_NAME, F_LABEL, F_URL, F_COMMAND, F_ARGS, F_HEADERS, F_ENV, F_CLIENT_ID, F_CLIENT_SECRET, F_SCOPE, F_CLIENT_NAME, F_CALLBACK, F_COUNT };
static const struct { const char *key, *label; int tab; bool multi, secret; } FIELDS[F_COUNT] = {
    { "name", "Tool name", 0, false, false }, { "label", "Label", 0, false, false },
    { "url", "HTTP endpoint", 0, false, false }, { "command", "Command on the core machine", 0, false, false },
    { "args", "Arguments (JSON array; spaces and empty strings are preserved)", 0, true, false },
    { "headers", "Replacement headers (JSON object)", 0, true, false }, { "env", "Replacement environment (JSON object)", 0, true, false },
    { "oauthClientId", "OAuth client ID (empty = register automatically)", 2, false, false },
    { "oauthClientSecret", "Replacement OAuth client secret", 2, false, true },
    { "oauthScope", "OAuth scope", 2, false, false }, { "oauthClientName", "OAuth client name", 2, false, false },
    { NULL, "Complete callback URL from the browser", 0, false, true },
};
enum { ACT_SAVE = 1200, ACT_DELETE, ACT_TAB, ACT_TRANSPORT, ACT_ENABLED, ACT_ALL, ACT_REPO, ACT_SECRET, ACT_REDIRECT, ACT_CONNECT, ACT_SIGN_IN, ACT_BROWSER, ACT_FINISH, ACT_FOCUS, ACT_RELOAD };
enum { ID_FIELD = 2100, TIMER_STATUS = 16 };
typedef struct {
    Screen base;
    ApiClient *account;
    Json *row, *repos;
    double id;
    HWND edits[F_COUNT]; RECT rects[F_COUNT]; bool laid[F_COUNT], clipped[F_COUNT];
    bool shown, filling, dirty, stdio, enabled, all, loopback, uncertain, conflict, create_retry, read_error;
    int tab;
    McpSecretMode secrets[3];
    McpSignIn sign_in;
    Request *read, *write;
    Poller poll;
    char *error, *create_name;
} McpForm;

static char *form_id(double id) { return id ? xstrfmt("settings-mcp:%.0f", id) : xstrdup("settings-mcp:new"); }
static bool account_current(McpForm *s) { return s->account && s->account == g_store.client && mcp_settings_supported("settings_mcp_servers"); }
static void open_sign_in(const char *url) {
    if (!mcp_secure_url(url)) return;
    wchar_t *w = utf8_to_wide(url);
    ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL); free(w);
}
static bool busy(McpForm *s) { return s->write != NULL; }
static bool can_save(McpForm *s) { return !busy(s) && (s->dirty || !s->id) && (!s->uncertain || (!s->id && s->create_retry)); }
static char *edit_text(HWND edit) {
    int n = GetWindowTextLengthW(edit);
    wchar_t *w = xmalloc(((size_t)n + 1) * sizeof *w); GetWindowTextW(edit, w, n + 1);
    char *raw = wide_to_utf8(w), *lf = str_replace(raw, "\r\n", "\n"); free(raw); free(w); return lf;
}
static void set_text(HWND edit, const char *text) {
    char *crlf = str_replace(text ? text : "", "\n", "\r\n"); wchar_t *w = utf8_to_wide(crlf);
    SetWindowTextW(edit, w); free(w); free(crlf);
}
static void fill(McpForm *s) {
    s->filling = true;
    s->stdio = str_eq(json_str(json_get(s->row, "transport")), "stdio");
    s->enabled = !json_bool_is(json_get(s->row, "enabled"), false);
    s->loopback = str_eq(json_str(json_get(s->row, "oauthRedirect")), "loopback");
    json_free(s->repos); s->repos = json_is_array(json_get(s->row, "repos")) ? json_clone(json_get(s->row, "repos")) : json_array();
    s->all = !json_count(s->repos);
    for (int f = 0; f < F_CALLBACK; f++) {
        if (!s->edits[f]) continue;
        char *text;
        if (f == F_ARGS) text = json_is_array(json_get(s->row, "args")) ? json_serialize(json_get(s->row, "args"), false) : xstrdup("[]");
        else if (f == F_HEADERS || f == F_ENV) text = xstrdup("{}");
        else text = xstrdup(FIELDS[f].key && f != F_CLIENT_SECRET ? json_str_or(json_get(s->row, FIELDS[f].key), "") : "");
        set_text(s->edits[f], text); free(text);
    }
    memset(s->secrets, 0, sizeof s->secrets);
    s->dirty = false; s->conflict = false; s->filling = false;
}
static void changed(McpForm *s) {
    if (s->filling || s->dirty) return;
    s->dirty = true; pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}
static void repaint(McpForm *s) { pane_relayout(s->base.pane); pane_header_changed(s->base.pane); }
static void error_text(McpForm *s, const char *error) { s->read_error = false; set_string(&s->error, error); repaint(s); }
static bool field_shown(McpForm *s, int f) {
    if (FIELDS[f].tab != s->tab) return false;
    switch (f) {
    case F_URL: return !s->stdio;
    case F_COMMAND: case F_ARGS: return s->stdio;
    case F_HEADERS: return !s->stdio && s->secrets[0] == MCP_REPLACE;
    case F_ENV: return s->stdio && s->secrets[1] == MCP_REPLACE;
    case F_CLIENT_SECRET: return s->secrets[2] == MCP_REPLACE;
    case F_CALLBACK: return mcp_sign_in_can_finish(&s->sign_in);
    default: return true;
    }
}
static bool repos_changed(McpForm *s) {
    const Json *stored = json_get(s->row, "repos");
    if (s->all) return json_count(stored) != 0;
    if (json_count(stored) != json_count(s->repos)) return true;
    for (size_t i = 0; i < json_count(stored); i++) {
        bool found = false;
        for (size_t k = 0; k < json_count(s->repos); k++) if (json_equal(json_at(stored, i), json_at(s->repos, k))) found = true;
        if (!found) return true;
    }
    return false;
}
static bool nul_escape(const char *text) {
    // Skip escaped backslashes so literal \\u0000 remains ordinary text.
    for (const char *p = text; *p; p++) if (*p == '\\') {
        p++;
        if (!*p) break;
        if (str_has_prefix(p, "u0000")) return true;
    }
    return false;
}
static Json *body_now(McpForm *s, char **error) {
    Json *fields = json_object();
    for (int f = 0; f < F_CALLBACK; f++) {
        char *text = edit_text(s->edits[f]);
        if (f == F_URL && s->id && !s->stdio && str_eq(json_str(json_get(s->row, "transport")), "http")) {
            char *trim = str_trim(text);
            bool unchanged = str_eq(trim, json_str(json_get(s->row, "url")));
            free(trim);
            if (unchanged) { free(text); continue; }
        }
        bool authored_json = (f == F_ARGS && s->stdio) || (f == F_HEADERS && !s->stdio && s->secrets[0] == MCP_REPLACE) || (f == F_ENV && s->stdio && s->secrets[1] == MCP_REPLACE);
        if (authored_json && nul_escape(text)) {
            *error = xstrfmt("%s must not contain NUL escapes.", FIELDS[f].label);
            free(text); json_free(fields); return NULL;
        }
        if (f == F_ARGS || f == F_HEADERS || f == F_ENV) json_object_set(fields, FIELDS[f].key, json_parsez(text));
        else json_set_str(fields, FIELDS[f].key, text);
        free(text);
    }
    json_set_str(fields, "transport", s->stdio ? "stdio" : "http");
    json_set_str(fields, "oauthRedirect", s->loopback ? "loopback" : "callback");
    json_set_bool(fields, "enabled", s->enabled);
    json_object_set(fields, "repos", s->all ? json_array() : json_clone(s->repos));
    Json *body = NULL;
    if (s->enabled && !s->all && !json_count(s->repos)) *error = xstrdup("Select at least one repository, or choose all; disable the server to mount it nowhere.");
    else body = mcp_form_body(fields, s->stdio ? MCP_KEEP : s->secrets[0], s->stdio ? s->secrets[1] : MCP_KEEP, s->secrets[2], error);
    if (body && s->id && !repos_changed(s)) json_object_remove(body, "repos");
    if (body && s->id && json_equal(json_get(body, "transport"), json_get(s->row, "transport"))) {
        // Core checks connections by key presence, so metadata edits must omit unchanged connection fields.
        static const char *const connection[] = { "transport", "url", "command", "args", "oauthClientId", "oauthScope", "oauthClientName", "oauthRedirect" };
        for (size_t i = 0; i < sizeof connection / sizeof *connection; i++)
            if (json_equal(json_get(body, connection[i]), json_get(s->row, connection[i]))) json_object_remove(body, connection[i]);
    }
    json_free(fields); return body;
}
static void note(Doc *doc, int w, const char *text) { doc_text(doc, 0, w, text, FONT_FOOTNOTE, theme.muted, DT_WORDBREAK); doc_space(doc, px(12)); }
static void paint_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc) { (void)doc; (void)it; fill_round_rect(cv, rc, px(6), theme.raise, theme.line); }
static void field(McpForm *s, Doc *doc, int w, int f) {
    if (!field_shown(s, f)) return;
    doc_field_label(doc, 0, w, FIELDS[f].label, theme.ink, NULL); doc_space(doc, px(6));
    int h = FIELDS[f].multi ? px(100) : px(36);
    RECT r = { 0, doc->y, w, doc->y + h };
    Item *it = doc_item(doc, doc_add(doc, &r, paint_box)); it->action = ACT_FOCUS; it->arg = f;
    s->rects[f] = (RECT){ px(8), r.top + px(7), w - px(8), r.bottom - px(7) }; s->laid[f] = true;
    doc->y = r.bottom + px(14);
}
static bool selected(McpForm *s, const char *repo) {
    for (size_t i = 0; i < json_count(s->repos); i++) if (str_eq(repo, json_str(json_at(s->repos, i)))) return true;
    return false;
}
static void secret_mode(McpForm *s, Doc *doc, int w, int index, const char *title, const Json *names) {
    static const char *const modes[] = { "Keep stored", "Replace whole set", "Clear stored" };
    char *stored = index == 2 ? xstrdup(json_bool_is(json_get(s->row, "hasOAuthClientSecret"), true) ? "stored" : "none stored") : json_serialize(names, false);
    char *label = xstrfmt("%s: %s (%s)", title, modes[s->secrets[index]], stored);
    doc_button(doc, 0, w, label, BUTTON_BORDERED, ACT_SECRET, index, !busy(s));
    free(stored); free(label); doc_space(doc, px(12));
}
static void form_layout(Screen *base, Doc *doc) {
    McpForm *s = (McpForm *)base; int w = doc->width;
    memset(s->laid, 0, sizeof s->laid);
    if (!account_current(s)) { note(doc, w, "MCP settings require the same connected account, an Admin token and a server offering the MCP API. Reopen Settings after reconnecting."); return; }
    int tx = 0, ty = doc->y;
    const char *const tabs[] = { "Connection", "Repositories", "Advanced OAuth" };
    for (int t = 0; t < 3; t++) doc_tab(doc, &tx, &ty, 0, w, px(42), 0xE713, tabs[t], NULL, s->tab == t, ACT_TAB, t);
    doc->y = ty + px(50);
    if (s->error) { doc_notice_box(doc, 0, w, s->error); doc_space(doc, px(14)); }
    if (s->conflict) {
        note(doc, w, "This server's configuration changed on core. Your draft is preserved; reload the current fields or confirm replacing them when saving.");
        doc_button(doc, 0, w, "Reload current server fields", BUTTON_BORDERED, ACT_RELOAD, 0, !busy(s)); doc_space(doc, px(12));
    }
    if (!s->tab) {
        const char *status = json_str_or(json_get(s->row, "status"), "unchecked");
        char *label = xstrfmt("Status: %s%s", status, json_bool_is(json_get(s->row, "signedIn"), true) ? " · OAuth sign-in stored" : "");
        note(doc, w, label); free(label);
        const char *diagnostic = json_str_nonempty(json_get(s->row, "error")); if (diagnostic) note(doc, w, diagnostic);
        if (s->id) {
            bool can = !busy(s) && !s->dirty && !s->uncertain && mcp_settings_supported("connect_mcp_server");
            doc_button(doc, 0, w, "Check connection", BUTTON_BORDERED, ACT_CONNECT, 0, can); doc_space(doc, px(8));
            if (!s->stdio) { doc_button(doc, 0, w, "Sign in again", BUTTON_BORDERED, ACT_SIGN_IN, 0, can); doc_space(doc, px(8)); }
        }
        if (s->sign_in.url && !s->sign_in.blocked) {
            note(doc, w, s->sign_in.paste ? "Approve in the browser, then paste the complete callback address, even if that page cannot load." : "Approve in the browser; core finishes the callback. Status refreshes while this screen is active.");
            doc_button(doc, 0, w, "Open sign-in in browser", BUTTON_BORDERED, ACT_BROWSER, 0, !busy(s) && !s->dirty);
            doc_space(doc, px(8)); field(s, doc, w, F_CALLBACK);
            if (s->sign_in.paste) { doc_button(doc, 0, w, "Finish sign-in", BUTTON_PROMINENT, ACT_FINISH, 0, !busy(s) && !s->dirty && mcp_settings_supported("finish_mcp_sign_in")); doc_space(doc, px(12)); }
        } else if (str_eq(status, "needs-sign-in")) note(doc, w, "There is no usable sign-in link. It may have expired, been used, or been invalidated. Choose Sign in again to start a fresh attempt.");
        doc_button(doc, 0, w, s->stdio ? "Transport: stdio" : "Transport: HTTP", BUTTON_BORDERED, ACT_TRANSPORT, 0, !busy(s)); doc_space(doc, px(12));
        note(doc, w, "Commands, environment and loopback endpoints run on the core machine. Core mounts tools into Claude/Codex turns; changes apply on their next turn.");
        for (int f = F_NAME; f <= F_ARGS; f++) field(s, doc, w, f);
        secret_mode(s, doc, w, s->stdio ? 1 : 0, s->stdio ? "Environment" : "Headers", json_get(s->row, s->stdio ? "envNames" : "headerNames"));
        note(doc, w, "Keep omits secrets from the save. Replace sends the whole set as a JSON object, including unchanged entries. Clear sends an empty object. An Authorization header bypasses OAuth. Switching transport removes the other transport's secrets on core.");
        field(s, doc, w, F_HEADERS); field(s, doc, w, F_ENV);
    } else if (s->tab == 1) {
        doc_button(doc, 0, w, s->enabled ? "Enabled: sessions get this server" : "Disabled: no sessions get this server", BUTTON_BORDERED, ACT_ENABLED, 0, !busy(s)); doc_space(doc, px(12));
        doc_button(doc, 0, w, s->all ? "All repositories (including future ones)" : "Selected repositories", BUTTON_BORDERED, ACT_ALL, 0, !busy(s)); doc_space(doc, px(12));
        if (!s->all) {
            const Json *projects = settings_project_rows();
            for (size_t i = 0; i < json_count(projects); i++) {
                const Json *p = json_at(projects, i); const char *repo = json_str_nonempty(json_get(p, "repo")); if (!repo) continue;
                char *label = xstrfmt("%s %s%s", selected(s, repo) ? "[x]" : "[ ]", repo, json_bool_is(json_get(p, "enabled"), false) ? " (inactive project)" : "");
                doc_button(doc, 0, w, label, BUTTON_BORDERED, ACT_REPO, (intptr_t)i, !busy(s)); free(label); doc_space(doc, px(6));
            }
            // Keep assignments that disappeared from the project picker visible and removable.
            for (size_t i = 0; i < json_count(s->repos); i++) {
                const char *repo = json_str(json_at(s->repos, i)); bool known = false;
                for (size_t k = 0; k < json_count(projects); k++) if (str_eq(repo, json_str(json_get(json_at(projects, k), "repo")))) known = true;
                if (!known) { char *label = xstrfmt("[x] %s (unavailable; click to remove)", repo); doc_button(doc, 0, w, label, BUTTON_BORDERED, ACT_REPO, -(intptr_t)i - 1, !busy(s)); free(label); doc_space(doc, px(6)); }
            }
        }
    } else {
        for (int f = F_CLIENT_ID; f <= F_CLIENT_NAME; f++) field(s, doc, w, f);
        secret_mode(s, doc, w, 2, "Client secret", NULL);
        note(doc, w, "Keep omits the client secret. Replace sends the new value as typed. Clear sends an empty string.");
        doc_button(doc, 0, w, s->loopback ? "Redirect: loopback (paste back)" : "Redirect: core callback", BUTTON_BORDERED, ACT_REDIRECT, 0, !busy(s));
    }
    doc_space(doc, px(30));
}
static void form_header(Screen *base, HeaderInfo *info) {
    McpForm *s = (McpForm *)base;
    snprintf(info->title, sizeof info->title, "%s", s->id ? json_str_or(json_get(s->row, "label"), "MCP server") : "New MCP server");
    snprintf(info->subtitle, sizeof info->subtitle, "MCP settings%s", s->dirty ? " · unsaved changes" : "");
    if (!account_current(s)) return;
    HeaderButton *b = &info->buttons[info->button_count++]; b->glyph = 0xE74E; b->action = ACT_SAVE; b->prominent = true;
    snprintf(b->label, sizeof b->label, "Save"); b->enabled = can_save(s) && mcp_settings_supported(s->id ? "update_mcp_server" : "create_mcp_server");
    if (s->id) { b = &info->buttons[info->button_count++]; b->glyph = 0xE74D; b->action = ACT_DELETE; b->destructive = true; b->tip = "Delete MCP server"; b->enabled = !busy(s) && mcp_settings_supported("delete_mcp_server"); }
}
static void form_place(Screen *base, const RECT *content, int scroll_y) {
    McpForm *s = (McpForm *)base; RECT client; GetClientRect(pane_hwnd(base->pane), &client); int margin = (client.right - pane_content_width(base->pane)) / 2;
    for (int f = 0; f < F_COUNT; f++) {
        HWND e = s->edits[f]; if (!e) continue;
        EnableWindow(e, account_current(s) && !busy(s));
        if (!s->shown || !s->laid[f]) { ShowWindow(e, SW_HIDE); continue; }
        RECT r = { content->left + margin + s->rects[f].left, content->top + s->rects[f].top - scroll_y, content->left + margin + s->rects[f].right, content->top + s->rects[f].bottom - scroll_y }, visible;
        if (!IntersectRect(&visible, &r, content)) { ShowWindow(e, SW_HIDE); continue; }
        MoveWindow(e, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
        bool clipped = !EqualRect(&visible, &r);
        if (clipped) SetWindowRgn(e, CreateRectRgn(visible.left - r.left, visible.top - r.top, visible.right - r.left, visible.bottom - r.top), TRUE);
        else if (s->clipped[f]) SetWindowRgn(e, NULL, TRUE);
        s->clipped[f] = clipped; ShowWindow(e, SW_SHOWNA);
    }
}
static void form_save(McpForm *s);
static LRESULT CALLBACK edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    McpForm *s = (McpForm *)ref;
    if (msg == WM_KEYDOWN) {
        if (wp == VK_TAB) {
            int step = GetKeyState(VK_SHIFT) & 0x8000 ? -1 : 1;
            for (int k = 1; k <= F_COUNT; k++) {
                int f = ((int)id + step * k + F_COUNT) % F_COUNT;
                if (!s->laid[f]) continue;
                RECT content = pane_content_rect(s->base.pane);
                int top = pane_scroll_y(s->base.pane);
                if (s->rects[f].top < top || s->rects[f].bottom > top + content.bottom - content.top)
                    pane_scroll_to(s->base.pane, s->rects[f].top);
                SetFocus(s->edits[f]); break;
            }
            return 0;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wp == 'S') { form_save(s); return 0; }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wp == 'A') { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
    }
    if (msg == WM_CHAR && (wp == '\t' || wp == 0x13 || wp == 0x01)) return 0;
    if (msg == WM_MOUSEWHEEL) { SendMessageW(GetParent(hwnd), msg, wp, lp); return 0; }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, edit_proc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}
static void ensure_controls(McpForm *s) {
    if (s->edits[0]) return;
    for (int f = 0; f < F_COUNT; f++) {
        DWORD style = WS_CHILD | WS_TABSTOP | (FIELDS[f].multi ? ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL : ES_AUTOHSCROLL) | (FIELDS[f].secret ? ES_PASSWORD : 0);
        s->edits[f] = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, pane_hwnd(s->base.pane), (HMENU)(INT_PTR)(ID_FIELD + f), GetModuleHandleW(NULL), NULL);
        SendMessageW(s->edits[f], WM_SETFONT, (WPARAM)font(FONT_MONO), TRUE); SendMessageW(s->edits[f], EM_SETLIMITTEXT, 300000, 0);
        SetWindowSubclass(s->edits[f], edit_proc, (UINT_PTR)f, (DWORD_PTR)s);
    }
    fill(s);
}
static void status_update(McpForm *s, const Json *row) {
    char *previous = xstrdup(s->sign_in.url); mcp_sign_in_update(&s->sign_in, row);
    if (!str_eq(previous, s->sign_in.url) && s->edits[F_CALLBACK]) set_text(s->edits[F_CALLBACK], "");
    free(previous);
    static const char *const config[] = { "name", "label", "transport", "url", "command", "args", "repos", "enabled", "oauthClientId", "oauthScope", "oauthClientName", "oauthRedirect", "headerNames", "envNames", "hasOAuthClientSecret" };
    bool different = false;
    for (size_t i = 0; i < sizeof config / sizeof *config; i++) if (!json_equal(json_get(s->row, config[i]), json_get(row, config[i]))) different = true;
    json_free(s->row); s->row = json_clone(row);
    if (different) { if (s->dirty) s->conflict = true; else fill(s); }
}
static void adopt_id(McpForm *s, double id) {
    s->id = id; free(s->base.id); s->base.id = form_id(id); pane_set_selected_id(app_sidebar_pane(), s->base.id);
}
static void load(McpForm *s);
static void read_done(void *owner, Request *req) {
    McpForm *s = owner;
    if (!s->shown || !account_current(s) || req->client != s->account) return;
    const Json *rows = json_get(req->result, "servers"), *row = NULL;
    bool creating = !s->id && s->uncertain && s->create_name;
    bool listed = req->ok && json_is_array(rows);
    if (listed) for (size_t i = 0; i < json_count(rows); i++) {
        const Json *candidate = json_at(rows, i);
        if (mcp_server_id(candidate) && (creating ? str_eq(json_str(json_get(candidate, "name")), s->create_name) : mcp_server_id(candidate) == s->id)) row = candidate;
    }
    if (row) {
        bool visible_changed = creating;
        static const char *const visible[] = { "label", "transport", "status", "enabled" };
        for (size_t i = 0; i < sizeof visible / sizeof *visible; i++) if (!json_equal(json_get(s->row, visible[i]), json_get(row, visible[i]))) visible_changed = true;
        if (creating) adopt_id(s, mcp_server_id(row));
        status_update(s, row);
        if (visible_changed) settings_mcp_changed();
        if (s->uncertain || s->read_error) set_string(&s->error, creating ? "The created server was found. Your draft is preserved; review it before saving any further changes." : NULL);
        s->read_error = false;
        s->uncertain = false; s->create_retry = false;
    } else if (creating && listed) {
        s->read_error = false;
        s->create_retry = true;
        set_string(&s->error, "The submitted name is not listed yet; the create may still complete. Save retries with the original name. Your draft name is preserved for an update after the server is found.");
    }
    else {
        mcp_sign_in_failed(&s->sign_in);
        if (s->edits[F_CALLBACK]) set_text(s->edits[F_CALLBACK], "");
        s->read_error = !listed;
        if (listed) { s->uncertain = true; settings_mcp_changed(); set_string(&s->error, "This MCP server is no longer listed. Reopen it from Settings."); }
        else { char *e = xstrfmt("MCP status could not be refreshed (HTTP %d). Refresh or reconnect before finishing sign-in.", req->error.status); set_string(&s->error, e); free(e); }
    }
    poller_finished(&s->poll, !listed, req->error.retry_after); repaint(s);
}
static void load(McpForm *s) {
    if (!s->shown || (!s->id && !(s->uncertain && s->create_name)) || !account_current(s) || s->read || busy(s)) return;
    store_call("settings_mcp_servers", json_object(), 0, s, read_done, 0, &s->read);
}
static void write_done(void *owner, Request *req) {
    McpForm *s = owner;
    if (!s->shown || !account_current(s) || req->client != s->account) return;
    if (req->tag == ACT_FINISH) { set_text(s->edits[F_CALLBACK], ""); if (!req->ok) mcp_sign_in_failed(&s->sign_in); }
    const Json *row = json_get(req->result, "server");
    if (!req->ok || (req->tag != ACT_DELETE && (!mcp_server_id(row) || (s->id && mcp_server_id(row) != s->id)))) {
        char *e = req->ok ? xstrdup("Unexpected MCP response; refresh before trying again.") : xstrfmt("MCP action failed (HTTP %d). Refresh status; sign-in callbacks are single-use and must not be resubmitted automatically.", req->error.status);
        error_text(s, e); free(e);
        // Create may commit before a follow-up connection/status save returns an HTTP error.
        s->uncertain = s->uncertain || req->ok || request_outcome_unknown(req) || (req->tag == ACT_SAVE && !s->id);
        // Reads recover an unknown outcome; honor rate limits/unavailability before that read.
        if (req->error.status != 429 && req->error.status != 503) load(s);
        poller_finished(&s->poll, true, req->error.retry_after); return;
    }
    if (req->tag == ACT_DELETE) { s->dirty = false; settings_mcp_changed(); app_clear_detail(); return; }
    char *draft_name = req->tag == ACT_SAVE && !s->id ? edit_text(s->edits[F_NAME]) : NULL;
    adopt_id(s, mcp_server_id(row)); status_update(s, row); s->uncertain = false; s->create_retry = false; s->read_error = false; set_string(&s->error, NULL);
    if (req->tag == ACT_SAVE) {
        fill(s);
        if (draft_name && !str_eq(draft_name, json_str(json_get(row, "name")))) { set_text(s->edits[F_NAME], draft_name); changed(s); }
    }
    free(draft_name);
    settings_mcp_changed(); repaint(s);
    poller_finished(&s->poll, false, -1);
    if ((req->tag == ACT_SAVE || req->tag == ACT_CONNECT || req->tag == ACT_SIGN_IN) && s->sign_in.url && !s->sign_in.blocked) open_sign_in(s->sign_in.url);
}
static void write_call(McpForm *s, const char *operation, Json *args, int tag) {
    if (!s->shown || !account_current(s) || !mcp_settings_supported(operation)) { json_free(args); return; }
    if (tag == ACT_SAVE && !s->id) {
        if (s->uncertain && s->create_name) json_set_str(args, "name", s->create_name);
        else set_string(&s->create_name, json_str(json_get(args, "name")));
        s->create_retry = false;
    }
    request_cancel(&s->read); if (s->id) json_set_num(args, "id", s->id);
    store_call(operation, args, 60000, s, write_done, tag, &s->write); repaint(s);
}
static void form_save(McpForm *s) {
    if (!account_current(s) || !s->shown || !can_save(s) || !mcp_settings_supported(s->id ? "update_mcp_server" : "create_mcp_server")) return;
    char *why = NULL; Json *body = body_now(s, &why);
    if (!body) { error_text(s, why); free(why); return; }
    if (s->conflict && !app_confirm("Replace newer MCP configuration?", "This server changed on core while you were editing. Saving replaces its current configuration with your draft. Cancel to keep editing or reload the current fields.", "Replace", true)) { json_free(body); return; }
    write_call(s, s->id ? "update_mcp_server" : "create_mcp_server", body, ACT_SAVE);
}
static void toggle_repo(McpForm *s, const char *repo) {
    Json *next = json_array(); bool had = selected(s, repo);
    for (size_t i = 0; i < json_count(s->repos); i++) if (!str_eq(repo, json_str(json_at(s->repos, i)))) json_array_push(next, json_clone(json_at(s->repos, i)));
    if (!had) json_array_push(next, json_string(repo));
    json_free(s->repos); s->repos = next; changed(s); repaint(s);
}
static void form_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt; McpForm *s = (McpForm *)base;
    if (!s->shown || !account_current(s)) return;
    if (action == ACT_TAB && arg >= 0 && arg < 3) { s->tab = (int)arg; repaint(s); pane_scroll_to_top(base->pane); return; }
    if (action == ACT_FOCUS && arg >= 0 && arg < F_COUNT) { SetFocus(s->edits[arg]); return; }
    if (busy(s)) return;
    switch (action) {
    case ACT_SAVE: form_save(s); break;
    case ACT_RELOAD:
        if (s->conflict && app_confirm("Discard unsaved MCP changes?", "Reload the latest server fields and discard this draft, including replacement secrets.", "Reload", true)) { fill(s); repaint(s); }
        break;
    case ACT_DELETE:
        if (s->id && mcp_settings_supported("delete_mcp_server") && app_confirm("Delete MCP server?", "Assigned sessions will lose these tools on their next turn.", "Delete", true)) write_call(s, "delete_mcp_server", json_object(), ACT_DELETE);
        break;
    case ACT_TRANSPORT: s->stdio = !s->stdio; changed(s); repaint(s); break;
    case ACT_ENABLED: s->enabled = !s->enabled; changed(s); repaint(s); break;
    case ACT_ALL: s->all = !s->all; changed(s); repaint(s); break;
    case ACT_REDIRECT: s->loopback = !s->loopback; changed(s); repaint(s); break;
    case ACT_REPO: {
        const char *repo = arg < 0 ? json_str(json_at(s->repos, (size_t)(-arg - 1))) : json_str(json_get(json_at(settings_project_rows(), (size_t)arg), "repo"));
        if (repo) toggle_repo(s, repo);
        break;
    }
    case ACT_SECRET:
        if (arg >= 0 && arg < 3) { s->secrets[arg] = (McpSecretMode)((s->secrets[arg] + 1) % 3); changed(s); repaint(s); } break;
    case ACT_CONNECT: case ACT_SIGN_IN:
        if (s->id && !s->dirty && !s->uncertain && mcp_settings_supported("connect_mcp_server")) {
            Json *body = json_object(); json_set_bool(body, "signIn", action == ACT_SIGN_IN);
            write_call(s, "connect_mcp_server", body, action);
        } break;
    case ACT_BROWSER: if (!s->dirty && s->sign_in.url && !s->sign_in.blocked) open_sign_in(s->sign_in.url); break;
    case ACT_FINISH:
        if (s->id && !s->dirty && !s->uncertain && mcp_sign_in_can_finish(&s->sign_in) && mcp_settings_supported("finish_mcp_sign_in")) {
            char *url = edit_text(s->edits[F_CALLBACK]), *trim = str_trim(url); free(url);
            if (!mcp_callback_url(trim)) { free(trim); error_text(s, "Paste the complete callback URL with state and code (or provider error)."); break; }
            Json *body = json_object(); json_set_str(body, "url", trim); free(trim); write_call(s, "finish_mcp_sign_in", body, ACT_FINISH);
        } break;
    }
}
static void form_command(Screen *base, int id, int code, HWND control) {
    (void)control; McpForm *s = (McpForm *)base; int f = id - ID_FIELD;
    if (f >= 0 && f < F_CALLBACK && code == EN_CHANGE && !busy(s)) changed(s);
}
static void departed_write_done(void *owner, Request *req) {
    (void)owner;
    if (req->client && req->client == g_store.client && mcp_settings_supported("settings_mcp_servers")) settings_mcp_changed();
}
static void detach_write(McpForm *s) {
    if (!s->write) return;
    // The worker continues on core. Observe completion without retaining the form or its controls.
    s->write->owner = NULL; s->write->slot = NULL; s->write->done = departed_write_done;
    s->write = NULL; s->uncertain = true; s->create_retry = false; s->read_error = false;
    set_string(&s->error, "An action is still completing on core. Refresh status before repeating it.");
}
static void form_visible(Screen *base, bool shown) {
    McpForm *s = (McpForm *)base; s->shown = shown;
    if (shown && account_current(s)) { ensure_controls(s); poller_start(&s->poll, base->pane, TIMER_STATUS, 5000); load(s); }
    else {
        poller_stop(&s->poll); request_cancel(&s->read);
        detach_write(s); mcp_sign_in_clear(&s->sign_in);
        for (int f = 0; f < F_COUNT; f++) if (s->edits[f]) { if (f == F_CALLBACK) set_text(s->edits[f], ""); ShowWindow(s->edits[f], SW_HIDE); }
    }
}
static void form_timer(Screen *base, UINT id) {
    McpForm *s = (McpForm *)base;
    if (!account_current(s)) { form_visible(base, false); repaint(s); return; }
    if (poller_fired(&s->poll, id)) { if (busy(s) || (!s->id && !s->uncertain)) poller_finished(&s->poll, false, -1); else load(s); }
}
static void form_refresh(Screen *base) { McpForm *s = (McpForm *)base; request_cancel(&s->read); load(s); }
static bool form_leave(Screen *base) { McpForm *s = (McpForm *)base; return !s->dirty || app_confirm("Discard unsaved MCP changes?", "The edited server fields have not been saved.", "Discard", true); }
static void form_destroy(Screen *base) {
    McpForm *s = (McpForm *)base; poller_stop(&s->poll); request_cancel(&s->read); detach_write(s);
    for (int f = 0; f < F_COUNT; f++) if (s->edits[f]) DestroyWindow(s->edits[f]);
    json_free(s->row); json_free(s->repos); free(s->error); free(s->create_name); mcp_sign_in_clear(&s->sign_in); api_client_release(s->account); screen_release(base);
}
static const ScreenVTable VT = { .destroy = form_destroy, .layout = form_layout, .header = form_header, .action = form_action, .place = form_place, .visible = form_visible, .command = form_command, .timer = form_timer, .refresh = form_refresh, .can_leave = form_leave };
Screen *mcp_settings_screen_new(const Json *row, const Json *defaults) {
    McpForm *s = xcalloc(1, sizeof *s); s->base.vt = &VT; s->account = api_client_retain(g_store.client);
    s->row = json_is_object(row) ? json_clone(row) : json_is_object(defaults) ? json_clone(defaults) : json_object();
    s->id = mcp_server_id(s->row); s->base.id = form_id(s->id); fill(s);
    return &s->base;
}
