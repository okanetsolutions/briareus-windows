#include "security_review.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

static char *nonempty(const Json *value) { return xstrdup(json_str_nonempty(value)); }
static int count_of(const Json *value) { int n = json_int_or(value, 0); return n > 0 ? n : 0; }
// An id the server may send as a number or a string, as text; NULL when neither.
static char *id_text(const Json *value) {
    double n;
    if (json_num(value, &n)) return xstrfmt("%.0f", n);
    return nonempty(value);
}
static void categories_parse(const Json *list, SecurityReview *out) {
    out->categories = xcalloc(json_count(list) + 1, sizeof *out->categories);
    for (size_t i = 0; i < json_count(list); i++) {
        const Json *c = json_at(list, i);
        const char *id = json_str_nonempty(json_get(c, "id"));
        if (!id) continue;
        SecurityCategory *category = &out->categories[out->category_count++];
        category->id = xstrdup(id); category->assessment = nonempty(json_get(c, "assessment"));
        category->applicable = json_bool_is(json_get(c, "applicable"), true);
        category->findings = count_of(json_get(c, "findings"));
    }
}
static void entries_parse(const Json *list, SecurityReview *out) {
    out->entries = xcalloc(json_count(list) + 1, sizeof *out->entries);
    for (size_t i = 0; i < json_count(list); i++) {
        const Json *e = json_at(list, i);
        const char *title = json_str_nonempty(json_get(e, "title"));
        if (!title) continue;
        SecurityEntry *entry = &out->entries[out->entry_count++];
        entry->id = id_text(json_get(e, "id")); entry->title = xstrdup(title);
        entry->category = nonempty(json_get(e, "category")); entry->severity = nonempty(json_get(e, "severity"));
        entry->file_path = nonempty(json_get(e, "filePath")); entry->claim = nonempty(json_get(e, "claim"));
        entry->start_line = count_of(json_get(e, "startLine"));
    }
}
static void claims_parse(const Json *list, SecurityReview *out) {
    out->claims = xcalloc(json_count(list) + 1, sizeof *out->claims);
    for (size_t i = 0; i < json_count(list); i++) {
        const Json *c = json_at(list, i);
        const char *ref = json_str_nonempty(json_get(c, "findingRef"));
        if (!ref) continue;
        SecurityClaim *claim = &out->claims[out->claim_count++];
        claim->finding_ref = xstrdup(ref); claim->status = nonempty(json_get(c, "status")); claim->evidence = nonempty(json_get(c, "evidence"));
    }
}
// `widened` in words: a sentence, or a list of what it took in.
static char *widened_text(const Json *value) {
    if (json_str_nonempty(value)) return xstrdup(json_str(value));
    if (!json_is_array(value) || !json_count(value)) return NULL;
    Str text; str_init(&text);
    for (size_t i = 0; i < json_count(value); i++) {
        const char *part = json_str_nonempty(json_at(value, i));
        if (part) str_appendf(&text, "%s%s", text.len ? ", " : "", part);
    }
    if (text.len) return str_detach(&text);
    str_free(&text);
    return NULL;
}
static void coverage_parse(const Json *coverage, SecurityReview *out) {
    if (!json_is_object(coverage)) return;
    out->has_coverage = true;
    out->recorded = count_of(json_get(coverage, "findings"));
    categories_parse(json_get(coverage, "categories"), out);
    entries_parse(json_get(coverage, "entries"), out);
    claims_parse(json_get(coverage, "claims"), out);
    out->widened = widened_text(json_get(coverage, "widened"));
}
static void counts_parse(const Json *counts, SecurityCounts *out) {
    if (!json_is_object(counts)) return;
    out->present = true;
    out->findings = count_of(json_get(counts, "findings")); out->fix = count_of(json_get(counts, "fix"));
    out->optional = count_of(json_get(counts, "optional")); out->dismissed = count_of(json_get(counts, "dismissed"));
    out->suppressed = count_of(json_get(counts, "suppressed")); out->unverified = count_of(json_get(counts, "unverified"));
}
bool security_review_parse(const Json *review, SecurityReview *out) {
    memset(out, 0, sizeof *out);
    const Json *j = json_get(review, "security");
    review_security_parse(j, &out->result);
    out->stage = nonempty(json_get(j, "stage"));
    // A policy that ran no pass has nothing of its own to show.
    if (!out->result.present || (str_eq(out->result.result, "not_required") && !out->stage)) { security_review_free(out); return false; }
    out->required_by_risk = nonempty(json_get(j, "requiredByRisk"));
    out->stale = json_bool_is(json_get(j, "stale"), true);
    coverage_parse(json_get(j, "coverage"), out);
    counts_parse(json_get(j, "counts"), &out->counts);
    return true;
}
void security_review_free(SecurityReview *s) {
    if (!s) return;
    review_security_free(&s->result);
    free(s->stage); free(s->required_by_risk); free(s->widened);
    for (size_t i = 0; i < s->category_count; i++) { free(s->categories[i].id); free(s->categories[i].assessment); }
    for (size_t i = 0; i < s->entry_count; i++) {
        SecurityEntry *e = &s->entries[i];
        free(e->id); free(e->category); free(e->severity); free(e->title); free(e->file_path); free(e->claim);
    }
    for (size_t i = 0; i < s->claim_count; i++) { free(s->claims[i].finding_ref); free(s->claims[i].status); free(s->claims[i].evidence); }
    free(s->categories); free(s->entries); free(s->claims);
    memset(s, 0, sizeof *s);
}

char *security_category_title(const char *id) {
    static const char *const known[][2] = {
        {"permissions", "Permissions"}, {"secrets", "Secrets"}, {"isolation", "Isolation"},
        {"execution", "Execution"}, {"untrusted_input", "Untrusted input"}, {"dependencies", "Dependencies"},
    };
    for (size_t i = 0; i < sizeof known / sizeof *known; i++) if (str_eq(id, known[i][0])) return xstrdup(known[i][1]);
    return str_replace(id ? id : "", "_", " ");
}
const char *security_review_stage_line(const SecurityReview *s) {
    if (!s->stage) return "No security pass ran in this review";
    if (str_eq(s->stage, "completed")) return "Security pass completed";
    if (str_eq(s->stage, "failed")) return "Security pass failed";
    if (str_eq(s->stage, "running")) return "Security pass running";
    return str_eq(s->stage, "pending") ? "Security pass not started" : "Security pass";
}
static void count_part(Str *text, int n, const char *what) {
    if (n > 0) str_appendf(text, "%s%d %s", text->len ? " \xC2\xB7 " : "", n, what);
}
char *security_review_counts_line(const SecurityReview *s) {
    const SecurityCounts *c = &s->counts;
    if (!c->present) return NULL;
    Str text; str_init(&text);
    str_appendf(&text, "%d security finding%s", c->findings, c->findings == 1 ? "" : "s");
    count_part(&text, c->fix, "to fix"); count_part(&text, c->optional, "optional"); count_part(&text, c->dismissed, "dismissed");
    count_part(&text, c->suppressed, "suppressed"); count_part(&text, c->unverified, "unverified");
    return str_detach(&text);
}
char *security_review_risk_line(const SecurityReview *s) {
    return s->required_by_risk ? xstrfmt("Required by the %s risk tier", s->required_by_risk) : NULL;
}
char *security_entry_line(const SecurityEntry *e) {
    Str text; str_init(&text);
    if (e->severity) str_appendf(&text, "%s \xC2\xB7 ", e->severity);
    str_appendz(&text, e->title);
    if (e->file_path) {
        str_appendf(&text, " \xC2\xB7 %s", e->file_path);
        if (e->start_line) str_appendf(&text, ":%d", e->start_line);
    }
    if (e->category) {
        char *category = security_category_title(e->category);
        str_appendf(&text, " \xC2\xB7 %s", category); free(category);
    }
    return str_detach(&text);
}
char *security_claim_line(const SecurityClaim *c) {
    Str text; str_init(&text);
    str_appendf(&text, "%s: %s", c->finding_ref, c->status ? c->status : "unchecked");
    if (c->evidence) str_appendf(&text, " \xC2\xB7 %s", c->evidence);
    return str_detach(&text);
}
