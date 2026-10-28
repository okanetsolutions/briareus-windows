#include "screen_settings_internal.h"

static const char *const SSH_PICK_KEYS[SP_COUNT] = {"repo", "permissionMode"};

static int ssh_field_tab(int f) { return f >= S_DBHOST ? ST_DATABASE : ST_SERVER; }
static char *ssh_text(SshForm *s, int f) {
    if (f == S_DBUSER && !json_str(json_get(s->row, "dbUsername"))) return xstrdup(s->db_user ? s->db_user : "");
    return screen_settings_ssh_field_text(s->row, f);
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
        if (f == S_PASSWORD || f == S_SUDO_PASSWORD) continue;
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
    if (!ssh_secrets_body(&s->secrets, body, why)) { ssh_secrets_scrub(body); json_free(body); return NULL; }
    return body;
}
