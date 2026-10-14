#include "project_sftp_internal.h"

static void sessions_changed(void *ctx);
static void rows_done(void *ctx, Request *req);

static ProjectSftp *g_instances[MAX_INSTANCES];

bool project_sftp_offered(void) { return store_supports("settings_ssh_servers"); }

char *project_sftp_server_key(const Json *row) {
    double id;
    if (json_num(json_get(row, "id"), &id) && isfinite(id)) return xstrfmt("sftp:%.0f", id);
    const char *host = json_str(json_get(row, "host"));
    return xstrfmt("sftp:%s", host ? host : "");
}

char *project_sftp_server_name(const Json *row) {
    const char *label = json_str_nonempty(json_get(row, "label"));
    if (label) return xstrdup(label);
    const char *user = json_str(json_get(row, "username")), *host = json_str(json_get(row, "host"));
    return user && *user ? xstrfmt("%s@%s", user, host ? host : "") : xstrdup(host ? host : "SSH server");
}

int project_sftp_server_port(const Json *row) { int port = json_int_or(json_get(row, "port"), 22); return port > 0 ? port : 22; }

/// The project's session on show: the active one when it is the project's, else its first.
SftpSession *project_sftp_shown_session(ProjectSftp *p) {
    SftpSession *active = sftp_active();
    if (active && str_eq(sftp_group(active), p->repo)) return active;
    for (size_t i = 0; i < sftp_count(); i++) if (str_eq(sftp_group(sftp_at(i)), p->repo)) return sftp_at(i);
    return NULL;
}

size_t project_sftp_session_count(const char *repo) {
    size_t n = 0;
    for (size_t i = 0; i < sftp_count(); i++) if (str_eq(sftp_group(sftp_at(i)), repo)) n++;
    return n;
}

HWND project_sftp_owner(ProjectSftp *p) { return p->host->pane ? pane_hwnd(p->host->pane) : NULL; }

void project_sftp_relayout(ProjectSftp *p) {
    if (!p->host->pane) return;
    pane_relayout(p->host->pane);
    pane_header_changed(p->host->pane);
}

static void sessions_changed(void *ctx) { project_sftp_relayout(ctx); }

// MARK: - Loading

static void rows_done(void *ctx, Request *req) {
    ProjectSftp *p = ctx;
    p->loaded = true; p->loading = false;
    if (!req->ok) request_error_into(&p->error, req);
    else {
        set_string(&p->error, NULL);
        json_free(p->rows); p->rows = json_array();
        const Json *all = json_get(req->result, "servers");
        for (size_t i = 0; i < json_count(all); i++)
            if (str_eq(json_str(json_get(json_at(all, i), "repo")), p->repo)) json_array_push(p->rows, json_clone(json_at(all, i)));
    }
    project_sftp_relayout(p);
}

void project_sftp_load(ProjectSftp *p) {
    if (p->loaded || p->req) return;
    if (!project_sftp_offered()) { p->loaded = true; return; }
    p->loading = true;
    store_call("settings_ssh_servers", json_object(), 0, p, rows_done, 0, &p->req);
}

void project_sftp_refresh(ProjectSftp *p) {
    request_cancel(&p->req);
    p->loaded = false;
    project_sftp_load(p);
    // ⟳ also reads the folder on show again.
    SftpSession *s = project_sftp_shown_session(p);
    if (s && sftp_state(s) == SFTP_READY) {
        const char *sel = sftp_selected(s);
        SftpNode *n = sftp_node(s, sel);
        char *dir = n && (n->dir || n->link) ? xstrdup(n->path) : sel ? sftp_parent(sel) : NULL;
        if (dir) sftp_list(s, dir);
        free(dir);
    }
}

void servers_sftp_changed(void) {
    for (int i = 0; i < MAX_INSTANCES; i++) if (g_instances[i] && (g_instances[i]->loaded || g_instances[i]->req)) project_sftp_refresh(g_instances[i]);
}

// MARK: - Connecting

void project_sftp_connect_row(ProjectSftp *p, const Json *row) {
    if (!json_is_object(row)) return;
    char *key = project_sftp_server_key(row), *name = project_sftp_server_name(row);
    SftpSession *open = sftp_find(key);
    if (open) sftp_set_active(open);
    else {
        TermTarget target = { key, p->repo, name, json_str(json_get(row, "username")), json_str(json_get(row, "host")), project_sftp_server_port(row) };
        char *error = NULL;
        if (!sftp_open(&target, &error)) {
            char *title = xstrfmt("Could not connect to %s", name);
            app_alert(title, error ? error : "The session could not start.");
            free(title);
        }
        free(error);
    }
    free(key); free(name);
    project_sftp_relayout(p);
}

void project_sftp_close_session(SftpSession *s) {
    if (!s) return;
    size_t queued = 0;
    const char *busy = sftp_busy(s, &queued);
    if (sftp_state(s) != SFTP_CLOSED && busy) {
        char *title = xstrfmt("Disconnect from %s?", sftp_label(s));
        bool ok = app_confirm(title, "A transfer is still running; disconnecting stops it, and the transfers waiting behind it are dropped.", "Disconnect", true);
        free(title);
        if (!ok) return;
    }
    sftp_close(s);
}

void project_sftp_header(ProjectSftp *p, HeaderInfo *info) {
    SftpSession *s = project_sftp_shown_session(p);
    if (!s) return;
    SftpState state = sftp_state(s);
    const char *where = sftp_selected(s);
    snprintf(info->subtitle, sizeof info->subtitle, "SFTP %s \xC2\xB7 %s%s%s", sftp_target(s),
             state == SFTP_READY ? "connected" : state == SFTP_CONNECTING ? "connecting" : "closed", where ? " \xC2\xB7 " : "", where ? where : "");
    snprintf(info->status, sizeof info->status, "%s", state == SFTP_CLOSED ? "closed" : "idle");
    bool ready = state == SFTP_READY;
    SftpNode *sel = sftp_node(s, sftp_selected(s));
    HeaderButton *b = &info->buttons[info->button_count++];
    b->glyph = 0xE898; b->action = p->base + A_UPLOAD; b->enabled = ready; b->prominent = true; b->tip = "Upload files into the selected folder";
    snprintf(b->label, sizeof b->label, "Upload");
    b = &info->buttons[info->button_count++];
    b->glyph = 0xE896; b->action = p->base + A_DOWNLOAD; b->enabled = ready && sel && !str_eq(sel->path, "/"); b->tip = "Download the selected file or folder";
    snprintf(b->label, sizeof b->label, "Download");
    b = &info->buttons[info->button_count++];
    b->glyph = 0xE8F4; b->action = p->base + A_MKDIR; b->enabled = ready; b->tip = "New folder in the selected folder";
    b = &info->buttons[info->button_count++];
    b->glyph = 0xE80F; b->action = p->base + A_HOME; b->enabled = ready && sftp_home(s); b->tip = "Go to the folder the server starts in";
    if (state == SFTP_CLOSED) {
        b = &info->buttons[info->button_count++];
        b->glyph = 0xE72C; b->action = p->base + A_RECONNECT; b->enabled = true; b->tip = "Connect this session again";
        snprintf(b->label, sizeof b->label, "Reconnect");
    }
    b = &info->buttons[info->button_count++];
    b->glyph = 0xE711; b->action = p->base + A_CLOSE; b->enabled = true; b->tip = "Close this session";
}

void project_sftp_place(ProjectSftp *p, const RECT *content, int scroll_y, bool shown) {
    (void)content; (void)scroll_y;
    project_sftp_set_dropping(p, shown && project_sftp_offered());
}

// MARK: - Lifetime

ProjectSftp *project_sftp_new(const char *repo, Screen *host, int action_base) {
    ProjectSftp *p = xcalloc(1, sizeof *p);
    p->repo = xstrdup(repo ? repo : "");
    p->host = host; p->base = action_base;
    p->rows = json_array();
    for (int i = 0; i < MAX_INSTANCES; i++) if (!g_instances[i]) { g_instances[i] = p; break; }
    sftp_add_listener(sessions_changed, p);
    return p;
}

void project_sftp_free(ProjectSftp *p) {
    if (!p) return;
    for (int i = 0; i < MAX_INSTANCES; i++) if (g_instances[i] == p) g_instances[i] = NULL;
    sftp_remove_listener(sessions_changed, p);
    project_sftp_set_dropping(p, false);
    // The sessions go on, for the tab to come back to.
    request_cancel(&p->req);
    json_free(p->rows);
    project_sftp_forget_rows(p);
    free(p->repo); free(p->error); free(p->last_click);
    free(p);
}
