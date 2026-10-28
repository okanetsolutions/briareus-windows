#include "mail_wake.h"
#include "str.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

const char *const mail_wake_cap_keys[] = {"perHour", "maxTurns"};
const char *const mail_wake_cap_labels[] = {"Wakes an hour", "Turns in a row"};
const char *const mail_wake_filter_keys[] = {"to", "from", "subject"};
const char *const mail_wake_filter_labels[] = {"To or Cc", "From", "Subject contains"};

void mail_wake_free(MailWake *w) {
    json_free(w->rules); json_free(w->saved_rules);
    for (int c = 0; c < MAIL_WAKE_CAPS; c++) { free(w->caps[c]); free(w->saved_caps[c]); }
    memset(w, 0, sizeof *w);
}
static void baseline(MailWake *w) {
    json_free(w->saved_rules); w->saved_rules = json_clone(w->rules);
    for (int c = 0; c < MAIL_WAKE_CAPS; c++) { free(w->saved_caps[c]); w->saved_caps[c] = xstrdup(w->caps[c]); }
}
bool mail_wake_dirty(const MailWake *w) {
    if (!json_equal(w->rules, w->saved_rules)) return true;
    for (int c = 0; c < MAIL_WAKE_CAPS; c++) if (!str_eq(w->caps[c], w->saved_caps[c])) return true;
    return false;
}
void mail_wake_discard(MailWake *w) {
    json_free(w->rules); w->rules = json_clone(w->saved_rules);
    for (int c = 0; c < MAIL_WAKE_CAPS; c++) { free(w->caps[c]); w->caps[c] = xstrdup(w->saved_caps[c]); }
}
static bool rule_readable(const Json *rule) {
    if (!json_is_object(rule) || json_int_or(json_get(rule, "account"), 0) < 1) return false;
    const Json *id = json_get(rule, "id"), *enabled = json_get(rule, "enabled");
    if (!json_is_null(id) && !json_str_nonempty(id)) return false;
    if (!json_is_null(enabled) && json_bool_tristate(enabled) < 0) return false;
    for (int f = 0; f < MAIL_WAKE_FILTERS; f++) {
        const Json *value = json_get(rule, mail_wake_filter_keys[f]);
        if (!json_is_null(value) && !json_str(value)) return false;
    }
    return true;
}
bool mail_wake_read(MailWake *w, const Json *record) {
    if (!json_is_object(record)) return false;
    const Json *rules = json_get(record, "rules");
    if (!json_is_null(rules) && !json_is_array(rules)) return false;
    if (json_count(rules) > MAIL_WAKE_LIMIT) return false;
    MailWake next = {0}; next.rules = json_array();
    for (size_t i = 0; i < json_count(rules); i++) {
        const Json *r = json_at(rules, i);
        if (!rule_readable(r)) { mail_wake_free(&next); return false; }
        Json *rule = json_clone(r);
        if (json_is_null(json_get(rule, "enabled"))) json_set_bool(rule, "enabled", true);
        for (int f = 0; f < MAIL_WAKE_FILTERS; f++) {
            if (json_is_null(json_get(rule, mail_wake_filter_keys[f]))) json_set_str(rule, mail_wake_filter_keys[f], "");
        }
        json_array_push(next.rules, rule);
    }
    for (int c = 0; c < MAIL_WAKE_CAPS; c++) {
        const Json *value = json_get(record, mail_wake_cap_keys[c]);
        int n = json_int_or(value, -1);
        if (!json_is_null(value) && (value->type != JSON_NUMBER || n < 0)) { mail_wake_free(&next); return false; }
        next.caps[c] = xstrfmt("%d", json_is_null(value) ? (c ? 10 : 30) : n);
    }
    baseline(&next); mail_wake_free(w); *w = next; return true;
}
bool mail_wake_active(const Json *record) {
    const Json *rules = json_get(record, "rules");
    if (!json_is_array(rules)) return false;
    for (size_t i = 0; i < json_count(rules); i++) {
        const Json *r = json_at(rules, i);
        if (json_is_object(r) && !json_bool_is(json_get(r, "enabled"), false)) return true;
    }
    return false;
}
bool mail_wake_mailbox_usable(const MailAccounts *accounts, int id) {
    const MailAccount *a = mail_account_find(accounts, id);
    return a && a->enabled && str_eq(a->status, "connected") && mail_provider_available(accounts, a->provider);
}
int mail_wake_tools(const Json *projects, const char *repo) {
    const Json *rows = json_is_array(projects) ? projects : json_get(projects, "projects");
    if (!repo || !json_is_array(rows)) return -1;
    for (size_t i = 0; i < json_count(rows); i++) {
        const Json *row = json_at(rows, i);
        if (str_eq(json_str(json_get(row, "repo")), repo)) return json_bool_tristate(json_get(row, "mailToolsEnabled"));
    }
    return -1;
}
bool mail_wake_add(MailWake *w, int account) {
    if (!w->rules || json_count(w->rules) >= MAIL_WAKE_LIMIT) return false;
    Json *r = json_object(); json_set_num(r, "account", account); json_set_bool(r, "enabled", true);
    for (int f = 0; f < MAIL_WAKE_FILTERS; f++) json_set_str(r, mail_wake_filter_keys[f], "");
    json_array_push(w->rules, r); return true;
}
void mail_wake_remove(MailWake *w, size_t row) {
    if (!w->rules || row >= json_count(w->rules)) return;
    Json *next = json_array();
    for (size_t i = 0; i < json_count(w->rules); i++) if (i != row) json_array_push(next, json_clone(json_at(w->rules, i)));
    json_free(w->rules); w->rules = next;
}
// ECMAScript trim and UTF-16 length match the server, including supplementary characters.
static bool js_space(unsigned c) {
    if (c >= 9 && c <= 13) return true;
    if (c >= 0x2000 && c <= 0x200a) return true;
    return c == 32 || c == 0xa0 || c == 0x1680 || c == 0x2028 || c == 0x2029
        || c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff;
}
static bool codepoint(const unsigned char **p, unsigned *value) {
    unsigned c = *(*p)++, min = 0; int extra = 0;
    if (c >= 0xf0 && c <= 0xf4) { c &= 7; extra = 3; min = 0x10000; }
    else if (c >= 0xe0 && c <= 0xef) { c &= 15; extra = 2; min = 0x800; }
    else if (c >= 0xc2 && c <= 0xdf) { c &= 31; extra = 1; min = 0x80; }
    else if (c >= 0x80) return false;
    for (int i = 0; i < extra; i++) {
        if ((**p & 0xc0) != 0x80) return false;
        c = (c << 6) | (*(*p)++ & 63);
    }
    if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
    *value = c; return true;
}
static char *filter_text(const char *text, bool address) {
    const unsigned char *p = (const unsigned char *)text, *start = NULL, *end = p;
    size_t units = 0, end_units = 0; bool whitespace = false, internal_space = false;
    while (*p) {
        const unsigned char *before = p; unsigned c;
        if (!codepoint(&p, &c)) return NULL;
        bool space = js_space(c);
        if (!start && !space) start = before;
        if (!start) continue;
        units += c > 0xffff ? 2 : 1;
        if (!space) { end = p; end_units = units; if (whitespace) internal_space = true; }
        else whitespace = true;
    }
    if (end_units > 320 || (address && internal_space)) return NULL;
    char *out = start ? xstrndup((const char *)start, (size_t)(end - start)) : xstrdup("");
    if (!address || !*out) return out;
    char *at = strchr(out, '@');
    char *dot = at && at[1] ? strchr(at + 2, '.') : NULL;
    while (dot && !dot[1]) dot = strchr(dot + 1, '.');
    if (at && at != out && !strchr(at + 1, '@') && dot) return out;
    free(out); return NULL;
}
Json *mail_wake_body(const MailWake *w, const MailAccounts *accounts, int tools, char **error) {
    *error = NULL;
    if (!json_is_array(w->rules) || json_count(w->rules) > MAIL_WAKE_LIMIT) { *error = xstrdup("A session keeps at most 20 rules."); return NULL; }
    Json *out = json_object(), *rules = json_array(); json_object_set(out, "rules", rules);
    for (size_t i = 0; i < json_count(w->rules); i++) {
        const Json *r = json_at(w->rules, i); int account = json_int_or(json_get(r, "account"), 0);
        if (!rule_readable(r)) { *error = xstrfmt("Rule %u needs a mailbox and valid fields.", (unsigned)i + 1); break; }
        bool enabled = !json_bool_is(json_get(r, "enabled"), false);
        if (enabled && tools != 1) { *error = xstrdup("Mail tools must be enabled in this project's settings before enabling rules."); break; }
        if (enabled && (!accounts || !mail_wake_mailbox_usable(accounts, account))) {
            *error = xstrfmt("Rule %u needs a connected, enabled mailbox with an available provider; choose another or turn the rule off.", (unsigned)i + 1); break;
        }
        Json *rule = json_object(); json_array_push(rules, rule);
        const char *id = json_str_nonempty(json_get(r, "id")); json_set_str(rule, "id", id);
        for (size_t j = 0; id && j < i; j++) {
            if (str_eq(id, json_str(json_get(json_at(w->rules, j), "id")))) { *error = xstrdup("Duplicate rule id; refresh before saving."); break; }
        }
        if (*error) break;
        json_set_num(rule, "account", account); json_set_bool(rule, "enabled", enabled);
        for (int f = 0; f < MAIL_WAKE_FILTERS; f++) {
            char *text = filter_text(json_str_or(json_get(r, mail_wake_filter_keys[f]), ""), f != 2);
            if (!text) { *error = xstrfmt("Rule %u: %s must be %s, at most 320 UTF-16 characters.", (unsigned)i + 1, mail_wake_filter_labels[f], f == 2 ? "valid text" : "one email address or empty"); break; }
            json_set_str(rule, mail_wake_filter_keys[f], text); free(text);
        }
        if (*error) break;
    }
    for (int c = 0; !*error && c < MAIL_WAKE_CAPS; c++) {
        char *text = str_trim(w->caps[c] ? w->caps[c] : ""), *end = NULL;
        errno = 0; long n = strtol(text, &end, 10); int max = c ? 1000 : 600;
        bool ok = *text && !*end && errno != ERANGE && n >= 1 && n <= max;
        free(text);
        if (!ok) *error = xstrfmt("%s must be a whole number from 1 to %d.", mail_wake_cap_labels[c], max);
        else json_set_num(out, mail_wake_cap_keys[c], (double)n);
    }
    if (*error) { json_free(out); return NULL; }
    return out;
}

bool mail_wake_saved_matches(const MailWake *saved, const Json *sent) {
    if (json_count(saved->rules) != json_count(json_get(sent, "rules"))) return false;
    for (int c = 0; c < MAIL_WAKE_CAPS; c++) {
        if (atoi(saved->caps[c]) != json_int_or(json_get(sent, mail_wake_cap_keys[c]), -1)) return false;
    }
    for (size_t i = 0; i < json_count(saved->rules); i++) {
        const Json *a = json_at(saved->rules, i), *b = json_at(json_get(sent, "rules"), i);
        for (size_t k = 0; k < json_count(b); k++) {
            const char *key = json_key(b, k); if (!json_equal(json_get(a, key), json_get(b, key))) return false;
        }
    }
    return true;
}
