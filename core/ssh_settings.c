#include "ssh_settings.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

const char *const SSH_SECRET_KEYS[SSH_SECRET_COUNT] = {"password", "sudoPassword"};
const char *const SSH_SECRET_FLAGS[SSH_SECRET_COUNT] = {"hasPassword", "hasSudoPassword"};

void ssh_secret_free(char *text, size_t bytes) {
    if (text) { volatile char *p = text; while (bytes--) *p++ = 0; free(text); }
}
static bool has_key(const Json *row, const char *key) {
    for (size_t i = 0; i < json_count(row); i++) if (str_eq(json_key(row, i), key)) return true;
    return false;
}
void ssh_secrets_init(SshSecrets *s, const Json *row, const Json *rows, bool fresh) {
    memset(s, 0, sizeof *s);
    for (int f = 0; f < SSH_SECRET_COUNT; f++) {
        SshSecret *v = &s->fields[f];
        v->offered = has_key(row, SSH_SECRET_FLAGS[f]);
        v->stored = !fresh && json_bool_is(json_get(row, SSH_SECRET_FLAGS[f]), true);
        if (!fresh || v->offered || !json_is_array(rows)) continue;
        v->offered = !json_count(rows);
        for (size_t i = 0; i < json_count(rows); i++) v->offered |= has_key(json_at(rows, i), SSH_SECRET_FLAGS[f]);
    }
}
void ssh_secrets_free(SshSecrets *s) {
    for (int f = 0; f < SSH_SECRET_COUNT; f++) ssh_secret_free(s->fields[f].text, s->fields[f].bytes);
    memset(s, 0, sizeof *s);
}
void ssh_secrets_set(SshSecrets *s, int field, const char *text, size_t bytes) {
    if (field < 0 || field >= SSH_SECRET_COUNT) return;
    SshSecret *v = &s->fields[field];
    // Retain a draft after a response loses capability; it must never be sent to that server.
    char *copy = xstrndup(text ? text : "", text ? bytes : 0);
    ssh_secret_free(v->text, v->bytes); v->text = copy; v->bytes = text ? bytes : 0;
    if (v->bytes) v->remove = false;
    v->revision++;
}
void ssh_secrets_toggle(SshSecrets *s, int field) {
    if (field < 0 || field >= SSH_SECRET_COUNT || !s->fields[field].offered) return;
    SshSecret *v = &s->fields[field];
    bool remove = !v->remove;
    ssh_secrets_set(s, field, "", 0); v->remove = remove;
}
bool ssh_secrets_changed(const SshSecrets *s) {
    for (int f = 0; f < SSH_SECRET_COUNT; f++) if (s->fields[f].bytes || s->fields[f].remove) return true;
    return false;
}
bool ssh_secrets_body(const SshSecrets *s, Json *body, char **error) {
    for (int f = 0; f < SSH_SECRET_COUNT; f++) {
        json_object_remove(body, SSH_SECRET_KEYS[f]); json_object_remove(body, SSH_SECRET_FLAGS[f]);
    }
    for (int f = 0; f < SSH_SECRET_COUNT; f++) {
        const SshSecret *v = &s->fields[f];
        if (!v->offered) continue;
        if (v->remove) { json_set_str(body, SSH_SECRET_KEYS[f], ""); continue; }
        if (!v->bytes) continue;
        bool invalid = v->bytes > 1023 || memchr(v->text, 0, v->bytes) || memchr(v->text, '\r', v->bytes) || memchr(v->text, '\n', v->bytes);
        if (invalid) { *error = xstrdup("Passwords must be at most 1023 UTF-8 bytes without NUL or line breaks."); return false; }
        json_set_str(body, SSH_SECRET_KEYS[f], v->text);
    }
    return true;
}
bool ssh_secrets_saved(SshSecrets *s, const Json *row, const Json *sent, const unsigned long long revisions[SSH_SECRET_COUNT]) {
    bool confirmed = true;
    for (int f = 0; f < SSH_SECRET_COUNT; f++) {
        SshSecret *v = &s->fields[f];
        const char *value = json_str(json_get(sent, SSH_SECRET_KEYS[f]));
        v->offered = has_key(row, SSH_SECRET_FLAGS[f]);
        v->stored = json_bool_is(json_get(row, SSH_SECRET_FLAGS[f]), true);
        bool accepted = !value || json_bool_is(json_get(row, SSH_SECRET_FLAGS[f]), *value != 0);
        confirmed &= accepted;
        if (value && accepted && v->revision == revisions[f]) { ssh_secrets_set(s, f, "", 0); v->remove = false; }
    }
    return confirmed;
}
void ssh_secrets_clone(SshSecrets *copy, const SshSecrets *s) {
    memset(copy, 0, sizeof *copy);
    for (int f = 0; f < SSH_SECRET_COUNT; f++) {
        copy->fields[f].offered = s->fields[f].offered;
        if (s->fields[f].offered && s->fields[f].bytes) ssh_secrets_set(copy, f, s->fields[f].text, s->fields[f].bytes);
    }
}

void ssh_secrets_scrub(Json *body) {
    for (int f = 0; f < SSH_SECRET_COUNT; f++) {
        Json *v = (Json *)json_get(body, SSH_SECRET_KEYS[f]);
        if (v->type == JSON_STRING) { volatile char *p = v->s; size_t n = strlen(v->s); while (n--) *p++ = 0; }
    }
}
